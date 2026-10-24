"""Compile-and-run guard for the real mic FIFO helpers.

Extracts mic_enqueue_frame / audio_dev_mic_read / mic_record_drop /
audio_dev_mic_dropped_samples from audio_dev.c and drives a bounded
single-threaded queue stub. Covers ordering, drop accounting, partial
reads, invalid inputs, and invalid queued frames.
"""

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


FIXTURE_PREAMBLE = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define MIC_FRAME_SAMPLES 320
#define MIC_QUEUE_LEN 16
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY (-1)
typedef int BaseType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) do { (void)(m); } while (0)
#define portEXIT_CRITICAL(m)  do { (void)(m); } while (0)

typedef struct {
    int count;
    int16_t pcm[MIC_FRAME_SAMPLES];
} mic_frame_t;

/* bounded stub queue storing copies */
typedef struct { mic_frame_t items[MIC_QUEUE_LEN]; int head, len; } QStub;
typedef QStub *QueueHandle_t;
static QStub g_q;
static QueueHandle_t s_mic_queue;
static mic_frame_t s_mic_pending;
static size_t s_mic_pending_offset;
static portMUX_TYPE s_mic_stats_lock;
static uint32_t s_mic_dropped_samples;
static int g_fail_send_retry;   /* make the second xQueueSend fail once */
static int g_fail_pop_once;     /* make the next xQueueReceive pop fail */

static BaseType_t xQueueSend(QueueHandle_t q, const mic_frame_t *f, int ticks)
{
    (void)ticks;
    if (q->len >= MIC_QUEUE_LEN) return pdFALSE;
    if (g_fail_send_retry) { g_fail_send_retry = 0; return pdFALSE; }
    q->items[(q->head + q->len) % MIC_QUEUE_LEN] = *f;
    q->len++;
    return pdTRUE;
}
static BaseType_t xQueueReceive(QueueHandle_t q, mic_frame_t *f, int ticks)
{
    (void)ticks;
    if (g_fail_pop_once) { g_fail_pop_once = 0; return pdFALSE; }
    if (q->len == 0) return pdFALSE;
    *f = q->items[q->head];
    q->head = (q->head + 1) % MIC_QUEUE_LEN;
    q->len--;
    return pdTRUE;
}
"""

FIXTURE_MAIN = r"""
static int g_fails;
static void check(const char *name, bool cond)
{
    if (!cond) { g_fails++; printf("FAIL %s\n", name); }
}
static void reset(void)
{
    g_q.head = g_q.len = 0;
    s_mic_queue = &g_q;
    memset(&s_mic_pending, 0, sizeof(s_mic_pending));
    s_mic_pending_offset = 0;
    s_mic_dropped_samples = 0;
    g_fail_send_retry = g_fail_pop_once = 0;
}
static void push(int idx)   /* frame idx: pcm[j] = idx*320 + j */
{
    mic_frame_t f = { .count = MIC_FRAME_SAMPLES };
    for (int j = 0; j < MIC_FRAME_SAMPLES; j++) f.pcm[j] = (int16_t)(idx * 320 + j);
    mic_enqueue_frame(&f);
}
static int read_first(int16_t *out)
{
    int16_t buf[MIC_FRAME_SAMPLES];
    int n = audio_dev_mic_read(buf, MIC_FRAME_SAMPLES);
    if (n > 0) memcpy(out, buf, n * sizeof(int16_t));
    return n;
}
static bool frame_is(int16_t *buf, int idx, int off, int n)
{
    for (int j = 0; j < n; j++)
        if (buf[j] != (int16_t)(idx * 320 + off + j)) return false;
    return true;
}

