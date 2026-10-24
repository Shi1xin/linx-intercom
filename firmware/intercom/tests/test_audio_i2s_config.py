"""Source guards for explicit left-slot 32-bit mono capture and diagnostic placement. On-target audio behavior is verified separately."""

import re
import subprocess
import tempfile
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
        self.assertIn("mic peak=%d clip=%lu samples=%lu elapsed_us=%lld", self.body)
        self.assertIn("mic_log_start = now", self.body)


class TestCaptureStageTrace(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (MAIN_DIR / "app_main.c").read_text()
        cls.body = _extract_function(cls.source, "static void capture_task(void *arg)")
        cls.helper = _extract_function(
            cls.source, "static void capture_trace_stage(const char *stage, int64_t started_us)"
        )

    def _assert_calls_traced(self, call: str, marker: str, min_count: int):
        starts = [m.start() for m in re.finditer(re.escape(call), self.body)]
        self.assertGreaterEqual(len(starts), min_count,
                                f"expected >= {min_count} {call} call sites, found {len(starts)}")
        for at in starts:
            semi = self.body.find(";", at)
            self.assertGreater(semi, at, f"{call} has no terminating ';'")
            rest = self.body[semi + 1:]
            self.assertRegex(
                rest,
                r"^\s*" + re.escape(marker),
                f"next statement after {call} must be {marker}",
            )

    def test_linx_send_audio_wrapped(self):
        self._assert_calls_traced(
            "linx_send_audio(pkt, enc)", 'capture_trace_stage("linx_audio"', 2
        )

    def test_rtc_send_audio_wrapped(self):
        self._assert_calls_traced(
            "rtc_peer_send_audio(pkt, enc, pts)", 'capture_trace_stage("rtc_audio"', 2
        )

    def test_slow_stage_log_threshold_and_format(self):
        self.assertIn("elapsed <= 40000", self.helper)  # warn above 40 ms
        self.assertIn('"capture stage=%s elapsed_us=%lld"', self.helper)
        self.assertIn('"capture stage=read max_us=%lld stalls=%u"', self.helper)
        self.assertIn("esp_timer_get_time() - started_us", self.helper)


class TestPsramStackCreation(unittest.TestCase):
    """Source guards for WithCaps stack placement of aud_cap/aud_play."""

    def test_capture_task_uses_withcaps_psram_fallback_checked(self):
        src = (MAIN_DIR / "app_main.c").read_text()
        self.assertIn('#include "freertos/idf_additions.h"', src)
        self.assertRegex(
            src,
            r"#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM"
            r"[\s\S]*?MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT"
            r"[\s\S]*?#else[\s\S]*?MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT",
        )
        self.assertRegex(
            src,
            r"xTaskCreatePinnedToCoreWithCaps\(\s*capture_task,\s*\"aud_cap\",\s*40 \* 1024",
        )
        self.assertNotIn(
            'xTaskCreatePinnedToCore(capture_task, "aud_cap"', src,
            "aud_cap must use the WithCaps variant",
        )
        self.assertRegex(src, r"== pdPASS\s*\?\s*ESP_OK : ESP_ERR_NO_MEM")

    def test_playback_task_uses_withcaps_psram_fallback_checked(self):
        src = (MAIN_DIR / "audio_dev.c").read_text()
        self.assertIn('#include "freertos/idf_additions.h"', src)
        self.assertRegex(
            src,
            r"#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM"
            r"[\s\S]*?MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT"
            r"[\s\S]*?#else[\s\S]*?MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT",
        )
        self.assertRegex(
            src,
            r"xTaskCreatePinnedToCoreWithCaps\(\s*playback_task,\s*\"aud_play\",\s*30 \* 1024",
        )
        self.assertNotIn(
            'xTaskCreatePinnedToCore(playback_task, "aud_play"', src,
            "aud_play must use the WithCaps variant",
        )
        self.assertIn("ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM", src)

    def test_resource_log_fields(self):
        body = _extract_function(
            (MAIN_DIR / "app_main.c").read_text(), "static void capture_task(void *arg)"
        )
        self.assertIn(
            "mic resources internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u stack_hwm=%u mic_overflows=%lu",
            body,
        )
        self.assertIn("heap_caps_get_free_size(MALLOC_CAP_INTERNAL)", body)
        self.assertIn("heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)", body)
        self.assertIn("uxTaskGetStackHighWaterMark(NULL)", body)


OVERFLOW_FIXTURE_PREAMBLE = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* stub platform types + lock macros (count calls, no locking semantics) */
typedef void *i2s_chan_handle_t;
typedef struct { int dummy; } i2s_event_data_t;
#define IRAM_ATTR
typedef int portMUX_TYPE;
static int g_enter_isr, g_exit_isr, g_enter, g_exit;
#define portENTER_CRITICAL_ISR(m) do { g_enter_isr++; (void)(m); } while (0)
#define portEXIT_CRITICAL_ISR(m)  do { g_exit_isr++; (void)(m); } while (0)
#define portENTER_CRITICAL(m)     do { g_enter++; (void)(m); } while (0)
#define portEXIT_CRITICAL(m)      do { g_exit++; (void)(m); } while (0)
static portMUX_TYPE s_mic_stats_lock;
static uint32_t s_mic_rx_overflows;
"""

OVERFLOW_FIXTURE_MAIN = r"""
int main(void)
{
    int fails = 0;
    if (audio_dev_mic_rx_overflows() != 0) { fails++; printf("FAIL initial\n"); }
    int returned_false = 0;
    for (int i = 0; i < 100; i++) {
        if (on_mic_rx_overflow(NULL, NULL, NULL) == false) returned_false++;
    }
    if (returned_false != 100) { fails++; printf("FAIL ret %d\n", returned_false); }
    if (audio_dev_mic_rx_overflows() != 100) { fails++; printf("FAIL count\n"); }
    if (g_enter_isr != 100 || g_exit_isr != 100) {
        fails++; printf("FAIL isr cs %d/%d\n", g_enter_isr, g_exit_isr);
    }
    if (g_enter != 2 || g_exit != 2) {
        fails++; printf("FAIL getter cs %d/%d\n", g_enter, g_exit);
    }
    if (fails == 0) printf("ALL PASS\n");
    return fails;
}
"""


class TestMicOverflowCounter(unittest.TestCase):
    """Compile-and-run guard for the ISR overflow counter + getter."""

    @classmethod
    def setUpClass(cls):
        src = (MAIN_DIR / "audio_dev.c").read_text()
        duts = "\n".join(
            _extract_function(src, sig)
            for sig in (
                "static bool IRAM_ATTR on_mic_rx_overflow(i2s_chan_handle_t handle,",
                "uint32_t audio_dev_mic_rx_overflows(void)",
            )
        )
        cls.stdout = ""
        with tempfile.TemporaryDirectory() as d:
            c_path = Path(d) / "ovf.c"
            bin_path = Path(d) / "ovf"
            c_path.write_text(
                OVERFLOW_FIXTURE_PREAMBLE + duts + OVERFLOW_FIXTURE_MAIN
            )
            subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Werror",
                 "-o", str(bin_path), str(c_path)],
                check=True, capture_output=True, text=True,
            )
            res = subprocess.run([str(bin_path)], capture_output=True, text=True)
            cls.stdout = res.stdout
            cls.returncode = res.returncode

    def test_overflow_counter_behavior(self):
        self.assertEqual(self.stdout.count("FAIL"), 0, self.stdout)
        self.assertIn("ALL PASS", self.stdout)
        self.assertEqual(self.returncode, 0)


class TestMicOverflowSourceGuards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = (MAIN_DIR / "audio_dev.c").read_text()
        cls.rx = _extract_function(cls.src, "static esp_err_t i2s_rx_init(void)")

    def test_callback_registered_after_init_before_enable(self):
        init = self.rx.find("i2s_channel_init_std_mode(s_rx")
        cb = self.rx.find("i2s_channel_register_event_callback(s_rx")
        on_ovf = self.rx.find(".on_recv_q_ovf = on_mic_rx_overflow")
        en = self.rx.find("i2s_channel_enable(s_rx)")
        self.assertTrue(0 < init < cb < en, "callback must register between init and enable")
        self.assertGreater(on_ovf, 0)

    def test_rx_dma_desc_num_12_tx_unchanged(self):
        self.assertIn("#define MIC_DMA_DESC_NUM 12", self.src)
        self.assertIn(".dma_desc_num = MIC_DMA_DESC_NUM", self.rx)
        tx = _extract_function(self.src, "static esp_err_t i2s_tx_init(void)")
        self.assertIn(".dma_desc_num = DMA_DESC_NUM", tx)
        self.assertIn("#define DMA_DESC_NUM     6", self.src)

    def test_isr_no_alloc_or_log(self):
        body = _extract_function(
            self.src,
            "static bool IRAM_ATTR on_mic_rx_overflow(i2s_chan_handle_t handle,",
        )
        self.assertNotIn("ESP_LOG", body)
        self.assertNotIn("malloc", body)
        self.assertNotIn("heap_caps", body)

    def test_wake_flags_atomic_and_inject_reset_order(self):
        wsrc = (MAIN_DIR / "wake_word.c").read_text()
        self.assertIn("#include <stdatomic.h>", wsrc)
        self.assertIn("static atomic_bool s_enabled;", wsrc)
        self.assertIn("static atomic_bool s_mic_hold;", wsrc)
        inj = _extract_function(wsrc, "static void inject_task(void *arg)")
        feed_reset = inj.rfind("s_feed_len = 0")
        unhold = inj.rfind("s_mic_hold = false")
        self.assertTrue(0 < feed_reset < unhold,
                        "inject must drain feed_len before releasing mic hold")


TRACE_FIXTURE_PREAMBLE = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>

#define TRACE_SAMPLES 8   /* shrunk for the fixture; real value is 16000*8 */
typedef int SemaphoreHandle_t;
static int g_sem_gives;
#define xSemaphoreGive(s) do { g_sem_gives++; (void)(s); } while (0)

/* device-under-test state (mirrors wake_trace.c storage) */
static int16_t s_ring[TRACE_SAMPLES];
static int16_t s_snapshot[TRACE_SAMPLES];
static size_t s_write;
static size_t s_filled;
static size_t s_snapshot_samples;
static atomic_bool s_ready;
static atomic_bool s_requested;
static atomic_bool s_busy;
static SemaphoreHandle_t s_upload;
"""

TRACE_FIXTURE_MAIN = r"""
static int g_fails;
static void check(const char *name, bool cond)
{
    if (!cond) { g_fails++; printf("FAIL %s\n", name); }
}
static void reset(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    memset(s_snapshot, 0, sizeof(s_snapshot));
    s_write = s_filled = s_snapshot_samples = 0;
    s_ready = true; s_requested = false; s_busy = false;
    g_sem_gives = 0;
}
static bool snap_eq(const int16_t *exp, size_t n)
{
    if (s_snapshot_samples != n) return false;
    return memcmp(s_snapshot, exp, n * sizeof(int16_t)) == 0;
}
static void feed_seq(int first, int n)   /* feeds samples first..first+n-1 */
{
    int16_t buf[32];
    for (int i = 0; i < n; i++) buf[i] = (int16_t)(first + i);
    wake_trace_feed(buf, n);
}

int main(void)
{
    /* zero/NULL safety */
    reset();
    wake_trace_feed(NULL, 5);
    int16_t one = 7;
    wake_trace_feed(&one, 0);
    check("zero-null safe", s_filled == 0 && g_sem_gives == 0);
    reset(); s_ready = false;
    wake_trace_feed(&one, 1);
    check("not ready", s_filled == 0);

    /* request drains on the next feed; snapshot includes it: [1,2,3,4] */
    reset();
    feed_seq(1, 3);
    s_requested = true;
    feed_seq(4, 1);          /* the draining feed is part of the snapshot */
    check("snap1", snap_eq((int16_t[]){1,2,3,4}, 4));
    check("give1", g_sem_gives == 1);

    /* wrap: ring holds 5..12; request drains on feed 13 -> last 8 [6..13] */
    reset();
    feed_seq(1, 8);
    feed_seq(9, 4);
    s_requested = true;
    feed_seq(13, 1);
    check("wrap last8", snap_eq((int16_t[]){6,7,8,9,10,11,12,13}, 8));

    /* busy keeps snapshot immutable; request stays pending */
    reset();
    feed_seq(1, 8);
    s_requested = true;
    feed_seq(9, 1);                       /* snapshot [1..8], busy set */
    int16_t before[8]; memcpy(before, s_snapshot, sizeof(before));
    s_requested = true;                   /* request while busy */
    feed_seq(10, 4);                      /* feeds proceed; no new snapshot */
    check("busy immutable", memcmp(before, s_snapshot, sizeof(before)) == 0);
    check("busy pending", s_requested == true && g_sem_gives == 1);
    s_busy = false;                       /* upload task finishes */
    feed_seq(14, 1);                      /* next feed drains pending */
    check("post busy snap", snap_eq((int16_t[]){7,8,9,10,11,12,13,14}, 8));

    /* 17 samples wrap; drain feed adds 18 -> last 8 [11..18] */
    reset();
    feed_seq(1, 17);
    s_requested = true;
    feed_seq(18, 1);
    check("17 wrap", snap_eq((int16_t[]){11,12,13,14,15,16,17,18}, 8));

    if (g_fails == 0) printf("ALL PASS\n");
    return g_fails;
}
"""


class TestWakeTraceRing(unittest.TestCase):
    """Compile-and-run guard for the real PCM ring/snapshot logic."""

    @classmethod
    def setUpClass(cls):
        src = (MAIN_DIR / "wake_trace.c").read_text()
        duts = "\n".join(
            _extract_function(src, sig)
            for sig in (
                "static void prepare_snapshot(void)",
                "void wake_trace_feed(const int16_t *pcm, size_t samples)",
            )
        )
        with tempfile.TemporaryDirectory() as d:
            c_path = Path(d) / "tr.c"
            bin_path = Path(d) / "tr"
            c_path.write_text(
                TRACE_FIXTURE_PREAMBLE + duts + TRACE_FIXTURE_MAIN
            )
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Werror",
                 "-o", str(bin_path), str(c_path)],
                check=True, capture_output=True, text=True,
            )
            res = subprocess.run([str(bin_path)], capture_output=True, text=True)
            cls.stdout = res.stdout
            cls.returncode = res.returncode

    def test_ring_snapshot_cases(self):
        self.assertEqual(self.stdout.count("FAIL"), 0, self.stdout)
        self.assertIn("ALL PASS", self.stdout)
        self.assertEqual(self.returncode, 0)


