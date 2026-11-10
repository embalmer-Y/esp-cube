/*
 * main.c —— EspCube 模拟器宿主(SDL2)
 *
 * 职责:
 *   - SDL 窗口/渲染器 + LVGL 显示驱动(全帧 RGB565 直绘缓冲);
 *   - 鼠标=触摸(滑动手势), 滚轮/键盘=场景动作; 亮度=整窗黑罩;
 *   --headless --demo: dummy 驱动 + 自动时间线 + BMP 截图(无屏自检)。
 *
 * 用法:
 *   ./run.sh                 # 交互窗口(H=帮助)
 *   ./run.sh --demo          # 无屏自动演示 + shots/ 截图
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "ui_pages.h"
#include "ui_model.h"
#include "sim_host.h"

#define SIM_W 460
#define SIM_H 460

static SDL_Window *s_win;
static SDL_Renderer *s_rend;
static SDL_Texture *s_tex;
static lv_display_t *s_disp;
static lv_color16_t *s_frame;                /* 全帧 RGB565(直绘模式) */

static lv_obj_t *s_fps_label, *s_help_panel;
static bool s_show_fps, s_show_help;
static bool s_running = true;
static bool s_demo_mode;
static int s_mouse_x, s_mouse_y;
static bool s_mouse_down;
static int s_wheel_acc;

/* ================= 截图(RGB565 → BMP24) ================= */
static void save_bmp(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "shots/%s.bmp", name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("[shot] cannot open %s\n", path);
        return;
    }
    const int row = SIM_W * 3;
    const int data = row * SIM_H;
    const unsigned char head[54] = {
        'B', 'M',
        (unsigned char)(54 + data), (unsigned char)((54 + data) >> 8),
        (unsigned char)((54 + data) >> 16), (unsigned char)((54 + data) >> 24),
        0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
        (unsigned char)SIM_W, (unsigned char)(SIM_W >> 8), 0, 0,
        (unsigned char)SIM_H, (unsigned char)(SIM_H >> 8), 0, 0,
        1, 0, 24, 0, 0, 0, 0, 0,
        (unsigned char)data, (unsigned char)(data >> 8),
        (unsigned char)(data >> 16), (unsigned char)(data >> 24),
        0x13, 0x0B, 0, 0, 0x13, 0x0B, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    fwrite(head, 1, 54, f);
    for (int y = SIM_H - 1; y >= 0; y--) {   /* BMP 自底向上 */
        unsigned char line[SIM_W * 3];
        for (int x = 0; x < SIM_W; x++) {
            uint16_t px = *(const uint16_t *)&s_frame[y * SIM_W + x];
            unsigned char r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F,
                          b = px & 0x1F;
            line[x * 3 + 0] = (unsigned char)(b << 3);
            line[x * 3 + 1] = (unsigned char)(g << 2);
            line[x * 3 + 2] = (unsigned char)(r << 3);
        }
        fwrite(line, 1, row, f);
    }
    fclose(f);
    printf("[shot] %s\n", path);
}

void sim_host_screenshot(const char *name) { save_bmp(name); }

/* ================= LVGL 显示/输入驱动 ================= */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)px;
    if (lv_display_flush_is_last(disp)) {
        SDL_UpdateTexture(s_tex, NULL, s_frame, SIM_W * 2);
        SDL_RenderClear(s_rend);
        SDL_RenderCopy(s_rend, s_tex, NULL, NULL);
        const ui_model_t *m = ui_model();
        if (m->brightness < 100) {
            Uint8 a = (Uint8)((100 - m->brightness) * 255 / 100);
            SDL_SetRenderDrawBlendMode(s_rend, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(s_rend, 0, 0, 0, a);
            SDL_RenderFillRect(s_rend, NULL);
        }
        SDL_RenderPresent(s_rend);
    }
    lv_display_flush_ready(disp);
}

static void pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = s_mouse_x;
    data->point.y = s_mouse_y;
    data->state = s_mouse_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ================= 帮助/帧率覆盖层 ================= */
static void overlay_create(void)
{
    s_fps_label = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_fps_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_fps_label, lv_color_hex(0x8B98AC), 0);
    lv_obj_align(s_fps_label, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_label_set_text(s_fps_label, "");

    s_help_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_help_panel);
    lv_obj_set_style_bg_color(s_help_panel, lv_color_hex(0x0E1420), 0);
    lv_obj_set_style_bg_opa(s_help_panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_help_panel, 16, 0);
    lv_obj_set_style_border_color(s_help_panel, lv_color_hex(0x2A3A55), 0);
    lv_obj_set_style_border_width(s_help_panel, 1, 0);
    lv_obj_set_size(s_help_panel, 330, 250);
    lv_obj_center(s_help_panel);
    lv_obj_add_flag(s_help_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *hl = lv_label_create(s_help_panel);
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(0xE9EFF7), 0);
    lv_obj_set_style_pad_all(s_help_panel, 18, 0);
    lv_label_set_text(hl,
        "EspCube 模拟器\n"
        "----------------\n"
        "鼠标: 上下滑=音量 左右滑=切曲\n"
        "滚轮: 音量\n"
        "空格: 播放/停止\n"
        "N / P: 下一曲 / 上一曲\n"
        "B: 蓝牙连接切换  M: BT切歌\n"
        "T: 电台标题刷新\n"
        "↑/↓: 音量  F: 帧率\n"
        "S: 截图  H: 帮助  ESC: 退出");
}

