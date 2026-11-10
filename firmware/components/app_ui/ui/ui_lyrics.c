/*
 * ui_lyrics.c —— 歌词流星式飞行动画(snapshot→image 方案)
 *
 * 从 ui_pages.c 拆出(docs/06 §5); 关键技术决策:
 *   - label 旋转在 LVGL 9.6.0 渲染路径缺失 → 排版 label 离屏仅供
 *     snapshot, 显示/旋转由 lv_image transform 承担(实测结论);
 *   - 飞行只做位移不叠 opa(旋转+透明双动画叠加会在离屏 layer 崩);
 *   - 快照双槽池(见 ui_internal.h), 飞出句沿用当前槽, 新句写另一槽。
 *
 * 动画: 飞入 ease_out(越近中心越慢) / 飞离 ease_in(加速离场),
 *       每句随机倾角 ±4~13° 与随机方向(不绑定, 错落感)。
 */
#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ui_lyr_t ui_lyr;

/* 飞行端点钳制屏内: 完全出屏的旋转 layer 裁剪区退化会分配失败 */
static int lyr_clamp_x(int x)
{
    if (x < 6) x = 6;
    if (x > ui_pg.screen_w - 6) x = ui_pg.screen_w - 6;
    return x;
}

static int lyr_clamp_y(int y)
{
    if (y < 6) y = 6;
    if (y > ui_pg.screen_h - 46) y = ui_pg.screen_h - 46;
    return y;
}

static void lyric_out_anim_done_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(ui_lyr.img_out, LV_OBJ_FLAG_HIDDEN);  /* 到边缘即隐 */
}

/* 旧句飞离: img_out 接管当前句快照, 沿同方向 ease_in 加速飞向对侧边缘 */
void ui_lyric_fly_out(void)
{
    /* 无旧句(img_cur 隐藏=屏上无句)则忽略 */
    if (lv_obj_has_flag(ui_lyr.img_cur, LV_OBJ_FLAG_HIDDEN)) return;

    /* 快照交接: out 直接引用当前槽内容(池不释放), cur 稍后写另一槽 */
    lv_image_set_src(ui_lyr.img_out, &ui_lyr.snap_dsc[ui_lyr.snap_slot]);
    lv_image_set_rotation(ui_lyr.img_out, ui_lyr.angle);
    lv_obj_set_pos(ui_lyr.img_out, ui_lyr.rest_x, ui_lyr.rest_y);
    lv_obj_remove_flag(ui_lyr.img_out, LV_OBJ_FLAG_HIDDEN);

    int ex = lyr_clamp_x(ui_lyr.rest_x + (int)(ui_lyr.dir_x * LYR_FLY_DIST));
    int ey = lyr_clamp_y(ui_lyr.rest_y + (int)(ui_lyr.dir_y * LYR_FLY_DIST));

    /* 只做位移(至边缘即隐); 不叠 opa 动画(离屏 layer 渲染缺陷) */
    lv_anim_del(ui_lyr.img_out, ui_anim_x_cb);
    lv_anim_del(ui_lyr.img_out, ui_anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui_lyr.img_out);
    lv_anim_set_exec_cb(&a, ui_anim_x_cb);
    lv_anim_set_values(&a, ui_lyr.rest_x, ex);
    lv_anim_set_duration(&a, LYR_FLY_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, lyric_out_anim_done_cb);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, ui_anim_y_cb);
    lv_anim_set_values(&a, ui_lyr.rest_y, ey);
    lv_anim_start(&a);

    /* 当前句立刻让位, 等新句飞入 */
    lv_anim_del(ui_lyr.img_cur, ui_anim_x_cb);
    lv_anim_del(ui_lyr.img_cur, ui_anim_y_cb);
    lv_obj_add_flag(ui_lyr.img_cur, LV_OBJ_FLAG_HIDDEN);
}