int main(void)
{
    int16_t out[MIC_FRAME_SAMPLES];

    /* ordered 16 frames, no drops */
    reset();
    for (int i = 0; i < 16; i++) push(i);
    check("normal depth", g_q.len == 16);
    for (int i = 0; i < 16; i++) {
        int n = read_first(out);
        check("ordered", n == 320 && frame_is(out, i, 0, 320));
    }
    check("no drops", audio_dev_mic_dropped_samples() == 0);

    /* full queue + one more -> oldest dropped, reader sees frame 1 */
    reset();
    for (int i = 0; i < 17; i++) push(i);
    check("drop oldest", audio_dev_mic_dropped_samples() == 320);
    int n = read_first(out);
    check("first after drop", n == 320 && frame_is(out, 1, 0, 320));

    /* pop succeeds but retry push fails -> old 320 + incoming 320 counted */
    reset();
    for (int i = 0; i < 16; i++) push(i);
    g_fail_send_retry = 1;
    push(17);
    check("pop ok + push fail = 640", audio_dev_mic_dropped_samples() == 640);

    /* pop fails AND push fails -> only incoming 320 counted */
    reset();
    for (int i = 0; i < 16; i++) push(i);
    g_fail_pop_once = 1;
    g_fail_send_retry = 1;
    push(17);
    check("pop fail + push fail = 320", audio_dev_mic_dropped_samples() == 320);

    /* partial reads: 100 + 220 from frame0, then over-size ask gets frame1 */
    reset();
    push(0); push(1);
    n = audio_dev_mic_read(out, 100);
    check("partial 100", n == 100 && frame_is(out, 0, 0, 100));
    n = audio_dev_mic_read(out, 220);
    check("partial 220", n == 220 && frame_is(out, 0, 100, 220));
    n = audio_dev_mic_read(out, 1000);
    check("oversize gets next frame", n == 320 && frame_is(out, 1, 0, 320));

    /* invalid args and unready queue reject without consuming */
    reset();
    push(0);
    check("null out", audio_dev_mic_read(NULL, 10) == -1);
    check("zero n", audio_dev_mic_read(out, 0) == -1);
    check("neg n", audio_dev_mic_read(out, -5) == -1);
    s_mic_queue = NULL;
    check("unready", audio_dev_mic_read(out, 10) == -1);
    s_mic_queue = &g_q;
    n = audio_dev_mic_read(out, 320);
    check("frame survives rejects", n == 320 && frame_is(out, 0, 0, 320));

    /* invalid queued count rejected and reset; next valid frame works */
    reset();
    mic_frame_t bad = { .count = 0 };
    xQueueSend(&g_q, &bad, 0);
    check("bad count rejected", audio_dev_mic_read(out, 320) == -1);
    push(3);
    n = audio_dev_mic_read(out, 320);
    check("valid after bad", n == 320 && frame_is(out, 3, 0, 320));

    if (g_fails == 0) printf("ALL PASS\n");
    return g_fails;
}
"""


class TestMicCaptureQueue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = (MAIN_DIR / "audio_dev.c").read_text()
        duts = "\n".join(
            _extract_function(src, sig)
            for sig in (
                "static void mic_record_drop(uint32_t samples)",
                "uint32_t audio_dev_mic_dropped_samples(void)",
                "static void mic_enqueue_frame(const mic_frame_t *frame)",
                "int audio_dev_mic_read(int16_t *out, int samples)",
            )
        )
        with tempfile.TemporaryDirectory() as d:
            c_path = Path(d) / "mq.c"
            bin_path = Path(d) / "mq"
            c_path.write_text(FIXTURE_PREAMBLE + duts + FIXTURE_MAIN)
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Werror",
                 "-o", str(bin_path), str(c_path)],
                check=True, capture_output=True, text=True,
            )
            res = subprocess.run([str(bin_path)], capture_output=True, text=True)
            cls.stdout = res.stdout
            cls.returncode = res.returncode

    def test_queue_cases(self):
        self.assertEqual(self.stdout.count("FAIL"), 0, self.stdout)
        self.assertIn("ALL PASS", self.stdout)
        self.assertEqual(self.returncode, 0)


class TestMicCaptureSourceGuards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = (MAIN_DIR / "audio_dev.c").read_text()
        cls.task = _extract_function(
            cls.src, "static void mic_capture_task(void *arg)"
        )
        cls.init = _extract_function(
            cls.src, "esp_err_t audio_dev_init(void)"
        )

    def test_only_producer_reads_hardware(self):
        self.assertEqual(self.src.count("i2s_channel_read"), 1,
                         "exactly one physical mic read site")
        self.assertIn("mic_read_i2s(frame.pcm, MIC_FRAME_SAMPLES)", self.task)
        self.assertIn("mic_enqueue_frame(&frame)", self.task)
        for forbidden in ("opus", "linx_", "rtc_peer", "wake_", "ESP_LOG",
                          "malloc", "heap_caps"):
            self.assertNotIn(forbidden, self.task,
                             f"producer task must not call {forbidden}")

    def test_fixed_internal_fifo_and_task_macros(self):
        self.assertIn("#define MIC_QUEUE_LEN     16", self.src)
        self.assertIn("#define MIC_TASK_STACK    4096", self.src)
        self.assertIn("#define MIC_TASK_PRIORITY 19", self.src)
        self.assertIn("#define MIC_TASK_CORE     ((portNUM_PROCESSORS > 1) ? 1 : 0)",
                      self.src)
        self.assertIn(
            "xQueueCreateWithCaps(MIC_QUEUE_LEN, sizeof(mic_frame_t),\n"
            "                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
            self.init,
        )
        self.assertIn(
            'xTaskCreatePinnedToCoreWithCaps(mic_capture_task, "aud_mic"',
            self.init,
        )

    def test_acquisition_preempts_peer_handshake(self):
        mic = re.search(r"#define\s+MIC_TASK_PRIORITY\s+(\d+)", self.src)
        peer_src = (MAIN_DIR / "rtc_peer.c").read_text()
        peer = re.search(
            r'xTaskCreatePinnedToCore\(\s*pc_task,\s*"pc_task",[^,]+,\s*s_peer,\s*(\d+)\s*,',
            peer_src,
        )
        self.assertIsNotNone(mic)
        self.assertIsNotNone(peer)
        self.assertGreater(int(mic.group(1)), int(peer.group(1)))

    def test_diag_fields_and_helpers_present(self):
        self.assertIn("audio_dev_mic_dropped_samples(void)", self.src)
        self.assertIn("audio_dev_mic_stack_hwm(void)", self.src)
        app = (MAIN_DIR / "app_main.c").read_text()
        self.assertIn("mic_dropped_samples=%lu", app)
        self.assertIn("mic_stack_hwm=%u", app)


if __name__ == "__main__":
    unittest.main()
