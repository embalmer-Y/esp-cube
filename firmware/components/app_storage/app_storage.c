/*
 * app_storage.c —— SD 卡存储服务实现(SDMMC 4-bit + FATFS)
 *
 * 硬件: 底板 TF 卡槽 slot0(CLK=43 CMD=44 D0..D3=39..42, Kaleido 26 号
 * 调研; 与 C5 WiFi 的 slot1 独立并发)。挂载失败(无卡/格式异常)只降级
 * 不阻塞启动: 各消费方(m soul_mem/face/wake/voice)按 ok() 回退。
 */
#include "app_storage.h"
#include "store_crypt.h"

#include <stdio.h>
#include <errno.h>
#include "esp_heap_caps.h"
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
#include "esp_rom_sys.h"
#include "board_pins.h"

static const char *TAG = "storage";

#define SD_MOUNT_POINT  "/sdcard"
#define SD_MAX_FILES    8

static sdmmc_card_t *s_card;
static bool s_ok;
static bool s_spi_inited;          /* SPI2 bus 由本模块管(失败/卸载要 free) */
static int  s_mode = -1;           /* 当前(尝试)挂载模式: 0=MMC 1=SDSPI */

/* ⚠ 必须带 /sdcard 前缀: 初版 mkdir("/mem") 建到了 VFS 根(flash)而非
 * SD 卡 —— 挂载成功但所有子目录路径 fopen 全失败, episodes 写入
 * 静默丢失(2026-11-12 台调: probe 文件写读 ESP_FAIL 定位) */
static const char *s_dirs[] = {
    SD_MOUNT_POINT "/mem", SD_MOUNT_POINT "/mem/raw",
    SD_MOUNT_POINT "/face", SD_MOUNT_POINT "/wake",
    SD_MOUNT_POINT "/voice", SD_MOUNT_POINT "/etc",
    SD_MOUNT_POINT "/logs",
    SD_MOUNT_POINT "/v1", SD_MOUNT_POINT "/v1/personas",  /* T20 角色隔离域 */
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
    if (tried) return s_ok ? ESP_OK : ESP_FAIL;   /* 自动路径只一次 */
    tried = true;
    /* 默认 SDSPI(SPI2): 板上例程三模式实测(2026-11-12 干净例程, 用户换
     * SanDisk 8G FAT32 后)—— SDMMC slot1(原生 PAD)/slot0(GPIO matrix)
     * 均 ESP_FAIL, 仅 SDSPI 挂载+读写回全通; SDMMC 路线与该卡座组合
     * 不兼容(与 hosted 无关, 例程无 C5 也不通)。1-bit ~1MB/s 够用。 */
    return app_storage_mount_mode(1);
}

/* 卡座 DET 脚电平(无卡悬空/插卡接地 —— 语义以插拔对比标定) */
static int det_level(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PIN_TF_DET,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    return gpio_get_level(BOARD_PIN_TF_DET);
}

