/*
 * app_storage.c —— SD 卡存储服务实现(SDMMC 4-bit + FATFS)
 *
 * 硬件: 底板 TF 卡槽 slot0(CLK=43 CMD=44 D0..D3=39..42, Kaleido 26 号
 * 调研; 与 C5 WiFi 的 slot1 独立并发)。挂载失败(无卡/格式异常)只降级
 * 不阻塞启动: 各消费方(m soul_mem/face/wake/voice)按 ok() 回退。
 */
#include "app_storage.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "cubecore.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CUBE_ENABLE_SD

#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "board_pins.h"

static const char *TAG = "storage";

#define SD_MOUNT_POINT  "/sdcard"
#define SD_MAX_FILES    8

static sdmmc_card_t *s_card;
static bool s_ok;

static const char *s_dirs[] = {
    SD_MOUNT_POINT, "/mem", "/mem/raw", "/face", "/wake",
    "/voice", "/etc", "/logs",
};

static void sd_mount_task(void *arg)
{
    (void)arg;
    app_storage_mount();
    vTaskDelete(NULL);
}

/* WiFi 就绪事件 → 挂载(一次性)。
 * ⚠ 不能在 WiFi 前挂载(板上实测 2026-11-12): esp_hosted 初始化时
 * 要分配 15872B 内部 DMA 聚合缓冲, SPI/SDMMC DMA 先占内部内存会让
 * hosted "cannot allocate" assert 重启循环 —— hosted 必须先挑内存。 */
static void storage_evt_cb(cube_evt_id_t id, const void *data, size_t len,
                           void *ctx)
{
    (void)data; (void)len; (void)ctx;
    if (id == CUBE_EVT_WIFI_CONNECTED) {
        cube_event_unsubscribe(storage_evt_cb);
        xTaskCreate(sd_mount_task, "sdmount", 4096, NULL, 5, NULL);
    }
}

esp_err_t app_storage_mount(void)
{
    static bool tried;
    if (tried) return s_ok ? ESP_OK : ESP_FAIL;
    tried = true;

    /* ⚠ 必须走 SDSPI(SPI2), 不能走 SDMMC —— 板上实测(2026-11-12):
     * 1) SDMMC 默认 slot1 = C5 WiFi(esp_hosted), SD 误占 → hosted
     *    "could not create sdio handle" assert 重启循环;
     * 2) 改 slot0 后 hosted 版本握手过了但 probe 仍 assert —— P4 两个
     *    SDMMC 槽共享同一控制器时钟域, SD 卡时序协商破坏 C5 SDIO 链路
     *    (Kaleido "TF/WiFi 可并存"只是引脚级推断, 未实机验证)。
     * SPI 外设与 SDMMC 控制器物理独立, 彻底解耦; 1-bit 吞吐 ~1MB/s,
     * 记忆/素材频度绰绰有余。卡座引脚 → SPI 映射(SPI 模式标准脚位):
     * CLK=43 SCK, CMD=44 MOSI, D0=39 MISO, D3=42 CS(SPI 模式卡 D3 即 CS)。 */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_PIN_TF_CMD,
        .miso_io_num = BOARD_PIN_TF_D0,
        .sclk_io_num = BOARD_PIN_TF_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    /* SDMMC slot0(4-bit): 卡在 SDMMC 模式通信实测正常(第一版即挂载成功);
     * 早期 probe assert 的真因 = 内部 DMA 被挤压(86KB 静态数组/次序),
     * 现已根治(net 先行分配 + 记忆数组挪 PSRAM), 恢复 4-bit 全带宽。
     * slot1 = C5 WiFi(esp_hosted), 两槽引脚独立; 若后续实测仍互相
     * 干扰再回退 SDSPI 方案(见 git 历史)。 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    sdmmc_slot_config_t slot = {
        .clk = BOARD_PIN_TF_CLK,
        .cmd = BOARD_PIN_TF_CMD,
        .d0 = BOARD_PIN_TF_D0,
        .d1 = BOARD_PIN_TF_D1,
        .d2 = BOARD_PIN_TF_D2,
        .d3 = BOARD_PIN_TF_D3,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
    };
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,        /* 绝不格式化用户卡 */
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                            &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sd mount failed: %s(无卡? 记忆回退 NVS)",
                 esp_err_to_name(err));
        return err;
    }
    /* 目录骨架(存在即跳过) */
    for (unsigned i = 1; i < sizeof(s_dirs) / sizeof(s_dirs[0]); i++) {
        mkdir(s_dirs[i], 0775);
    }
    uint64_t cap_mb = ((uint64_t)s_card->csd.capacity) *
                      s_card->csd.sector_size / (1024 * 1024);
    ESP_LOGI(TAG, "sd up: %s %lluMB %s",
             s_card->cid.name, (unsigned long long)cap_mb,
             s_card->is_sdio ? "sdio" : "sd");
    s_ok = true;
    cube_event_publish(CUBE_EVT_SD_READY, NULL, 0);
    return ESP_OK;
}

esp_err_t app_storage_init(void)
{
    /* 只订阅; 真正挂载在 WiFi 就绪后(见 storage_evt_cb 注释) */
    return cube_event_subscribe(storage_evt_cb, NULL);
}

bool app_storage_ok(void)
{
    return s_ok;
}

int app_storage_path(char *buf, size_t cap, const char *rel)
{
    return snprintf(buf, cap, "%s/%s", SD_MOUNT_POINT, rel);
}

char *app_storage_read(const char *rel, size_t *len)
{
    if (!s_ok) return NULL;
    char path[96];
    app_storage_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 8 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    if (len) *len = (size_t)n;
    return buf;
}

esp_err_t app_storage_write(const char *rel, const void *data, size_t len)
{
    if (!s_ok) return ESP_ERR_INVALID_STATE;
    char path[96], tmp[104];
    app_storage_path(path, sizeof(path), rel);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) return ESP_FAIL;
    size_t wr = fwrite(data, 1, len, f);
    fclose(f);
    if (wr != len) {
        remove(tmp);
        return ESP_FAIL;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_storage_append(const char *rel, const char *line)
{
    if (!s_ok) return ESP_ERR_INVALID_STATE;
    char path[96];
    app_storage_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "ab");
    if (f == NULL) return ESP_FAIL;
    size_t wr = fwrite(line, 1, strlen(line), f);
    fputc('\n', f);
    fclose(f);
    return wr == strlen(line) ? ESP_OK : ESP_FAIL;
}

#else /* SD off */

esp_err_t app_storage_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool app_storage_ok(void) { return false; }
int app_storage_path(char *b, size_t c, const char *r)
{ return snprintf(b, c, "/sdcard/%s", r); }
char *app_storage_read(const char *r, size_t *l) { (void)r; if (l) *l = 0; return NULL; }
esp_err_t app_storage_write(const char *r, const void *d, size_t l)
{ (void)r; (void)d; (void)l; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_storage_append(const char *r, const char *l)
{ (void)r; (void)l; return ESP_ERR_NOT_SUPPORTED; }

#endif
