#!/usr/bin/env python3
"""freeze_watch 实时分类状态的单元测试（freeze-and-face-blackscreen-fix 任务 13.3）。

覆盖 classify_stream 合约之外的状态：打开后的复位、!rst 之后的复位、时间驱动的静默判定、
Saved PC 解析与 USB 事件过滤。直接使用真实分类器，不使用模拟对象。
"""
import unittest

import freeze_watch as fw


class LiveStateTest(unittest.TestCase):

    def test_first_banner_after_open_is_monitor_open_reset(self):
        clf = fw.StreamClassifier()
        clf.arm_open(10.0)
        self.assertEqual(clf.on_line(10.2, "ESP-ROM:esp32p4-eco7-20260109"), [])
        ev = clf.on_line(10.3, "rst:0x17 (CHIP_USB_UART_RESET),boot:0x30f (SPI_FAST_FLASH_BOOT)")
        self.assertEqual([(k, t) for k, t, _ in ev], [("MONITOR_OPEN_RESET", 10.3)])

    def test_banner_after_open_window_is_rom_reset(self):
        clf = fw.StreamClassifier()
        clf.arm_open(0.0)
        ev = clf.on_line(fw.OPEN_RESET_S + 1, "rst:0x7 (TG0WDT_SYS_RST),boot:0x13 (SPI_FAST_FLASH_BOOT)")
        self.assertEqual([k for k, _, _ in ev], ["ROM_RESET"])

    def test_download_banner_after_open_is_reported_on_wait_line(self):
        clf = fw.StreamClassifier()
        clf.arm_open(0.0)
        first = clf.on_line(0.2, "rst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))")
        self.assertEqual([k for k, _, _ in first], ["MONITOR_OPEN_RESET"])
        second = clf.on_line(0.3, "waiting for download")
        self.assertEqual([k for k, _, _ in second], ["HOST_DOWNLOAD_MODE"])

    def test_silence_reported_once_by_tick_then_again_after_new_output(self):
        clf = fw.StreamClassifier()
        clf.arm_open(0.0)
        clf.on_line(1.0, "I (1000) app: alive")
        self.assertEqual(clf.tick(10.0), [])
        self.assertEqual([(k, t) for k, t, _ in clf.tick(16.5)], [("SILENT_STOP", 1.0)])
        self.assertEqual(clf.tick(40.0), [])
        clf.on_line(41.0, "I (41000) app: back")
        self.assertEqual([k for k, _, _ in clf.tick(60.0)], ["SILENT_STOP"])

    def test_no_silence_inside_rom_episode(self):
        clf = fw.StreamClassifier()
        clf.arm_open(0.0)
        clf.on_line(0.5, "ESP-ROM:esp32p4-eco7-20260109")
        self.assertEqual(clf.tick(60.0), [])

    def test_operator_reset_prevents_auto_recovered(self):
        clf = fw.StreamClassifier()
        clf.arm_open(0.0)
        clf.on_line(6.0, "I (6000) app: alive")        # 超过打开复位窗口
        ev = clf.on_line(7.0, "Guru Meditation Error: Core  0 panic'ed (LoadProhibited).")
        self.assertIn("PANIC", [k for k, _, _ in ev])
        clf.operator_reset(8.0)
        ev = clf.on_line(8.5, "rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)")
        self.assertEqual([k for k, _, _ in ev], ["ROM_RESET"])
        self.assertEqual(clf.on_line(9.0, "I (9000) sysdiag: fw=x elf=y built=z"), [])

    def test_watchdog_reset_then_sysdiag_is_auto_recovered(self):
        records = [(0.0, "I (1) app: alive"),
                   (1.0, "rst:0x7 (TG0WDT_SYS_RST),boot:0x13 (SPI_FAST_FLASH_BOOT)"),
                   (3.0, "I (2000) sysdiag: fw=x elf=y built=z")]
        self.assertEqual([k for k, _, _ in fw.classify_stream(records)], ["ROM_RESET", "AUTO_RECOVERED"])

    def test_saved_pc_detail_without_symbolizer(self):
        ev = fw.classify_stream([(0.0, "ESP-ROM:x"), (0.1, "Saved PC:0x40801f3e")])
        self.assertEqual(ev, [("SAVED_PC", 0.1, "0x40801f3e")])

    def test_usb_filter_matches_ch343_and_ttyacm_only(self):
        self.assertTrue(fw.USB_FILTER.search("usb 1-7.2: New USB device found, idVendor=1a86, idProduct=55d3"))
        self.assertTrue(fw.USB_FILTER.search("cdc_acm 1-7.2:1.0: ttyACM1: USB ACM device"))
        self.assertFalse(fw.USB_FILTER.search("perf: interrupt took too long (3158 > 3138)"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
