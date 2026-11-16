/*
 * face_perf_screen.c —— 表情圆网格采样实现(纯 C, 零 IDF 依赖)
 * 契约见 face_perf_screen.h, 几何口径见 face_perf_geom.h。
 */
#include "face_perf_screen.h"

#include <stddef.h>
#include <string.h>
#include "face_perf_geom.h"

uint32_t face_perf_screen_nonbg(const uint8_t *buf, uint32_t stride,
                                uint32_t w, uint32_t h, uint16_t bg,
                                uint32_t *hit)
{
    if (hit != NULL) *hit = 0;
    if (buf == NULL || w == 0 || h == 0 || stride / 2u < w) return 0;
    uint32_t pts = 0, n = 0;
    for (int j = 0; j < FACE_GEOM_GRID; j++) {
        const int y = FACE_GEOM_SAMPLE(FACE_GEOM_CY, j);
        for (int i = 0; i < FACE_GEOM_GRID; i++) {
            const int x = FACE_GEOM_SAMPLE(FACE_GEOM_CX, i);
            const int dx = x - FACE_GEOM_CX, dy = y - FACE_GEOM_CY;
            if (dx * dx + dy * dy > FACE_GEOM_R * FACE_GEOM_R) continue;
            pts++;
            if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h) {
                continue;                       /* 帧外 = 背景 */
            }
            uint16_t px;
            memcpy(&px, buf + (size_t)y * stride + (size_t)x * 2u, sizeof(px));
            if (px != bg) n++;
        }
    }
    if (hit != NULL) *hit = n;
    return pts;
}