esp_err_t app_storage_mount_mode(int mode)
{
    /* 已挂载: 先卸载释放引脚/总线, 再按新模式重来 */
    if (s_ok) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_ok = false;
        if (s_mode == 1 && s_spi_inited) {
            spi_bus_free(SPI2_HOST);
            s_spi_inited = false;
        }
    }
    /* SPI 模式残留 bus(上次失败未清): free 后重 init 才合法 */
    if (!s_ok && s_spi_inited && mode != 1) {
        spi_bus_free(SPI2_HOST);
        s_spi_inited = false;
    }
    s_mode = mode;
    ESP_LOGI(TAG, "sd mount(mode=%s) det=%d",
             mode == 0 ? "mmc-slot0" : "sdspi-spi2", det_level());

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,        /* 绝不格式化用户卡 */
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err;
    if (mode == 0) {
        /* ---- 路线 A: SDMMC slot0(4-bit) ----
         * slot1 = C5 WiFi(esp_hosted) 绝不能占; P4 双槽共控制器时钟域,
         * 与 hosted 并存曾出 probe assert(net 前移+记忆挪 PSRAM 后
         * 内存根治, 是否共存彻底稳待本轮台调)。 */
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
        err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                      &mcfg, &s_card);
    } else {
        /* ---- 路线 B: SDSPI(SPI2, 1-bit) ----
         * SPI 外设与 SDMMC 控制器物理独立, 与 C5 hosted 彻底解耦。
         * 卡座引脚 → SPI 映射(SPI 模式标准脚位): CLK=43 SCK,
         * CMD=44 MOSI, D0=39 MISO, D3=42 CS。1-bit 吞吐 ~1MB/s,
         * 记忆/素材频度绰绰有余。
         * SPI 模式要求 D1/D2 恒高(卡借 D3=低 + CMD0 进 SPI 态,
         * D1/D2 悬空会拒入): 底板若无外部上拉, 内部上拉兜底。 */
        gpio_config_t pu = {
            .pin_bit_mask = (1ULL << BOARD_PIN_TF_D1) |
                            (1ULL << BOARD_PIN_TF_D2),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&pu);
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = BOARD_PIN_TF_CMD,
            .miso_io_num = BOARD_PIN_TF_D0,
            .sclk_io_num = BOARD_PIN_TF_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        if (!s_spi_inited) {
            err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "spi bus init: %s", esp_err_to_name(err));
                return err;
            }
            s_spi_inited = true;
        }
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI2_HOST;
        sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
        dev.gpio_cs = BOARD_PIN_TF_D3;
        err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &dev,
                                      &mcfg, &s_card);
        if (err != ESP_OK) {
            /* vfs 层失败路径已 deinit host 设备; bus 归本模块 → free
             * 后重试才合法(下次 mount_mode 重新 initialize) */
            spi_bus_free(SPI2_HOST);
            s_spi_inited = false;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sd mount failed(mode=%d): %s(无卡/接触? "
                 "可插拔后 sdprobe 对比, sdretry 重试)", mode,
                 esp_err_to_name(err));
        return err;
    }
    /* 目录骨架(存在即跳过; 全列表都要建, 首项即 /sdcard/mem) */
    for (unsigned i = 0; i < sizeof(s_dirs) / sizeof(s_dirs[0]); i++) {
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

/* §55.16 续2: store_crypt 的 cache 是共享可变状态——SD_READY 时刻
 * ui_port(表情)/persona/soul_mem 多任务并发 app_storage_read, 无锁
 * free/malloc cache → 堆腐蚀 → 全核 Instruction access fault boot loop
 * (d423cd8 板上实证)。读段全程持锁(payload 拷贝含在内)。 */
static SemaphoreHandle_t s_crypt_lock;

esp_err_t app_storage_init(void)
{
    /* 只订阅; 真正挂载在 WiFi 就绪后(见 storage_evt_cb 注释)。
     * crypt 读锁提前建好(bind 前后都可能有人读)。 */
    if (s_crypt_lock == NULL)
        s_crypt_lock = xSemaphoreCreateMutex();
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

/* RAW 层(T14): crypt 容器的底层 io; 公开 read/write 在其上加解密 */
static char *raw_read(const char *rel, size_t *len);
static esp_err_t raw_write(const char *rel, const void *data, size_t len);
static store_crypt_t *s_crypt;   /* T14: EPSCV2 容器(enable 后非空) */

void app_storage_crypt_bind(store_crypt_t *c) { s_crypt = c; }

bool app_storage_crypt_active(void) { return s_crypt != NULL; }

char *app_storage_read(const char *rel, size_t *len)
{
    if (s_crypt != NULL) {
        size_t n = 0;
        char *out = NULL;
        /* buf=NULL + 返回值 0 → 数据在 crypt scratch; -2 → AUTH 拒读;
         * 1 → 不存在(继续按 NULL)。读+拷贝全程持锁(cache 共享态) */
        if (xSemaphoreTake(s_crypt_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGW(TAG, "crypt read lock timeout(%s)", rel);
            return NULL;
        }
        int crc = store_crypt_read(s_crypt, rel, NULL, 0, &n);
        if (crc == -2) {
            ESP_LOGW(TAG, "crypt read auth fail(%s) - 保护性拒读", rel);
        } else if (crc == 0) {
            const void *payload = store_crypt_payload(s_crypt);
            out = payload != NULL ? malloc(n + 1) : NULL;
            if (out != NULL) {
                memcpy(out, payload, n);
                out[n] = 0;
                if (len != NULL) *len = n;
            }
        }
        xSemaphoreGive(s_crypt_lock);
        if (crc == -2 || (crc == 0 && out != NULL)) return out;
        if (crc == 0) return NULL;          /* payload/malloc 失败 */
        /* crc == 1(文件不存在) 或其他 → 落回 raw */
        if (crc != 1) return NULL;
    }
    return raw_read(rel, len);
}

static char *raw_read(const char *rel, size_t *len)
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
    /* ⚠ PSRAM: 素材/模型尺寸到几十 KB 起步, 内部堆挤不过(pulse.efc
     * 63KB 板上实测 malloc 失败 → 热切"读取失败", 2026-11-13) */
    char *buf = heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM);
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
    if (s_crypt != NULL) return store_crypt_write(s_crypt, rel, data, len);
    return raw_write(rel, data, len);
}

static esp_err_t raw_write(const char *rel, const void *data, size_t len)
{
    if (!s_ok) return ESP_ERR_INVALID_STATE;
    char path[96], tmp[104];
    app_storage_path(path, sizeof(path), rel);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        /* 自动建父目录(逐级; SD 记忆作用域等新子目录) 后重试一次 */
        for (char *q = path + 1; *q; q++) {
            if (*q == '/') { *q = 0; mkdir(path, 0775); *q = '/'; }
        }
        f = fopen(tmp, "wb");
        if (f == NULL) {
            ESP_LOGW(TAG, "write fopen(%s) fail errno=%d", tmp, errno);
            return ESP_FAIL;
        }
    }
    size_t wr = fwrite(data, 1, len, f);
    int wr_errno = errno;
    /* fflush+fsync 后再 rename: FATFS 写缓存不落盘时, 硬重启会丢
     * "已返回成功"的文件(T20 板上实证: panic 重启丢注册表) ——
     * docs/15 M8 边界的落地修复; 记忆全量回写同路径受益 */
    if (fflush(f) != 0) {
        ESP_LOGW(TAG, "write fflush errno=%d", errno);
    }
    if (fsync(fileno(f)) != 0) {
        ESP_LOGW(TAG, "write fsync errno=%d", errno);
    }
    fclose(f);
    if (wr != len) {
        ESP_LOGW(TAG, "write fwrite %zu/%zu errno=%d", wr, len, wr_errno);
        remove(tmp);
        return ESP_FAIL;
    }
    /* FATFS f_rename 不覆盖已存在目标(FR_EXIST→errno 17), 而 POSIX
     * rename 允许覆盖 —— 先删目标再换名补齐语义(2026-11-12 台调:
     * episodes 全量回写第二笔起全败于此, errno=17 定位) */
    remove(path);
    if (rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "write rename errno=%d", errno);
        remove(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- 公开 RAW 层包装(T14: app_storage_crypt_enable 绑 RAW 用;
 * 批次 44 补——头文件已声明但实现缺失, 曾致启用路径链接不过) ---- */
char *app_storage_raw_read(const char *rel, size_t *len)
{
    return raw_read(rel, len);
}

esp_err_t app_storage_raw_write(const char *rel, const void *data, size_t len)
{
    return raw_write(rel, data, len);
}

esp_err_t app_storage_append(const char *rel, const char *line)
{
    if (!s_ok) return ESP_ERR_INVALID_STATE;
    /* T14: crypt 启用时走 读→追加→加密写(台账小文件, 追加频度低) */
    if (s_crypt != NULL) {
        uint8_t buf[4096];
        size_t n = 0;
        int crc = store_crypt_read(s_crypt, rel, buf, sizeof(buf), &n);
        if (crc != 0 && crc != 1) return ESP_FAIL;   /* AUTH/IO: 不破坏 */
        size_t add = strlen(line);
        if (n + add + 1 > sizeof(buf)) return ESP_ERR_NO_MEM;
        memcpy(buf + n, line, add);
        buf[n + add] = '\n';
        return store_crypt_write(s_crypt, rel, buf, n + add + 1);
    }
    char path[96];
    app_storage_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "ab");
    if (f == NULL) return ESP_FAIL;
    size_t wr = fwrite(line, 1, strlen(line), f);
    fputc('\n', f);
    /* 删除台账等追加记录必须落盘(T23): 防硬重启丢"已接受"的删除意图 */
    if (fflush(f) != 0) ESP_LOGW(TAG, "append fflush errno=%d", errno);
    if (fsync(fileno(f)) != 0) ESP_LOGW(TAG, "append fsync errno=%d", errno);
    fclose(f);
    return wr == strlen(line) ? ESP_OK : ESP_FAIL;
}

/* 台调诊断: 引脚电平全景(插/拔卡各跑一次对比)。
 * 未挂载时附加链路测试: 手动打 80 个 SCK 脉冲看 D0 是否跟随卡行为
 * (无卡悬空读上拉稳 1; 有卡时 CMD/CLK 驱动下 D0 会有翻转迹象)。 */
void app_storage_probe(void)
{
    ESP_LOGI(TAG, "probe: det=%d (插拔卡对比: 电平翻转=卡座 DET 通路)",
             det_level());
    if (s_ok) {
        /* 已挂载: 文件系统级写读自检(不动记忆文件, 用 logs/probe.txt) */
        const char *msg = "sd rw probe";
        esp_err_t e = app_storage_write("logs/probe.txt", msg, strlen(msg));
        size_t n = 0;
        char *back = app_storage_read("logs/probe.txt", &n);
        ESP_LOGI(TAG, "probe: 已挂载(mode=%d) 文件写读: %s/%s '%s'",
                 s_mode, esp_err_to_name(e),
                 back ? "read-ok" : "READ-FAIL", back ? back : "");
        free(back);
        return;
    }
    /* 全脚输入上拉读基准(卡在位时 D0..D3 被卡内上拉稳定拉高) */
    const int pins[] = { BOARD_PIN_TF_CLK, BOARD_PIN_TF_CMD,
                         BOARD_PIN_TF_D0, BOARD_PIN_TF_D1,
                         BOARD_PIN_TF_D2, BOARD_PIN_TF_D3 };
    gpio_config_t in = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        in.pin_bit_mask |= 1ULL << pins[i];
    }
    gpio_config(&in);
    vTaskDelay(pdMS_TO_TICKS(2));
    ESP_LOGI(TAG, "probe base: clk=%d cmd=%d d0=%d d1=%d d2=%d d3=%d",
             gpio_get_level(BOARD_PIN_TF_CLK),
             gpio_get_level(BOARD_PIN_TF_CMD),
             gpio_get_level(BOARD_PIN_TF_D0),
             gpio_get_level(BOARD_PIN_TF_D1),
             gpio_get_level(BOARD_PIN_TF_D2),
             gpio_get_level(BOARD_PIN_TF_D3));
    /* 手动 SCK 脉冲(CMD/D3 保持高 = 不发命令, 卡只得到时钟):
     * 有卡时 D0 可能出现忙指示(拉低)或保持高; 悬空读上拉恒高 */
    gpio_set_direction(BOARD_PIN_TF_CLK, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_PIN_TF_CLK, 1);
    gpio_set_direction(BOARD_PIN_TF_CMD, GPIO_MODE_OUTPUT_OD);  /* 开漏近似集电极开路 CMD */
    gpio_set_level(BOARD_PIN_TF_CMD, 1);
    gpio_set_direction(BOARD_PIN_TF_D3, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_PIN_TF_D3, 1);
    int d0_low_seen = 0;
    for (int i = 0; i < 80; i++) {
        gpio_set_level(BOARD_PIN_TF_CLK, 0);
        esp_rom_delay_us(5);                    /* ~100kHz */
        gpio_set_level(BOARD_PIN_TF_CLK, 1);
        esp_rom_delay_us(5);
        if (gpio_get_level(BOARD_PIN_TF_D0) == 0) d0_low_seen++;
    }
    ESP_LOGI(TAG, "probe clk80: d0 低电平采样 %d/80(0=恒高悬空/卡忙结束, "
             ">0=D0 有翻转迹象=卡在响应时钟)", d0_low_seen);
}

#else /* SD off */

esp_err_t app_storage_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_storage_mount_mode(int m) { (void)m; return ESP_ERR_NOT_SUPPORTED; }
void app_storage_probe(void) { }
bool app_storage_ok(void) { return false; }
int app_storage_path(char *b, size_t c, const char *r)
{ return snprintf(b, c, "/sdcard/%s", r); }
char *app_storage_read(const char *r, size_t *l) { (void)r; if (l) *l = 0; return NULL; }
esp_err_t app_storage_write(const char *r, const void *d, size_t l)
{ (void)r; (void)d; (void)l; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_storage_append(const char *r, const char *l)
{ (void)r; (void)l; return ESP_ERR_NOT_SUPPORTED; }

#endif
