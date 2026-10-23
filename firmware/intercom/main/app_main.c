/* 灵矽同频对讲固件 MVP — app_main
 *
 * Single app task serializes all events (app_events.h). State machine:
 * idle -> joining -> talking -> command/switching -> leaving -> idle.
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_peer.h"   /* esp_peer_state_t values for PEER_STATE event */

#include "app_state.h"
#include "app_events.h"
#include "board.h"
#include "nvs_store.h"
#include "wifi_sta.h"
#include "ctrl_client.h"
#include "rtc_peer.h"
#include "audio_dev.h"
#include "audio_codec.h"
#include "audio_proc.h"
#include "cmd_capture.h"
#include "cmd_input.h"

#define TAG "app"

#define MIC_FRAME_MS      20
#define MIC_FRAME_SAMPLES (AUDIO_INPUT_SAMPLE_RATE * MIC_FRAME_MS / 1000)  /* 320 */
#define RTP_PTS_STEP      (AUDIO_WIRE_SAMPLE_RATE * MIC_FRAME_MS / 1000)   /* 960 */

QueueHandle_t g_app_queue;

static app_state_t s_state = APP_STATE_IDLE;
static app_state_t s_state_before_cmd = APP_STATE_TALKING;
static app_state_t s_state_before_join = APP_STATE_IDLE;
static volatile bool s_muted;
static volatile bool s_command_active;
static esp_timer_handle_t s_reconnect_timer;

const char *app_state_name(app_state_t s)
{
    switch (s) {
    case APP_STATE_IDLE:      return "idle";
    case APP_STATE_JOINING:   return "joining";
    case APP_STATE_TALKING:   return "talking";
    case APP_STATE_COMMAND:   return "command";
    case APP_STATE_SWITCHING: return "switching";
    case APP_STATE_LEAVING:   return "leaving";
    default:                  return "?";
    }
}

bool app_post(app_ev_type_t type, char *msg, char *msg2, int arg0, bool flag)
{
    if (g_app_queue == NULL) {
        free(msg);
        free(msg2);
        return false;
    }
    app_event_t ev = { .type = type, .msg = msg, .msg2 = msg2, .arg0 = arg0, .flag = flag };
    if (xQueueSend(g_app_queue, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(msg);
        free(msg2);
        return false;
    }
    return true;
}

bool app_post_str(app_ev_type_t type, const char *msg, const char *msg2, int arg0, bool flag)
{
    return app_post(type, msg ? strdup(msg) : NULL, msg2 ? strdup(msg2) : NULL, arg0, flag);
}

static void set_state(app_state_t s)
{
    if (s != s_state) {
        ESP_LOGI(TAG, "state %s -> %s", app_state_name(s_state), app_state_name(s));
        s_state = s;
    }
    switch (s) {
    case APP_STATE_TALKING:   led_set(LED_ON);         break;
    case APP_STATE_COMMAND:   led_set(LED_BLINK_FAST); break;
    case APP_STATE_JOINING:
    case APP_STATE_SWITCHING: led_set(LED_BLINK_SLOW); break;
    default:                  led_set(LED_OFF);        break;
    }
}

/* ---------- helpers ---------- */

static void apply_mute(bool muted)
{
    s_muted = muted;
    nvs_store_set_muted(muted);
    ESP_LOGI(TAG, "muted=%d", muted);
}

static void start_join(const char *kind, const char *arg)
{
    if (arg == NULL || arg[0] == 0 || kind == NULL) {
        return;
    }
    s_state_before_join = s_state;
    if (s_state == APP_STATE_TALKING || s_state == APP_STATE_COMMAND) {
        set_state(APP_STATE_SWITCHING);
    } else {
        set_state(APP_STATE_JOINING);
    }
    ctrl_send_join(kind, arg);
}

static void schedule_ws_reconnect(int delay_ms)
{
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (int64_t)delay_ms * 1000);
    }
}

static void teardown_media(void)
{
    s_command_active = false;
    cmd_capture_abort();
    rtc_peer_stop();
    audio_rx_flush();
    audio_decoder_reset();
}

/* ---------- uplink capture task ---------- */

