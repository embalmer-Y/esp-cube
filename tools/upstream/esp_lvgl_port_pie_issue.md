# [Draft] esp_lvgl_port on ESP32-P4: default task stack caps lets the LVGL task stack land in LP SRAM (RTCRAM) — crashes any PIE workload (esp_efc_codec)

> 用途: 提交给 espressif/esp_lvgl_port 的社区 issue 草稿(§53 E3)。
> 发布前由用户审阅/删改; 标题与正文可按仓库 issue 模板微调。

## Environment

- Chip: ESP32-P4 (ESP32-P4-Function-EV-Board class board, 32MB PSRAM)
- IDF: v6.1 + esp_lvgl_port (latest at 2026-11) + esp_efc_codec 1.1.0 + LVGL 9.6
- Config: `CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=y` (default when no deep-sleep wake stub usage), SPIRAM enabled

## Symptom

Using `lv_efc` (esp_efc_codec widget) inside the LVGL task created by `esp_lvgl_port_init()`,
the board resets with a store exception ~1.6s after playback starts:

```
Guru Meditation Error: Core  1 panic'ed (StoreProhibited)
mcause=7, mepc=rtos_save_pie_coproc, mtval=0x50108370
```

Reproduced 6/6 boots at the identical fault site.

## Root cause

- On ESP32-P4, `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` (the effective default for the
  LVGL task stack) **includes LP SRAM (a.k.a. RTCRAM, 0x50108000–0x5010FFFF)** once
  `ALLOW_RTC_FAST_MEM_AS_HEAP=y` — LP SRAM is *not* excluded from those caps
  (it is only excluded from `MALLOC_CAP_DMA`).
- With a 16–20KB stack the allocation frequently lands in RTCRAM.
- esp_efc decoding uses the **PIE vector co-processor**; FreeRTOS carves the PIE
  context save area **inside the owning task's own stack** (`rtos_save_pie_coproc`),
  and PIE vector stores (`esp.vst.128`) **cannot target LP SRAM**.
  Result: the first PIE-context save after any other task touches PIE writes to
  an RTCRAM address → store fault.

## Workaround (what we ship)

Explicitly restrict the stack caps — the field exists but is easy to miss:

```c
lvgl_port_cfg_t cfg = ESP_LVGL_PORT_INIT_CONFIG();
cfg.task_stack = 20480;
cfg.task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;  /* DMA excludes RTCRAM */
```

Verified stable across a 1h soak + 10× clip-restart stress test.

## Suggestion

At least one of:

1. Document the PIE/LP-SRAM pitfall on `task_stack_caps` (README + header comment),
   ideally with a note that **any** task that will execute PIE instructions
   (esp_efc_codec, future PIE users) must keep its stack out of RTCRAM;
2. Default `task_stack_caps` on ESP32-P4 to `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`;
3. Add a startup check: if the created LVGL task stack lies inside the LP-SRAM
   range and PIE is enabled, log a loud warning.

## Details / credit

Full root-cause chain (mcause/mepc/mtval analysis, FreeRTOS PIE context carve-out,
LP-SRAM capability map) documented in our project log; happy to attach GDB dumps
and a minimal repro (esp_efc_codec test_app with default esp_lvgl_port config on P4)
if useful.