/* ================= 事件循环 ================= */
static void handle_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_ESCAPE: s_running = false; return;
    case SDLK_s: save_bmp("manual"); return;
    case SDLK_f: s_show_fps = !s_show_fps; return;
    case SDLK_h:
        s_show_help = !s_show_help;
        if (s_show_help) lv_obj_remove_flag(s_help_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_help_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    case SDLK_SPACE: mock_app_action(SIM_ACT_TOGGLE_PLAY); return;
    case SDLK_n: case SDLK_RIGHT:
        mock_app_action(SIM_ACT_NEXT_STATION); return;
    case SDLK_p: case SDLK_LEFT:
        mock_app_action(SIM_ACT_PREV_STATION); return;
    case SDLK_b: mock_app_action(SIM_ACT_BT_TOGGLE); return;
    case SDLK_m: mock_app_action(SIM_ACT_BT_META); return;
    case SDLK_t: mock_app_action(SIM_ACT_ICY_TITLE); return;
    case SDLK_UP: mock_app_action(SIM_ACT_VOL_UP); return;
    case SDLK_DOWN: mock_app_action(SIM_ACT_VOL_DOWN); return;
    default: return;
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) s_demo_mode = true;
        else if (strcmp(argv[i], "--headless") == 0) s_demo_mode = true;
        else printf("unknown arg: %s\n", argv[i]);
    }
    if (s_demo_mode) setenv("SDL_VIDEODRIVER", "dummy", 1);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    s_win = SDL_CreateWindow("EspCube Simulator  [H=帮助]",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             SIM_W, SIM_H, SDL_WINDOW_SHOWN);
    s_rend = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (s_rend == NULL) s_rend = SDL_CreateRenderer(s_win, -1, 0);
    s_tex = SDL_CreateTexture(s_rend, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, SIM_W, SIM_H);

    /* LVGL */
    lv_init();
    s_frame = malloc(sizeof(lv_color16_t) * SIM_W * SIM_H);
    s_disp = lv_display_create(SIM_W, SIM_H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, (uint8_t *)s_frame, NULL,
                           SIM_W * SIM_H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_indev_t *ptr = lv_indev_create();
    lv_indev_set_type(ptr, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ptr, pointer_read_cb);

    overlay_create();
    mock_app_init(s_demo_mode);
    ui_pages_create(SIM_W, SIM_H);
    printf("EspCube simulator ready%s\n",
           s_demo_mode ? " (demo mode)" : "  -- 按 H 查看按键帮助");

    /* 主循环 */
    uint32_t last = SDL_GetTicks(), last_fps_t = last, frames = 0;
    uint32_t demo_t0 = last, demo_frames = 0;   /* demo 帧耗时基线(性能回归对比) */
    uint32_t handler_ms_acc = 0;
    while (s_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: s_running = false; break;
            case SDL_MOUSEMOTION:
                s_mouse_x = ev.motion.x; s_mouse_y = ev.motion.y; break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) s_mouse_down = true;
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) s_mouse_down = false;
                break;
            case SDL_MOUSEWHEEL:
                s_wheel_acc += ev.wheel.y;
                while (s_wheel_acc > 0) {
                    mock_app_action(SIM_ACT_VOL_UP); s_wheel_acc--;
                }
                while (s_wheel_acc < 0) {
                    mock_app_action(SIM_ACT_VOL_DOWN); s_wheel_acc++;
                }
                break;
            case SDL_KEYDOWN: handle_key(ev.key.keysym.sym); break;
            }
        }

        uint32_t now = SDL_GetTicks();
        lv_tick_inc(now - last);
        last = now;
        uint32_t h0 = now;
        lv_timer_handler();
        handler_ms_acc += SDL_GetTicks() - h0;   /* 纯 UI 帧耗时(排除节流延时) */
        mock_app_periodic(now);
        if (s_demo_mode && mock_app_demo_done()) {
            SDL_Delay(300);
            break;
        }
        SDL_Delay(5);

        frames++;
        demo_frames++;
        if (s_show_fps && now - last_fps_t >= 500) {
            lv_label_set_text_fmt(s_fps_label, "%d fps", frames * 1000 / (now - last_fps_t));
            frames = 0;
            last_fps_t = now;
        }
    }
    if (s_demo_mode) {
        /* 帧耗时基线: demo 全时间线平均帧率/帧耗时(性能回归对比用) */
        uint32_t demo_ms = SDL_GetTicks() - demo_t0;
        printf("[perf] demo frames=%u elapsed=%ums avg=%.2fms/frame (%.0f fps)"
               " | UI work avg=%.3fms/frame\n",
               demo_frames, demo_ms,
               demo_ms ? (double)demo_ms / demo_frames : 0,
               demo_ms ? demo_frames * 1000.0 / demo_ms : 0,
               demo_frames ? (double)handler_ms_acc / demo_frames : 0);
    }

    free(s_frame);
    SDL_Quit();
    printf("simulator exit\n");
    return 0;
}
