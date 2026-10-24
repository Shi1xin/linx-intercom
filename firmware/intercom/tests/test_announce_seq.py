"""Compile-and-run guard for announce.c sequence building.

Compiles the real announce.c against stub headers and asserts the clip
sequences emitted for room names that decompose ("二号房") and names that
fall back to invite-code readout ("客厅").
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
#include "announce_clips.h"

static opus_frame_t played[512];
static int played_n;
static int play_calls;
bool audio_rx_play_frames(const opus_frame_t *f, int n) {
    memcpy(played, f, n * sizeof(opus_frame_t));
    played_n = n; play_calls++;
    return true;
}
#define ESP_LOGI(tag, ...) do { (void)0; } while (0)
#define ESP_LOGW(tag, ...) do { (void)0; } while (0)
static const char *TAG_ = "announce";

/* splice: announce.c expects TAG/ESP_LOG* already defined */
#define TAG "announce"
#include "announce_body.h"

static int same(const opus_frame_t *a, const opus_frame_t *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i].data != b[i].data || a[i].len != b[i].len) return 0;
    return 1;
}
static int starts_with(const opus_frame_t *clip, int n) {
    return played_n >= n && same(played, clip, n);
}
static int ends_with(const opus_frame_t *clip, int n) {
    return played_n >= n && same(played + played_n - n, clip, n);
}

int main(void) {
    int fails = 0;
    /* "二号房" -> 已进入 + 二 + 号房 */
    announce_room_entry("二号房", "468911");
    if (!starts_with(clip_entered, CLIP_ENTERED_FRAMES)) { puts("E1"); fails++; }
    if (!same(played + CLIP_ENTERED_FRAMES, clip_d2, CLIP_D2_FRAMES)) { puts("E2"); fails++; }
    if (!ends_with(clip_hao_fang, CLIP_HAO_FANG_FRAMES)) { puts("E3"); fails++; }
    int expect = CLIP_ENTERED_FRAMES + CLIP_D2_FRAMES + CLIP_HAO_FANG_FRAMES;
    if (played_n != expect) { printf("E4 n=%d want=%d\n", played_n, expect); fails++; }

    /* "十一号房" -> 已进入 + 十 + 一 + 号房 */
    played_n = 0;
    announce_room_entry("十一号房", "123456");
    if (!same(played + CLIP_ENTERED_FRAMES, clip_d10, CLIP_D10_FRAMES)) { puts("E5"); fails++; }
    if (!same(played + CLIP_ENTERED_FRAMES + CLIP_D10_FRAMES, clip_d1, CLIP_D1_FRAMES)) { puts("E6"); fails++; }
    if (!ends_with(clip_hao_fang, CLIP_HAO_FANG_FRAMES)) { puts("E7"); fails++; }

    /* "客厅" -> 已进入房间 + 邀请码 + digits */
    played_n = 0;
    announce_room_entry("客厅", "468911");
    if (!starts_with(clip_entered, CLIP_ENTERED_FRAMES)) { puts("E8"); fails++; }
    if (!same(played + CLIP_ENTERED_FRAMES, clip_room, CLIP_ROOM_FRAMES)) { puts("E9"); fails++; }
    int base = CLIP_ENTERED_FRAMES + CLIP_ROOM_FRAMES;
    if (!same(played + base, clip_invite, CLIP_INVITE_FRAMES)) { puts("E10"); fails++; }
    const opus_frame_t *want[6] = {clip_d4, clip_d6, clip_d8, clip_d9, clip_d1, clip_d1};
    int want_n[6] = {CLIP_D4_FRAMES, CLIP_D6_FRAMES, CLIP_D8_FRAMES,
                     CLIP_D9_FRAMES, CLIP_D1_FRAMES, CLIP_D1_FRAMES};
    int off = base + CLIP_INVITE_FRAMES;
    for (int i = 0; i < 6; i++) {
        if (!same(played + off, want[i], want_n[i])) { printf("E11 digit %d\n", i); fails++; }
        off += want_n[i];
    }
    if (played_n != off) { printf("E12 n=%d want=%d\n", played_n, off); fails++; }

    /* ascii-digit room name "3号房" parses like 三号房 */
    played_n = 0;
    announce_room_entry("3号房", "286509");
    if (!same(played + CLIP_ENTERED_FRAMES, clip_d3, CLIP_D3_FRAMES)) { puts("E13"); fails++; }
    if (!ends_with(clip_hao_fang, CLIP_HAO_FANG_FRAMES)) { puts("E14"); fails++; }

    /* NULL name -> generic room + invite readout, no crash */
    played_n = 0;
    announce_room_entry(NULL, "286509");
    if (!starts_with(clip_entered, CLIP_ENTERED_FRAMES)) { puts("E15"); fails++; }
    if (!same(played + CLIP_ENTERED_FRAMES, clip_room, CLIP_ROOM_FRAMES)) { puts("E16"); fails++; }

    /* leave + failure cues are single clips */
    played_n = 0;
    announce_leave_done();
    if (played_n != CLIP_LEFT_FRAMES || !same(played, clip_left, CLIP_LEFT_FRAMES)) { puts("E17"); fails++; }
    played_n = 0;
    announce_op_failed();
    if (played_n != CLIP_FAILED_FRAMES || !same(played, clip_failed, CLIP_FAILED_FRAMES)) { puts("E18"); fails++; }

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails;
}
"""


def build_and_run() -> str:
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        body = (MAIN_DIR / "announce.c").read_text()
        # strip the firmware-only includes; harness provides replacements
        body = body.replace('#include "announce.h"', '#include "opus_frame_stub.h"')
        body = body.replace('#include "esp_log.h"', '')
        body = body.replace('#include "audio_dev.h"', '')
        (td / "announce_body.h").write_text(body)
        (td / "opus_frame_stub.h").write_text(
            "#pragma once\n#include <stdint.h>\n"
            "typedef struct { const uint8_t *data; uint16_t len; } opus_frame_t;\n"
            "bool audio_rx_play_frames(const opus_frame_t *f, int n);\n"
            "void announce_room_entry(const char *, const char *);\n"
            "void announce_leave_done(void);\nvoid announce_op_failed(void);\n")
        (td / "announce_clips.h").write_text(
            (MAIN_DIR / "announce_clips.h").read_text())
        (td / "main.c").write_text(HARNESS)
        subprocess.run(["cc", "-O1", "-o", str(td / "t"), str(td / "main.c"),
                        "-I", str(td)], check=True)
        return subprocess.run([str(td / "t")], capture_output=True,
                              text=True).stdout


class AnnounceSeqTest(unittest.TestCase):
    def test_sequences(self):
        out = build_and_run()
        self.assertIn("PASS", out, msg=out)


if __name__ == "__main__":
    unittest.main()
