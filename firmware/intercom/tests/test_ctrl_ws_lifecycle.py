"""Compile-and-run guard for the real ctrl ws_event_handler().

Extracts the handler from ctrl_client.c and drives it with stub event ids,
verifying that the clean-CLOSED terminal event takes the same
clear+DISCONNECTED post path as an abortive disconnect.
"""

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

#define ESP_LOGI(tag, ...) do { (void)0; } while (0)
#define ESP_LOGW(tag, ...) do { (void)0; } while (0)
#define TAG "ctrl"
#define RX_BUF_SIZE 16384

typedef const char *esp_event_base_t;
typedef int app_ev_type_t;
enum { APP_EV_WS_CONNECTED = 1, APP_EV_WS_DISCONNECTED = 2 };
enum {
    WEBSOCKET_EVENT_CONNECTED = 1,
    WEBSOCKET_EVENT_DISCONNECTED = 2,
    WEBSOCKET_EVENT_DATA = 3,
    WEBSOCKET_EVENT_ERROR = 4,
    WEBSOCKET_EVENT_FINISH = 5,
    WEBSOCKET_EVENT_CLOSED = 6,
};

typedef struct {
    int op_code;
    int payload_offset;
    int data_len;
    const char *data_ptr;
    int payload_len;
} esp_websocket_event_data_t;

/* observable stubs */
static volatile bool s_connected;
static char s_rx_buf_data[RX_BUF_SIZE];
static char *s_rx_buf = s_rx_buf_data;
static size_t s_rx_len;
static int g_post_count;
static int g_post_last;
static int g_text_calls;

static bool app_post(app_ev_type_t type, char *msg, char *msg2, int arg0, bool flag)
{
    (void)msg; (void)msg2; (void)arg0; (void)flag;
    g_post_count++;
    g_post_last = (int)type;
    return true;
}
static void handle_text(const char *data, int len)
{
    (void)data; (void)len;
    g_text_calls++;
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
    s_connected = false; s_rx_len = 0;
    g_post_count = 0; g_post_last = 0; g_text_calls = 0;
}
static void fire(int id, void *data)
{
    ws_event_handler(NULL, "ws", id, data);
}

int main(void)
{
    /* CONNECTED: flag set, rx reset, one CONNECTED post */
    reset(); s_rx_len = 9;
    fire(WEBSOCKET_EVENT_CONNECTED, NULL);
    check("conn flag", s_connected == true);
    check("conn rx", s_rx_len == 0);
    check("conn post", g_post_count == 1 && g_post_last == APP_EV_WS_CONNECTED);

    /* CLOSED from connected+rx: clears both, one DISCONNECTED post, NULL data ok */
    reset(); s_connected = true; s_rx_len = 512;
    fire(WEBSOCKET_EVENT_CLOSED, NULL);
    check("closed flag", s_connected == false);
    check("closed rx", s_rx_len == 0);
    check("closed post", g_post_count == 1 && g_post_last == APP_EV_WS_DISCONNECTED);

    /* DISCONNECTED behaves identically */
    reset(); s_connected = true; s_rx_len = 7;
    fire(WEBSOCKET_EVENT_DISCONNECTED, NULL);
    check("disc flag", s_connected == false);
    check("disc rx", s_rx_len == 0);
    check("disc post", g_post_count == 1 && g_post_last == APP_EV_WS_DISCONNECTED);

    /* initial failed connect: flag already false still posts DISCONNECTED */
    reset();
    fire(WEBSOCKET_EVENT_DISCONNECTED, NULL);
    check("failed conn post", g_post_count == 1 &&
          g_post_last == APP_EV_WS_DISCONNECTED);

    /* FINISH after CLOSED: no additional post */
    reset(); s_connected = true;
    fire(WEBSOCKET_EVENT_CLOSED, NULL);
    fire(WEBSOCKET_EVENT_FINISH, NULL);
    check("finish no post", g_post_count == 1);

    /* ERROR: no edge post (old behavior retained) */
    reset(); s_connected = true;
    fire(WEBSOCKET_EVENT_ERROR, NULL);
    check("error no post", g_post_count == 0);
    check("error keeps flag", s_connected == true);

    /* DATA text frame still reaches handle_text */
    reset(); s_connected = true;
    esp_websocket_event_data_t e = {
        .op_code = 0x1, .payload_offset = 0,
        .data_ptr = "{}", .data_len = 2, .payload_len = 2,
    };
    fire(WEBSOCKET_EVENT_DATA, &e);
    check("data to text", g_text_calls == 1);

    if (g_fails == 0) printf("ALL PASS\n");
    return g_fails;
}
"""


class TestCtrlWsLifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = (MAIN_DIR / "ctrl_client.c").read_text()
        dut = _extract_function(
            src,
            "static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)",
        )
        with tempfile.TemporaryDirectory() as d:
            c_path = Path(d) / "ws.c"
            bin_path = Path(d) / "ws"
            c_path.write_text(FIXTURE_PREAMBLE + dut + FIXTURE_MAIN)
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Werror",
                 "-o", str(bin_path), str(c_path)],
                check=True, capture_output=True, text=True,
            )
            res = subprocess.run([str(bin_path)], capture_output=True, text=True)
            cls.stdout = res.stdout
            cls.returncode = res.returncode

    def test_lifecycle_cases(self):
        self.assertEqual(self.stdout.count("FAIL"), 0, self.stdout)
        self.assertIn("ALL PASS", self.stdout)
        self.assertEqual(self.returncode, 0)


class TestCtrlWsSourceGuards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = (MAIN_DIR / "ctrl_client.c").read_text()
        cls.body = _extract_function(
            src,
            "static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)",
        )

    def test_closed_shares_disconnected_path(self):
        closed = self.body.find("case WEBSOCKET_EVENT_CLOSED:")
        disc = self.body.find("case WEBSOCKET_EVENT_DISCONNECTED:")
        clear = self.body.find("s_connected = false;", disc)
        self.assertTrue(0 < closed < disc < clear,
                        "CLOSED must fall through the DISCONNECTED path")
        between = self.body[closed:disc]
        self.assertNotIn("break", between, "CLOSED must share the disconnect path")

    def test_log_records_clean_flag(self):
        self.assertIn('"ws disconnected (clean=%d)"', self.body)
        self.assertIn("id == WEBSOCKET_EVENT_CLOSED", self.body)


if __name__ == "__main__":
    unittest.main()