static void capture_task(void *arg)
{
    int16_t *pcm = heap_caps_malloc(MIC_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
    uint8_t *pkt = heap_caps_malloc(512, MALLOC_CAP_8BIT);
    uint32_t pts = 0;
    ESP_LOGI(TAG, "capture task up");
    int dbg = 0;
    for (;;) {
        int n = audio_dev_mic_read(pcm, MIC_FRAME_SAMPLES);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(MIC_FRAME_MS));
            continue;
        }
        if (++dbg % 200 == 0) {
            int32_t peak = 0;
            for (int i = 0; i < n; i++) {
                int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v > peak) peak = v;
            }
            ESP_LOGI(TAG, "mic peak=%d", (int)peak);
        }
        if (s_command_active) {
            cmd_capture_feed(pcm, n);
            continue;
        }
        if (!rtc_peer_is_connected() || s_muted) {
            continue;
        }
        audio_proc_mic(pcm, n);
        int enc = audio_opus_encode(pcm, n, pkt, 512);
        if (dbg % 200 == 0) {
            ESP_LOGI(TAG, "enc %dB first=%02x%02x", enc, pkt[0], enc > 1 ? pkt[1] : 0);
        }
        if (enc > 0) {
            rtc_peer_send_audio(pkt, enc, pts);
            pts += RTP_PTS_STEP;
        }
    }
}

/* ---------- event handlers ---------- */

static void on_wifi_connected(void)
{
    if (!ctrl_client_is_connected()) {
        ctrl_client_connect();
    }
}

static void on_ws_connected(void)
{
    ctrl_send_hello(wifi_sta_device_id());
}

static void on_ws_disconnected(void)
{
    teardown_media();
    set_state(APP_STATE_IDLE);
    if (wifi_sta_is_connected()) {
        schedule_ws_reconnect(3000);
    }
}

static void on_hello_result(app_event_t *ev)
{
    if (!ev->flag) {
        ESP_LOGW(TAG, "hello failed");
        return;
    }
    apply_mute(ev->arg0 != 0);
    if (ev->msg && ev->msg[0]) {
        ESP_LOGI(TAG, "resume room: %s", ev->msg);
        start_join("room_name", ev->msg);
    }
}

static void on_join_result(app_event_t *ev)
{
    if (!ev->flag) {
        ESP_LOGW(TAG, "join failed: %s", ev->msg ? ev->msg : "?");
        set_state(s_state_before_join);
        return;
    }
    ESP_LOGI(TAG, "joined room %s need_media=%d", ev->msg ? ev->msg : "?", ev->arg0);
    if (ev->arg0) {
        /* need media: bring peer up and produce an offer */
        rtc_peer_start();
        rtc_peer_create_offer();
    } else {
        /* media reused: flush downlink and confirm */
        audio_rx_flush();
        audio_decoder_reset();
        ctrl_send_confirm();
    }
}

static void on_peer_state(app_event_t *ev)
{
    int st = ev->arg0;
    if (st == ESP_PEER_STATE_CONNECTED) {
        ESP_LOGI(TAG, "media connected");
        ctrl_send_ice_completed();
        if (s_state == APP_STATE_JOINING || s_state == APP_STATE_SWITCHING) {
            ctrl_send_confirm();
        }
    } else if (st == ESP_PEER_STATE_CONNECT_FAILED) {
        ESP_LOGW(TAG, "media connect failed");
        if (s_state == APP_STATE_JOINING || s_state == APP_STATE_SWITCHING) {
            set_state(s_state_before_join);
        }
    } else if (st == ESP_PEER_STATE_DISCONNECTED) {
        ESP_LOGW(TAG, "media disconnected");
    }
}

static void on_confirm_result(app_event_t *ev)
{
    if (ev->flag) {
        set_state(APP_STATE_TALKING);
        ESP_LOGI(TAG, "in room %s", ev->msg ? ev->msg : "");
    } else {
        ESP_LOGW(TAG, "confirm failed");
        set_state(APP_STATE_IDLE);
    }
}

static void on_cmd_release(void)
{
    if (!s_command_active) {
        return;
    }
    s_command_active = false;
    int samples = cmd_capture_finish();
    set_state(s_state_before_cmd);
    if (samples <= 0) {
        ESP_LOGI(TAG, "command too short, ignored");
        return;
    }
    cmd_result_t res;
    if (cmd_recognize(cmd_capture_data(), samples, &res)) {
        start_join(res.kind, res.arg);
    }
}

static void on_notify(app_event_t *ev)
{
    const char *event = ev->msg;
    const char *room = ev->msg2;
    if (event == NULL) {
        return;
    }
    ESP_LOGI(TAG, "notify %s room=%s", event, room ? room : "-");
    if (strcmp(event, "summon") == 0 && room && room[0]) {
        /* summoned: join via invite code (device has no membership yet) */
        start_join(ev->arg0 ? "room_name" : "invite", room);
    }
}

