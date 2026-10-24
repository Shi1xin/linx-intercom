"""Compile-and-run guard for the real dump_snapshot_serial().

Extracts the function from wake_trace.c, feeds known samples, and checks the
emitted indexed little-endian PCMTRACE frames and per-frame yields.
"""

import hashlib
import re
import struct
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
#include <stdio.h>

static int g_yields;
#define pdMS_TO_TICKS(ms) (ms)
#define vTaskDelay(t) do { g_yields++; (void)(t); } while (0)

/* device-under-test state (mirrors wake_trace.c storage) */
static int16_t s_snapshot[128];
static size_t s_snapshot_samples;
"""

FIXTURE_MAIN = r"""
int main(int argc, char **argv)
{
    if (argc < 3) { return 9; }
    if (argv[1][0] == 's') {
        static const int16_t vals[4] = { -32768, -1, 0, 32767 };
        for (int i = 0; i < 4; i++) s_snapshot[i] = vals[i];
        s_snapshot_samples = 4;
    } else {
        for (int i = -32; i <= 32; i++) s_snapshot[i + 32] = (int16_t)i;
        s_snapshot_samples = 65;
    }
    dump_snapshot_serial(argv[2]);
    printf("YIELDS %d\n", g_yields);
    return 0;
}
"""

SMALL = [-32768, -1, 0, 32767]
BIG = list(range(-32, 33))


class TestDumpSnapshotSerial(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = (MAIN_DIR / "wake_trace.c").read_text()
        dut = _extract_function(
            src, "static void dump_snapshot_serial(const char *hash)"
        )
        cls._tmp = tempfile.TemporaryDirectory()
        c_path = Path(cls._tmp.name) / "dump.c"
        cls._bin = Path(cls._tmp.name) / "dump"
        c_path.write_text(FIXTURE_PREAMBLE + dut + FIXTURE_MAIN)
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Werror",
             "-o", str(cls._bin), str(c_path)],
            check=True, capture_output=True, text=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def _run(self, mode, pcm):
        sha = hashlib.sha256(struct.pack("<%dh" % len(pcm), *pcm)).hexdigest()
        res = subprocess.run(
            [str(self._bin), mode, sha], check=True, capture_output=True, text=True
        )
        return res.stdout.splitlines(), sha

    def test_four_sample_frame(self):
        lines, sha = self._run("small", SMALL)
        self.assertEqual(
            lines[0],
            f"PCMTRACE BEGIN id=1 samples=4 sha256={sha}",
        )
        self.assertEqual(lines[1], "PCMTRACE DATA id=1 offset=0 hex=0080ffff0000ff7f")
        self.assertEqual(lines[2], "PCMTRACE END id=1")
        self.assertEqual(lines[3], "YIELDS 1")

    def test_65_sample_two_frames(self):
        lines, sha = self._run("big", BIG)
        self.assertEqual(
            lines[0],
            f"PCMTRACE BEGIN id=1 samples=65 sha256={sha}",
        )
        m0 = re.fullmatch(r"PCMTRACE DATA id=1 offset=0 hex=([0-9a-f]+)", lines[1])
        m1 = re.fullmatch(r"PCMTRACE DATA id=1 offset=64 hex=([0-9a-f]+)", lines[2])
        self.assertIsNotNone(m0)
        self.assertIsNotNone(m1)
        self.assertEqual(len(m0[1]), 64 * 4)
        self.assertEqual(m1[1], "2000")  # sample 32 -> 0x0020 little-endian
        body = bytes.fromhex(m0[1]) + bytes.fromhex(m1[1])
        self.assertEqual(body, struct.pack("<65h", *BIG))
        self.assertEqual(lines[3], "PCMTRACE END id=1")
        self.assertEqual(lines[4], "YIELDS 2")


class TestSerialFallbackSourceGuards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = (MAIN_DIR / "wake_trace.c").read_text()

    def test_dump_only_on_http_failure_before_unbusy(self):
        body = _extract_function(self.src, "static void upload_task(void *arg)")
        dump = body.find("dump_snapshot_serial(hash)")
        guard = body.rfind("err != ESP_OK || status != 200", 0, dump)
        unbusy = body.find("s_busy = false", dump)
        self.assertTrue(0 < guard < dump < unbusy,
                        "serial dump must run only on HTTP failure, before unbusy")


if __name__ == "__main__":
    unittest.main()
