"""Compile-and-run test for clip_sched.c and tts_watchdog.h.

Compiles the real scheduler against a stub opus_frame_t and drives the
producer/consumer in the interleavings from the slot-lifecycle review:
select-then-flush/rearm, fetch-then-flush/rearm, and completing a sequence
while a newer one is already published.
"""

import subprocess
import tempfile
import unittest
from pathlib import Path

MAIN_DIR = Path(__file__).resolve().parent.parent / "main"

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "opus_frame_stub.h"
#include "clip_sched_stub.h"
#include "tts_watchdog.h"

static const uint8_t A1=1,A2=2,B1=3,B2=4,B3=5;
static opus_frame_t seqA[2] = {{&A1,1},{&A2,1}};
static opus_frame_t seqB[3] = {{&B1,1},{&B2,1},{&B3,1}};

static int fails;
#define CHK(cond, tag) do { if (!(cond)) { printf("FAIL %s\n", tag); fails++; } } while (0)

/* injected by the test wrapper into clip_sched.c to poke the mailbox gen */
void probe_set_gen_odd(void);

int main(void) {
    /* basic order */
    CHK(clip_sched_post(seqA, 2), "postA");
    const opus_frame_t *f = clip_sched_next();
    CHK(f && f->data == &A1, "A1");
    f = clip_sched_next();
    CHK(f && f->data == &A2, "A2");
    CHK(clip_sched_next() == NULL, "A-done");
    CHK(clip_sched_done_frames() == 2, "A-done-n");

    /* select-then-flush+rearm: playback holds cursor on A; producer flushes
     * and posts B; next frames must be B's — stale A never resumes */
    clip_sched_post(seqA, 2);
    clip_sched_next();                       /* A1 delivered */
    clip_sched_flush();
    clip_sched_post(seqB, 3);
    f = clip_sched_next();
    CHK(f && f->data == &B1, "rearm-B1");
    CHK(clip_sched_cur_gen() != 0, "genB");
    f = clip_sched_next();
    CHK(f && f->data == &B2, "rearm-B2");
    f = clip_sched_next();
    CHK(f && f->data == &B3, "rearm-B3");
    CHK(clip_sched_next() == NULL, "B-done");

    /* completing old sequence while new already published: A finishes,
     * B posted during last frame — next call yields B[0], nothing clears B */
    clip_sched_post(seqA, 2);
    clip_sched_next();                       /* A1 */
    clip_sched_post(seqB, 3);                /* B supersedes before A ends */
    f = clip_sched_next();
    CHK(f && f->data == &B1, "cut-B1");
    CHK(clip_sched_next() && clip_sched_next() && clip_sched_next() == NULL, "cut-B-rest");

    /* flush mid-play leaves scheduler idle */
    clip_sched_post(seqA, 2);
    clip_sched_next();
    clip_sched_flush();
    CHK(clip_sched_next() == NULL, "flush-idle");

    /* writer-active window: odd gen keeps the current cursor playing;
     * a flush landing while the write is active still retires the cursor */
    clip_sched_post(seqA, 2);
    clip_sched_next();                       /* A1 */
    probe_set_gen_odd();
    f = clip_sched_next();
    CHK(f && f->data == &A2, "writer-hold"); /* stays on current while write lands */
    clip_sched_flush();
    CHK(clip_sched_next() == NULL, "hold-done");

    /* invalid input */
    CHK(!clip_sched_post(NULL, 2), "inv-null");
    CHK(!clip_sched_post(seqA, 0), "inv-zero");

    /* tts_round_expired truth table */
    int64_t t0 = 100000000;
    /* frames flowing: not expired even past window */
    CHK(!tts_round_expired(t0 + 40000000, t0, t0 + 39500000), "alive-long");
    /* frames began then stopped >5s: expired */
    CHK(tts_round_expired(t0 + 6000000, t0, t0 + 500000), "idle-dead");
    /* start produced no audio within dead window: expired */
    CHK(tts_round_expired(t0 + 26000000, t0, t0 - 100000), "noaudio-dead");
    /* fresh round, no frames yet: alive */
    CHK(!tts_round_expired(t0 + 2000000, t0, t0 - 99000000), "fresh-alive");
    /* last_frame from an older round must not block expiry */
    CHK(tts_round_expired(t0 + 30000000, t0, t0 - 60000000), "oldframe-dead");

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails;
}
"""


def build_and_run() -> str:
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        body = (MAIN_DIR / "clip_sched.c").read_text()
        body = body.replace('#include "clip_sched.h"', '#include "clip_sched_stub.h"')
        # expose mailbox for the writer-active probe
        body = body.replace("} s_box;", "} s_box;\nvoid probe_set_gen_odd(void){s_box.gen|=1;}")
        (td / "clip_sched_probe.h").write_text("")
        (td / "opus_frame_stub.h").write_text(
            "#pragma once\n#include <stdint.h>\n"
            "typedef struct { const uint8_t *data; uint16_t len; } opus_frame_t;\n")
        (td / "clip_sched_stub.h").write_text(
            "#pragma once\n#include <stdbool.h>\n#include <stdint.h>\n"
            '#include "opus_frame_stub.h"\n'
            "#define CLIP_FRAMES_CAP 384\n"
            "bool clip_sched_post(const opus_frame_t *frames, int nframes);\n"
            "void clip_sched_flush(void);\n"
            "const opus_frame_t *clip_sched_next(void);\n"
            "uint32_t clip_sched_cur_gen(void);\n"
            "int clip_sched_done_frames(void);\n")
        (td / "clip_sched.c").write_text(body)
        (td / "tts_watchdog.h").write_text((MAIN_DIR / "tts_watchdog.h").read_text())
        (td / "main.c").write_text(HARNESS)
        subprocess.run(["cc", "-O1", "-o", str(td / "t"),
                        str(td / "main.c"), str(td / "clip_sched.c"),
                        "-I", str(td)], check=True,
                       capture_output=True)
        return subprocess.run([str(td / "t")], capture_output=True,
                              text=True).stdout


class ClipSchedTest(unittest.TestCase):
    def test_lifecycle(self):
        out = build_and_run()
        self.assertIn("PASS", out, msg=out)


if __name__ == "__main__":
    unittest.main()
