"""Behavior regression tests for the Linx reply-gating policy.

The real update_linx_gate / open_tts_window / on_linx_tts bodies are extracted
from app_main.c and compiled into a small C fixture with stub dependencies, so
the assertions exercise the shipped logic rather than a reimplementation.
Plus source guards for disconnect/abort cleanup and the rx-accepted counter.
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

MAIN_DIR = Path(__file__).resolve().parent.parent / "main"
APP_MAIN = (MAIN_DIR / "app_main.c").read_text()


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


FIXTURE_PREAMBLE = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define AUDIO_SRC_LINX 0
#define AUDIO_SRC_ROOM 1
#define TTS_WINDOW_MS 25000
#define ESP_LOGI(tag, ...) do { (void)0; } while (0)
typedef struct { int flag; } app_event_t;

/* stub dependencies + observables */
static bool g_intercom;
static int64_t g_now_us;
static bool g_wake_enabled;
static int g_gate[64]; static int g_gate_n;
static int g_src[64];  static int g_src_n;

static bool in_intercom(void) { return g_intercom; }
static int64_t esp_timer_get_time(void) { return g_now_us; }
static void linx_set_playback_gate(bool a) { if (g_gate_n < 64) g_gate[g_gate_n] = a; g_gate_n++; }
static void audio_rx_set_source(int s) { if (g_src_n < 64) g_src[g_src_n] = s; g_src_n++; }
static void wake_word_enable(bool e) { g_wake_enabled = e; }

/* device-under-test state (mirrors app_main.c storage) */
static volatile bool s_linx_playing;
static bool s_linx_reply_pending;
static volatile bool s_wake_listen;
static volatile bool s_command_active;
static int64_t s_tts_window_until;
"""

FIXTURE_HARNESS = r"""
static int g_fails;
static void reset(void)
{
    g_intercom = false; g_now_us = 0; g_wake_enabled = false;
    g_gate_n = 0; g_src_n = 0;
    s_linx_playing = false; s_linx_reply_pending = false;
    s_wake_listen = false; s_command_active = false;
    s_tts_window_until = 0;
}
static void check(const char *name, bool cond)
{
    if (!cond) { g_fails++; printf("FAIL %s\n", name); }
}
static int last_gate(void) { return g_gate_n ? g_gate[g_gate_n - 1] : -1; }
static int last_src(void)  { return g_src_n ? g_src[g_src_n - 1] : -1; }
static void tts(int flag) { app_event_t ev = { .flag = flag }; on_linx_tts(&ev); }

int main(void)
{
    /* 1. in room, nothing pending: gate stays on the room */
    reset(); g_intercom = true;
    update_linx_gate();
    check("case1 gate room", last_gate() == 0 && last_src() == AUDIO_SRC_ROOM);

    /* 2. expected reply armed but TTS not started: room keeps downlink */
    reset(); g_intercom = true;
    open_tts_window();
    check("case2 still room", last_gate() == 0 && last_src() == AUDIO_SRC_ROOM);

    /* 3. expected TTS start during wake round: switch to linx, rearm wake */
    reset(); g_intercom = true; s_wake_listen = true;
    open_tts_window();
    tts(1);
    check("case3 gate linx", last_gate() == 1 && last_src() == AUDIO_SRC_LINX);
    check("case3 listen closed", s_wake_listen == false);
    check("case3 wake rearmed", g_wake_enabled == true);
    check("case3 playing+pending", s_linx_playing && s_linx_reply_pending);

    /* 4. repeated sentence_start keeps linx gate */
    reset(); g_intercom = true;
    open_tts_window(); tts(1); tts(1); tts(1);
    check("case4 stays linx", last_gate() == 1 && last_src() == AUDIO_SRC_LINX);

    /* 5. tts stop: playing+pending cleared, back to room */
    reset(); g_intercom = true;
    open_tts_window(); tts(1); tts(0);
    check("case5 back to room", last_gate() == 0 && last_src() == AUDIO_SRC_ROOM);
    check("case5 cleared", !s_linx_playing && !s_linx_reply_pending);

    /* 6. unsolicited tts start in room: ignored, room audio kept */
    reset(); g_intercom = true;
    tts(1);
    check("case6 ignored", s_linx_playing == false);
    check("case6 room kept", last_gate() == 0 && last_src() == AUDIO_SRC_ROOM);

    /* 7. unsolicited tts start while idle (linx mode): plays */
    reset(); g_intercom = false;
    tts(1);
    check("case7 idle plays", last_gate() == 1 && last_src() == AUDIO_SRC_LINX);

    /* 8. expected reply lands after the 25 s window expired: refresh + play */
    reset(); g_intercom = true;
    g_now_us = 0;
    open_tts_window();                 /* deadline = 25 s */
    g_now_us = 30000 * 1000;           /* 30 s later, window expired */
    tts(1);
    check("case8 late reply plays", last_gate() == 1 && last_src() == AUDIO_SRC_LINX);
    check("case8 deadline refreshed",
          s_tts_window_until == g_now_us + (int64_t)TTS_WINDOW_MS * 1000);

    /* 9. after an in-room stop, a new expected round works again */
    reset(); g_intercom = true;
    open_tts_window(); tts(1); tts(0);          /* first round done -> room */
    check("case9 room after stop", last_src() == AUDIO_SRC_ROOM);
    open_tts_window();                           /* next request arms reply */
    check("case9 armed still room", last_src() == AUDIO_SRC_ROOM);
    tts(1);
    check("case9 new reply plays", last_gate() == 1 && last_src() == AUDIO_SRC_LINX);

    if (g_fails == 0) { printf("ALL PASS\n"); }
    return g_fails;
}
"""