static void app_task(void *arg)
{
    app_event_t ev;
    for (;;) {
        if (xQueueReceive(g_app_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (ev.type) {
        case APP_EV_WIFI_CONNECTED:    on_wifi_connected(); break;
        case APP_EV_WIFI_DISCONNECTED: ESP_LOGW(TAG, "wifi down"); break;
        case APP_EV_WS_CONNECTED:      on_ws_connected(); break;
        case APP_EV_WS_DISCONNECTED:   on_ws_disconnected(); break;
        case APP_EV_HELLO_RESULT:      on_hello_result(&ev); break;
        case APP_EV_JOIN_RESULT:       on_join_result(&ev); break;
        case APP_EV_SDP:
            if (ev.msg) rtc_peer_set_remote_sdp(ev.msg);
            break;
        case APP_EV_ICE:
            if (ev.flag) {
                ESP_LOGI(TAG, "remote ice completed");
            } else if (ev.msg) {
                rtc_peer_add_remote_candidate(ev.msg);
            }
            break;
        case APP_EV_CONFIRM_RESULT:    on_confirm_result(&ev); break;
        case APP_EV_MUTE_RESULT:
            if (ev.flag) apply_mute(ev.arg0 != 0);
            break;
        case APP_EV_LEAVE_RESULT:
            teardown_media();
            set_state(APP_STATE_IDLE);
            break;
        case APP_EV_NOTIFY:            on_notify(&ev); break;
        case APP_EV_ERROR_MSG:
            ESP_LOGW(TAG, "server error %s: %s", ev.msg ? ev.msg : "?", ev.msg2 ? ev.msg2 : "");
            break;
        case APP_EV_PEER_SDP:
            if (ev.msg) ctrl_send_sdp(ev.msg);
            break;
        case APP_EV_PEER_ICE:
            if (ev.msg) ctrl_send_ice_candidate(ev.msg);
            break;
        case APP_EV_PEER_STATE:        on_peer_state(&ev); break;
        case APP_EV_CMD_PRESS:
            if (s_state == APP_STATE_TALKING) {
                s_state_before_cmd = s_state;
                set_state(APP_STATE_COMMAND);
                s_command_active = true;
                cmd_capture_begin();
            }
            break;
        case APP_EV_CMD_RELEASE:       on_cmd_release(); break;
        case APP_EV_TOUCH_TAP:
            if (ctrl_client_is_connected()) {
                ctrl_send_set_mute(!s_muted);
            }
            break;
        case APP_EV_VOL_UP: {
            int v = audio_dev_get_volume() + 10;
            audio_dev_set_volume(v);
            nvs_store_set_volume(audio_dev_get_volume());
            break;
        }
        case APP_EV_VOL_DOWN: {
            int v = audio_dev_get_volume() - 10;
            audio_dev_set_volume(v);
            nvs_store_set_volume(audio_dev_get_volume());
            break;
        }
        case APP_EV_ROOM_INFO_RESULT:
            ESP_LOGI(TAG, "room %s online %d", ev.msg ? ev.msg : "?", ev.arg0);
            break;
        case APP_EV_RECONNECT_TIMER:
            if (!ctrl_client_is_connected() && wifi_sta_is_connected()) {
                ctrl_client_connect();
            }
            break;
        case APP_EV_CMD_RESULT:
            break;
        default:
            break;
        }
        free(ev.msg);
        free(ev.msg2);
    }
}

static void reconnect_timer_cb(void *arg)
{
    app_post(APP_EV_RECONNECT_TIMER, NULL, NULL, 0, false);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_ERROR_CHECK(nvs_store_init());
    g_app_queue = xQueueCreate(24, sizeof(app_event_t));
    configASSERT(g_app_queue);

    ESP_ERROR_CHECK(ctrl_client_init());
    ESP_ERROR_CHECK(cmd_capture_init());
    ESP_ERROR_CHECK(audio_dev_init());
    ESP_ERROR_CHECK(audio_codec_init());
    audio_proc_init();
    ESP_ERROR_CHECK(cmd_input_init());
    rtc_peer_init();          /* DTLS cert pre-gen (slow, once) */
    audio_dev_set_volume(nvs_store_get_volume());
    s_muted = nvs_store_get_muted();

    const esp_timer_create_args_t tmr = {
        .callback = reconnect_timer_cb,
        .name = "ws_reconnect",
    };
    esp_timer_create(&tmr, &s_reconnect_timer);

    xTaskCreatePinnedToCore(capture_task, "aud_cap", 40 * 1024, NULL, 9, NULL, 0);
    xTaskCreatePinnedToCore(app_task, "app", 8 * 1024, NULL, 6, NULL, 0);

    ESP_ERROR_CHECK(wifi_sta_start());
}
