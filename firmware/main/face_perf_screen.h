/*
 * face_perf_screen.h —— 表情圆网格采样(纯 C, 零 IDF 依赖, 宿主可测)
 *
 * face_perf.c(任务 4.2)在主屏首次完整渲染后对活动帧缓冲调用本函数,
 * 输出 `face screen check: nonbg=xx.x%`。采样口径唯一来源是
 * face_perf_geom.h, 与 tools/eaf_codec.py geom_sample_points()/screen_nonbg()
 * 逐点一致(32×32 网格、圆心 (230,230)、半径 215 → 810 个圆内点)。
 * 宿主测试: firmware/main/test/test_face_perf_screen.c(host_tests.sh [18])。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 在 RGB565(本机字节序 uint16)帧上采样表情圆, 统计像素值 != bg 的点数。
 *   buf    帧首地址; stride 每行字节数(>= w * 2); w/h 帧宽高(像素)。
 *   返回值 圆内网格点总数(帧外的点按背景计入总数, 与 lint "素材外=背景"一致);
 *   *hit   非背景点数(hit 可为 NULL)。
 * buf 为 NULL、w/h 为 0 或 stride < w * 2 时返回 0 且 *hit = 0。
 * 只读 buf, 不分配内存。
 */
uint32_t face_perf_screen_nonbg(const uint8_t *buf, uint32_t stride,
                                uint32_t w, uint32_t h, uint16_t bg,
                                uint32_t *hit);

#ifdef __cplusplus
}
#endif