class TestWakeTraceSourceGuards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = (MAIN_DIR / "wake_trace.c").read_text()
        cls.wake = (MAIN_DIR / "wake_word.c").read_text()
        cls.app = (MAIN_DIR / "app_main.c").read_text()
        cls.kconfig = (MAIN_DIR / "Kconfig.projbuild").read_text()

    def test_config_default_off(self):
        m = re.search(
            r"config INTERCOM_WAKE_PCM_TRACE\n(.*?)\n\s*config ",
            self.kconfig, re.S,
        )
        self.assertIsNotNone(m, "INTERCOM_WAKE_PCM_TRACE block not found")
        block = m[1]
        self.assertIn("bool", block)
        self.assertIn("depends on SPIRAM", block)
        self.assertRegex(block, r"\bdefault n\b")

    def test_upload_bounded_and_authenticated(self):
        self.assertIn(".timeout_ms = 5000", self.src)
        self.assertIn(".disable_auto_redirect = true", self.src)
        self.assertIn('"Bearer " CONFIG_INTERCOM_WAKE_PCM_TRACE_TOKEN', self.src)
        self.assertIn('"X-Linx-PCM-SHA256"', self.src)

    def test_feed_hook_after_afe_feed(self):
        feed = self.wake.find("s_afe_iface->feed(s_afe, s_feed_buf + off)")
        hook = self.wake.find("wake_trace_feed(s_feed_buf + off, s_feed_chunk)")
        self.assertTrue(0 < feed < hook,
                        "trace hook must follow the AFE feed call")

    def test_request_only_on_unmute_falling_edge(self):
        body = _extract_function(
            self.app, "static void apply_mute(bool muted)"
        )
        self.assertIn("was_muted && !muted", body)
        self.assertIn("wake_trace_request();", body)


if __name__ == "__main__":
    unittest.main()