def _build_fixture(tmp: Path) -> Path:
    duts = "\n".join(
        _extract_function(APP_MAIN, sig)
        for sig in (
            "static void update_linx_gate(void)",
            "static void open_tts_window(void)",
            "static void on_linx_tts(app_event_t *ev)",
        )
    )
    src = FIXTURE_PREAMBLE + duts + FIXTURE_HARNESS
    c_path = tmp / "fixture.c"
    bin_path = tmp / "fixture"
    c_path.write_text(src)
    subprocess.run(
        ["cc", "-std=c99", "-Wall", "-Werror", "-o", str(bin_path), str(c_path)],
        check=True, capture_output=True, text=True,
    )
    return bin_path


class TestLinxAudioPolicyBehavior(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as d:
            binary = _build_fixture(Path(d))
            res = subprocess.run([str(binary)], capture_output=True, text=True)
            cls.stdout = res.stdout
            cls.returncode = res.returncode

    def test_policy_cases(self):
        self.assertEqual(self.stdout.count("FAIL"), 0, self.stdout)
        self.assertIn("ALL PASS", self.stdout)
        self.assertEqual(self.returncode, 0)


class TestLinxPolicySourceGuards(unittest.TestCase):
    """Text-level guards for cleanup paths and the rx-accepted counter."""

    def test_disconnect_clears_playing_pending_and_updates_gate(self):
        body = _extract_function(APP_MAIN, "static void on_linx_disconnected(void)")
        self.assertIn("s_linx_playing = false;", body)
        self.assertIn("s_linx_reply_pending = false;", body)
        self.assertIn("s_tts_window_until = esp_timer_get_time();", body)
        self.assertIn("update_linx_gate();", body)

    def test_abort_clears_playing_pending_and_updates_gate(self):
        start = APP_MAIN.find("case APP_EV_LINX_ABORT:")
        end = APP_MAIN.find("break;", start)
        body = APP_MAIN[start:end]
        self.assertIn("s_linx_playing = false;", body)
        self.assertIn("s_linx_reply_pending = false;", body)
        self.assertIn("s_tts_window_until = esp_timer_get_time();", body)
        self.assertIn("update_linx_gate();", body)

    def test_rx_accepted_counter_format_and_filter(self):
        src = (MAIN_DIR / "audio_dev.c").read_text()
        body = _extract_function(
            src, "void audio_rx_enqueue_src(int src, const uint8_t *data, int size)"
        )
        self.assertIn('"audio rx accepted src=%d n=%lu"', body)
        filt = body.find("src == AUDIO_SRC_LINX || src == AUDIO_SRC_ROOM")
        inc = body.find("++s_rx_accepted[src]")
        enqueue = body.find("audio_rx_enqueue_tagged(src, data, size)")
        self.assertTrue(0 < filt < inc < enqueue,
                        "source filter must gate the increment before enqueue")
        self.assertIn("AUDIO_SRC_TONE", body)  # tone path still bypasses the gate

    def test_capture_loop_sends_are_bounded_below_fifo_depth(self):
        src = (MAIN_DIR / "linx_client.c").read_text()
        audio = _extract_function(
            src, "int linx_send_audio(const uint8_t *data, int size)")
        self.assertNotIn("portMAX_DELAY", audio,
                         "audio frames must drop on lock contention, not stall")
        audio_tmo = re.search(r"send_bin\(s_ws, \(const char \*\)data, size,\s*"
                              r"pdMS_TO_TICKS\((\d+)\)\)", audio)
        self.assertIsNotNone(audio_tmo)
        self.assertLessEqual(int(audio_tmo.group(1)) + 30, 320,
                             "audio lock wait + send timeout must stay under "
                             "the 320 ms mic FIFO")
        listen = _extract_function(
            src, 'esp_err_t linx_send_listen(const char *state, const char *mode)')
        listen_call = re.search(
            r"linx_send_json_tmo\(root, pdMS_TO_TICKS\((\d+)\),\s*"
            r"pdMS_TO_TICKS\((\d+)\)\)", listen)
        self.assertIsNotNone(listen_call,
                             "listen start/stop runs inside the capture loop")
        self.assertLessEqual(
            int(listen_call.group(1)) + int(listen_call.group(2)), 320,
            "listen lock wait + send timeout must stay under the 320 ms mic FIFO")


if __name__ == "__main__":
    unittest.main()