static void lyric_fly_in(const char *text)
{
    /* 随机倾角 ±4~13°(0.1° 单位) */
    int deg = 40 + ui_rand() % 90;               /* 40..130 = 4..13° */
    ui_lyr.angle = (ui_rand() % 2 ? 1 : -1) * deg;

    /* 随机飞入方向(单位向量) */
    float ang = (float)(ui_rand() % 360) * (float)M_PI / 180.0f;
    ui_lyr.dir_x = cosf(ang);
    ui_lyr.dir_y = sinf(ang);

    /* 排版源更新文本 → 快照成 ARGB image(倾斜由 image 承担) */
    lv_label_set_text(ui_lyr.lbl_src, text);
    lv_obj_update_layout(ui_lyr.lbl_src);
    /* 池槽惰性分配: lv_draw_buf_init 绑定外部缓冲(无 ALLOCATED 旗标,
     * 不会被误释放); take_to_draw_buf 内部 reshape 原地改头复用 */
    ui_lyr.snap_slot ^= 1;
    if (ui_lyr.snap_buf[ui_lyr.snap_slot] == NULL) {
        ui_lyr.snap_buf[ui_lyr.snap_slot] = malloc(LYR_SNAP_BYTES);
        if (ui_lyr.snap_buf[ui_lyr.snap_slot] != NULL) {
            lv_draw_buf_init(&ui_lyr.snap_dsc[ui_lyr.snap_slot], 1, 1,
                             LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO,
                             ui_lyr.snap_buf[ui_lyr.snap_slot], LYR_SNAP_BYTES);
        }
    }
    lv_draw_buf_t *dsc = NULL;
    if (ui_lyr.snap_buf[ui_lyr.snap_slot] != NULL) {
        dsc = (lv_snapshot_take_to_draw_buf(
                   ui_lyr.lbl_src, LV_COLOR_FORMAT_ARGB8888,
                   &ui_lyr.snap_dsc[ui_lyr.snap_slot]) == LV_RESULT_OK)
              ? &ui_lyr.snap_dsc[ui_lyr.snap_slot] : NULL;
    }
    if (dsc == NULL) {
        /* 兜底(池未建/文本超池容量): 一次性分配, 仅 OOM 退化态会走到;
         * 屏显引用期间不能立即释放, 由下句快照顶替后自然失引用 */
        dsc = lv_snapshot_take(ui_lyr.lbl_src, LV_COLOR_FORMAT_ARGB8888);
        if (dsc == NULL) return;
    }

    /* ease_out 飞入: 边缘 → 中心(越接近越慢)。
     * 只做位移不叠 opa 动画; 出发点钳制屏内(见 lyr_clamp_*) */
    int sx = lyr_clamp_x(ui_lyr.rest_x - (int)(ui_lyr.dir_x * LYR_FLY_DIST));
    int sy = lyr_clamp_y(ui_lyr.rest_y - (int)(ui_lyr.dir_y * LYR_FLY_DIST));
    lv_image_set_src(ui_lyr.img_cur, dsc);
    lv_image_set_rotation(ui_lyr.img_cur, ui_lyr.angle);
    lv_image_set_antialias(ui_lyr.img_cur, true);
    lv_obj_set_pos(ui_lyr.img_cur, sx, sy);
    lv_obj_remove_flag(ui_lyr.img_cur, LV_OBJ_FLAG_HIDDEN);

    lv_anim_del(ui_lyr.img_cur, ui_anim_x_cb);
    lv_anim_del(ui_lyr.img_cur, ui_anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui_lyr.img_cur);
    lv_anim_set_exec_cb(&a, ui_anim_x_cb);
    lv_anim_set_values(&a, sx, ui_lyr.rest_x);
    lv_anim_set_duration(&a, LYR_FLY_IN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, ui_anim_y_cb);
    lv_anim_set_values(&a, sy, ui_lyr.rest_y);
    lv_anim_start(&a);
}

void ui_lyrics_apply(const ui_model_t *m)
{
    if (m->lyric_cur[0] && strcmp(m->lyric_cur, ui_lyr.last) != 0) {
        strncpy(ui_lyr.last, m->lyric_cur, sizeof(ui_lyr.last) - 1);
        ui_lyr.last[sizeof(ui_lyr.last) - 1] = 0;
        ui_lyric_fly_out();                     /* 旧句加速飞离 */
        lyric_fly_in(m->lyric_cur);             /* 新句减速飞入 */
    }
    if (strcmp(m->lyric_next, ui_lyr.last_next) != 0) {
        strncpy(ui_lyr.last_next, m->lyric_next, sizeof(ui_lyr.last_next) - 1);
        ui_lyr.last_next[sizeof(ui_lyr.last_next) - 1] = 0;
        lv_label_set_text(ui_lyr.lbl_next,
                          m->lyric_next[0] ? m->lyric_next : " ");
    }
}

/* ================= 歌词对象构建 ================= */
void ui_lyrics_create(int w)
{
    /* 中心静止位(label 左上角) */
    ui_lyr.rest_x = (w - LYR_W) / 2;
    ui_lyr.rest_y = 172;
    /* 挂 lv_layer_top(系统置顶层): 主屏上带 pivot/rotation 的 label
     * 在 9.6.0 某些状态下不参与渲染(状态正确却不上屏, 已实测);
     * layer_top 与 overlay 同层, 渲染路径独立且可靠 */
    /* 排版源 label(离屏, 屏外坐标, 只供 snapshot; 不参与渲染) */
    ui_lyr.lbl_src = ui_label_make(lv_layer_top(), FONT_CJK24, C_TXT);
    lv_label_set_text(ui_lyr.lbl_src, " ");
    lv_obj_set_size(ui_lyr.lbl_src, LYR_W, 34);
    lv_label_set_long_mode(ui_lyr.lbl_src, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ui_lyr.lbl_src, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ui_lyr.lbl_src, -600, -600);
    /* 当前句/旧句: 快照 image(旋转走 image transform 成熟路径) */
    ui_lyr.img_cur = lv_image_create(lv_layer_top());
    lv_image_set_antialias(ui_lyr.img_cur, true);
    lv_obj_add_flag(ui_lyr.img_cur, LV_OBJ_FLAG_HIDDEN);
    ui_lyr.img_out = lv_image_create(lv_layer_top());
    lv_image_set_antialias(ui_lyr.img_out, true);
    lv_obj_add_flag(ui_lyr.img_out, LV_OBJ_FLAG_HIDDEN);
    ui_lyr.lbl_next = ui_label_make(ui_pg.scr_main, FONT_CJK, 0x5E6C80);
    lv_obj_set_width(ui_lyr.lbl_next, LYR_W);
    lv_label_set_long_mode(ui_lyr.lbl_next, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ui_lyr.lbl_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(ui_lyr.lbl_next, 0, 0);
    lv_obj_align(ui_lyr.lbl_next, LV_ALIGN_TOP_MID, 0, 224);
}
