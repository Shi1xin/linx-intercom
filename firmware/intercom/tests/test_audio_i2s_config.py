"""Source guards for explicit left-slot 32-bit mono capture and diagnostic placement. On-target audio behavior is verified separately."""

import re
import unittest
from pathlib import Path

MAIN_DIR = Path(__file__).resolve().parent.parent / "main"


def _extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"{signature} not found")
    brace = source.find("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise AssertionError(f"unbalanced braces after {signature}")


class TestRxSlotConfig(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.body = _extract_function(
            (MAIN_DIR / "audio_dev.c").read_text(), "static esp_err_t i2s_rx_init(void)"
        )

    def test_left_slot_override_before_init_std_mode(self):
        philips = self.body.find("I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG")
        override = self.body.find("std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT")
        init = self.body.find("i2s_channel_init_std_mode(s_rx")
        self.assertGreaterEqual(philips, 0, "Philips slot default macro missing")
        self.assertGreater(override, philips,
                           "LEFT slot_mask override must come after the macro init")
        self.assertGreater(init, override,
                           "LEFT slot_mask override must come before init_std_mode")

    def test_philips_config_uses_32bit_mono(self):
        m = re.search(
            r"I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG\(\s*I2S_DATA_BIT_WIDTH_32BIT\s*,\s*I2S_SLOT_MODE_MONO\s*\)",
            self.body,
        )
        self.assertIsNotNone(m, "RX must use 32-bit mono Philips slot config")


class TestCaptureDiagnostics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.body = _extract_function(
            (MAIN_DIR / "app_main.c").read_text(), "static void capture_task(void *arg)"
        )

    def test_stats_accumulate_before_audio_proc_mic(self):
        read = self.body.find("audio_dev_mic_read(pcm")
        acc = self.body.find("mic_samples += n")
        proc = self.body.find("audio_proc_mic(pcm")
        self.assertGreater(acc, read, "sample accumulation must follow the mic read")
        self.assertLess(acc, proc, "stats must accumulate before audio_proc_mic")
        peak_loop = self.body.find("if (v > mic_peak)")
        self.assertGreater(peak_loop, 0, "all-frame peak tracking missing")
        self.assertLess(peak_loop, proc, "peak tracking must run before audio_proc_mic")

    def test_periodic_log_reports_samples_and_elapsed(self):
        self.assertIn("mic peak=%d samples=%lu elapsed_us=%lld", self.body)
        self.assertIn("mic_log_start = now", self.body)


if __name__ == "__main__":
    unittest.main()
