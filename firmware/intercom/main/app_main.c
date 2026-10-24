/* 灵矽同频对讲固件 — app_main
 *
 * Single app task serializes all events (app_events.h). State machine:
 * idle(灵矽模式) -> joining -> talking(对讲) -> command/switching -> leaving.
 *
 * Linx channel: OTA -> ws hello(mcp) -> tools discovered -> tools/call
 * dispatched through the same room ops used by NFC/summon, replies only after
 * the media confirm. Command key is hold-to-talk toward Linx in both modes.
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_peer.h"   /* esp_peer_state_t values for PEER_STATE event */
#include "cJSON.h"

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
#include "linx_ota.h"
#include "linx_client.h"
#include "wake_word.h"

#define TAG "app"

#define MIC_FRAME_MS      20
#define MIC_FRAME_SAMPLES (AUDIO_INPUT_SAMPLE_RATE * MIC_FRAME_MS / 1000)  /* 320 */
#define RTP_PTS_STEP      (AUDIO_WIRE_SAMPLE_RATE * MIC_FRAME_MS / 1000)   /* 960 */

#define OP_TIMEOUT_MS       20000
#define TTS_WINDOW_MS       25000   /* post-command window where linx TTS may play */
#define LINXS_BOOT_RETRY_MS 15000

QueueHandle_t g_app_queue;

static app_state_t s_state = APP_STATE_IDLE;
static app_state_t s_state_before_cmd = APP_STATE_TALKING;
static app_state_t s_state_before_join = APP_STATE_IDLE;
static volatile bool s_muted;
static volatile bool s_command_active;      /* command key held: mic -> linx */
static volatile bool s_wake_listen;         /* wake word opened a linx listen round */
static volatile bool s_linx_playing;        /* platform TTS is on the speaker */
static TickType_t      s_wake_tick;         /* wake listen round opened at */
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_linx_reconnect_timer;
static esp_timer_handle_t s_op_timer;

/* room snapshot for status replies */
static char s_room_name[32];
static char s_room_code[12];
static int  s_online_count;

/* linx bootstrap state */
static bool s_linx_started;
static char s_linx_client_id[48];

/* auto-rejoin after media_lost: bounded retries so an empty room does not
 * thrash ICE every Janus keepalive cycle */
static int  s_rejoin_count;
static int64_t s_last_rejoin_us;
#define MAX_MEDIA_REJOIN 5
#define REJOIN_DECAY_US  (600 * 1000 * 1000)  /* 10min clean -> budget recovers */
static int16_t s_pcm_silence[MIC_FRAME_SAMPLES]; /* zeros: keepalive payload */

/* audible cue so cloud/NFC takeovers give room-entry feedback even when no
 * platform TTS reply arrives. sine burst encoded through the normal opus
 * path; transient interleave with room audio is harmless (same decoder). */
static void play_tone(int freq_hz, int ms)
{
    /* synthesized in the playback task: no opus encoder/decoder contention */
    audio_rx_play_tone(freq_hz, ms);
    ESP_LOGI(TAG, "tone %dHz %dms", freq_hz, ms);
}

/* pending MCP call: reply is sent when the underlying room op resolves */
static struct {
    bool active;
    bool from_ctrl;   /* reply goes over the control channel (op_result) */
    char id[64];      /* raw json id (linx) or bare op_id (ctrl) */
    char name[40];    /* tool name */
} s_mcp_op;

/* linx TTS playback window: platform audio may enter the speaker queue only
 * while in linx mode, or while a command/tool-result round is still open
 * (filter for late TTS after takeover). us deadline on esp_timer clock. */
static int64_t s_tts_window_until;

/* command key: press tick + whether a linx listen round was opened.
 * listen start is deferred ~250 ms so quick taps / double-tap mute never
 * reach the platform. */
static TickType_t s_cmd_press_tick;
static TickType_t s_last_tap_tick;
static volatile bool s_linx_listen_sent;
#define CMD_LISTEN_DELAY_MS 250

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

static bool in_intercom(void)
{
    return s_state == APP_STATE_TALKING || s_state == APP_STATE_COMMAND ||
           s_state == APP_STATE_SWITCHING || s_state == APP_STATE_LEAVING;
}

static void update_linx_gate(void)
{
    bool allow = !in_intercom() || esp_timer_get_time() < s_tts_window_until;
    linx_set_playback_gate(allow);
    audio_rx_set_source(allow ? AUDIO_SRC_LINX : AUDIO_SRC_ROOM);
}

static void open_tts_window(void)
{
    s_tts_window_until = esp_timer_get_time() + (int64_t)TTS_WINDOW_MS * 1000;
    update_linx_gate();
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
    update_linx_gate();
}

/* ---------- helpers ---------- */

static void apply_mute(bool muted)
{
    s_muted = muted;
    nvs_store_set_muted(muted);
    ESP_LOGI(TAG, "muted=%d", muted);
}

static void start_join_ex(const char *kind, const char *arg, bool auto_rejoin)
{
    if (!auto_rejoin) {
        s_rejoin_count = 0;
    } else {
        int64_t now = esp_timer_get_time();
        if (now - s_last_rejoin_us > REJOIN_DECAY_US) {
            s_rejoin_count = 0;   /* long stable run: decay the budget */
        }
        s_last_rejoin_us = now;
    }
    if (arg == NULL || arg[0] == 0 || kind == NULL) {
        return;
    }
    s_state_before_join = s_state;
    if (s_state == APP_STATE_TALKING || s_state == APP_STATE_COMMAND) {
        set_state(APP_STATE_SWITCHING);
    } else {
        set_state(APP_STATE_JOINING);
    }
    if (s_op_timer) {
        esp_timer_stop(s_op_timer);
        esp_timer_start_once(s_op_timer, (int64_t)OP_TIMEOUT_MS * 1000);
    }
    ctrl_send_join(kind, arg);
}

static void start_join(const char *kind, const char *arg)
{
    start_join_ex(kind, arg, false);
}

static void schedule_ws_reconnect(int delay_ms)
{
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (int64_t)delay_ms * 1000);
    }
}

static void schedule_linx_reconnect(int delay_ms)
{
    if (s_linx_reconnect_timer) {
        esp_timer_stop(s_linx_reconnect_timer);
        esp_timer_start_once(s_linx_reconnect_timer, (int64_t)delay_ms * 1000);
    }
}

static void teardown_media(void)
{
    s_command_active = false;
    cmd_capture_abort();
    rtc_peer_stop();
    audio_rx_flush();
    audio_rx_mark_decoder_reset();  /* playback task does it (decode-safety) */
}

/* ---------- MCP op lifecycle ---------- */

static void mcp_op_arm_timer(void)
{
    if (s_op_timer) {
        esp_timer_stop(s_op_timer);
        esp_timer_start_once(s_op_timer, (int64_t)OP_TIMEOUT_MS * 1000);
    }
}

static void mcp_op_cancel_timer(void)
{
    if (s_op_timer) {
        esp_timer_stop(s_op_timer);
    }
}

static void mcp_op_finish(bool ok, const char *result_json)
{
    if (!s_mcp_op.active) {
        return;
    }
    ESP_LOGI(TAG, "mcp %s -> %s", s_mcp_op.name, ok ? "ok" : "fail");
    if (s_mcp_op.from_ctrl) {
        ctrl_send_op_result(s_mcp_op.id, ok, result_json);
    } else {
        linx_mcp_reply_result(s_mcp_op.id, result_json, !ok);
    }
    s_mcp_op.active = false;
    mcp_op_cancel_timer();
    if (!s_mcp_op.from_ctrl) {
        open_tts_window();  /* the agent's ack reply may still legitimately play */
    }
}

static void mcp_op_finish_ok_room(void)
{
    char js[256];
    snprintf(js, sizeof(js),
             "{\"ok\":true,\"mode\":\"%s\",\"room\":\"%s\",\"invite_code\":\"%s\","
             "\"online_count\":%d,\"muted\":%s}",
             in_intercom() ? "intercom" : "linx", s_room_name, s_room_code,
             s_online_count, s_muted ? "true" : "false");
    mcp_op_finish(true, js);
}

static void mcp_op_finish_err(const char *code, const char *msg)
{
    char js[256];
    snprintf(js, sizeof(js), "{\"ok\":false,\"error\":\"%s\",\"message\":\"%s\"}",
             code, msg ? msg : "");
    mcp_op_finish(false, js);
}

static bool mcp_op_is(const char *name)
{
    return s_mcp_op.active && strcmp(s_mcp_op.name, name) == 0;
}

/* ---------- linx bootstrap ---------- */

static void linx_bootstrap(void)
{
    const char *dev = strlen(CONFIG_INTERCOM_LINX_DEVICE_ID) ?
                      CONFIG_INTERCOM_LINX_DEVICE_ID : wifi_sta_device_id();
    const char *cid = strlen(CONFIG_INTERCOM_LINX_CLIENT_ID) ?
                      CONFIG_INTERCOM_LINX_CLIENT_ID : s_linx_client_id;
    linx_ota_result_t ota;
    esp_err_t err = linx_ota_fetch(CONFIG_INTERCOM_LINX_OTA_URL, dev, cid,
                                   CONFIG_INTERCOM_LINX_BOARD_TYPE, &ota);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota fetch failed, retry in %ds", LINXS_BOOT_RETRY_MS / 1000);
        schedule_linx_reconnect(LINXS_BOOT_RETRY_MS);
        return;
    }
    if (!ota.bound) {
        ESP_LOGW(TAG, "device unbound, activation code=%s (bind then retry)",
                 ota.activation_code);
        schedule_linx_reconnect(LINXS_BOOT_RETRY_MS);
        return;
    }
    ESP_LOGI(TAG, "ota ok ws=%s", ota.ws_url);
    linx_client_connect(ota.ws_url, ota.token, dev, cid);
}

/* ---------- uplink capture task ---------- */

static void capture_task(void *arg)
{
    int16_t *pcm = heap_caps_malloc(MIC_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
    uint8_t *pkt = heap_caps_malloc(512, MALLOC_CAP_8BIT);
    uint32_t pts = 0;
    bool was_connected = false;
    ESP_LOGI(TAG, "capture task up");
    int dbg = 0;
    uint32_t mic_samples = 0;
    int32_t mic_peak = 0;
    int64_t mic_log_start = esp_timer_get_time();
    for (;;) {
        int n = audio_dev_mic_read(pcm, MIC_FRAME_SAMPLES);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(MIC_FRAME_MS));
            continue;
        }
        mic_samples += n;
        for (int i = 0; i < n; i++) {
            int32_t v = pcm[i] < 0 ? -(int32_t)pcm[i] : (int32_t)pcm[i];
            if (v > mic_peak) mic_peak = v;
        }
        if (++dbg % 200 == 0) {
            int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "mic peak=%d samples=%lu elapsed_us=%lld",
                     (int)mic_peak, (unsigned long)mic_samples,
                     (long long)(now - mic_log_start));
            mic_samples = 0;
            mic_peak = 0;
            mic_log_start = now;
        }
        audio_proc_mic(pcm, n);
        vTaskDelay(1);   /* 1 ms block: when encode keeps the loop hot the idle
                          * task still gets serviced (task_wdt on core 0) */
        /* wake detection runs whenever no listen round owns the mic; paused
         * while platform TTS is playing so the speaker cannot re-trigger it.
         * stays armed during linx outages so a flap does not kill the word */
        if (!s_command_active && !s_wake_listen && !s_linx_playing) {
            wake_word_feed(pcm, n);
        }
        if (s_wake_listen) {
            /* wake-triggered auto round: stream to linx until the platform
             * answers (tts) or the safety timeout fires */
            if (!linx_client_is_connected() ||
                xTaskGetTickCount() - s_wake_tick > pdMS_TO_TICKS(15000)) {
                s_wake_listen = false;
                wake_word_enable(true);
                linx_send_listen("stop", "auto");
            } else {
                int enc = audio_opus_encode(pcm, n, pkt, 512);
                if (enc > 0) {
                    linx_send_audio(pkt, enc);
                }
            }
            continue;
        }
        if (s_command_active) {
            /* hold-to-talk: command audio goes to Linx in either mode; the
             * room uplink is paused while the key is held. The listen round
             * opens only after CMD_LISTEN_DELAY_MS so taps stay local. */
            cmd_capture_feed(pcm, n);   /* keeps PCM stats for diagnostics */
            if (!s_linx_listen_sent && linx_client_is_connected() &&
                xTaskGetTickCount() - s_cmd_press_tick > pdMS_TO_TICKS(CMD_LISTEN_DELAY_MS)) {
                s_linx_listen_sent = true;
                linx_send_listen("start", "manual");
                if (!s_command_active) {   /* released mid-arm: close orphan listen */
                    s_linx_listen_sent = false;
                    linx_send_listen("stop", "manual");
                }
            }
            if (s_linx_listen_sent) {
                int enc = audio_opus_encode(pcm, n, pkt, 512);
                if (enc > 0) {
                    linx_send_audio(pkt, enc);
                }
            }
            continue;
        }
        if (s_state != APP_STATE_TALKING || !rtc_peer_is_connected()) {
            was_connected = false;
            continue;   /* uplink only after confirm lands us in TALKING */
        }
        if (s_muted) {
            /* send a silence frame periodically so Janus keepalive never
             * hangs up a muted-only leg (root cause of the rejoin churn) */
            int enc = audio_opus_encode(s_pcm_silence, MIC_FRAME_SAMPLES, pkt, 512);
            if (enc > 0 && (dbg % 25 == 0)) {   /* ~500ms cadence */
                rtc_peer_send_audio(pkt, enc, pts);
            }
            pts += RTP_PTS_STEP;   /* keep the wire clock at 48k even while muted */
            continue;
        }
        if (!was_connected) {
            was_connected = true;
            pts = 0; /* fresh peer: restart RTP timestamps */
        }
        int enc = audio_opus_encode(pcm, n, pkt, 512);
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
#if CONFIG_INTERCOM_LINX_ENABLE
    if (!s_linx_started) {
        s_linx_started = true;
        linx_bootstrap();
    }
#endif
}

static void on_ws_connected(void)
{
    ctrl_send_hello(wifi_sta_device_id());
}

static void on_ws_disconnected(void)
{
    /* control channel lost: any pending op cannot resolve; answer linx ops,
     * drop ctrl ops (their channel is the one that died) */
    if (s_mcp_op.active) {
        if (!s_mcp_op.from_ctrl) {
            mcp_op_finish_err("control_offline", "control channel lost");
        } else {
            s_mcp_op.active = false;
            mcp_op_cancel_timer();
        }
    }
    /* losing the control channel ends the room; the linx channel is
     * independent and keeps running */
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
        play_tone(220, 250);   /* failure cue */
        if (s_mcp_op.active &&
            (mcp_op_is("self.intercom.join") || mcp_op_is("self.intercom.switch"))) {
            mcp_op_finish_err("join_failed", ev->msg);
        }
        set_state(s_state_before_join);
        return;
    }
    ESP_LOGI(TAG, "joined room %s need_media=%d", ev->msg ? ev->msg : "?", ev->arg0);
    if (ev->arg0) {
        /* need media: rebuild a possibly-dead peer, then offer */
        rtc_peer_stop();
        rtc_peer_start();
        rtc_peer_create_offer();
    } else {
        /* media reused: flush downlink and confirm */
        audio_rx_flush();
        audio_rx_mark_decoder_reset();
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
        if (s_mcp_op.active &&
            (mcp_op_is("self.intercom.join") || mcp_op_is("self.intercom.switch"))) {
            mcp_op_finish_err("media_failed", "webrtc connect failed");
        }
        if (s_state == APP_STATE_JOINING || s_state == APP_STATE_SWITCHING) {
            set_state(s_state_before_join);
        }
    } else if (st == ESP_PEER_STATE_DISCONNECTED) {
        ESP_LOGW(TAG, "media disconnected");
        /* Janus may hang up a silent single-party leg (keepalive). The server
         * may still think we have media (its hangup event can lag or be lost),
         * so self-heal locally: rejoin the room we believe we are in. */
        if (s_state == APP_STATE_TALKING && s_room_name[0] &&
            s_rejoin_count < MAX_MEDIA_REJOIN) {
            s_rejoin_count++;
            ESP_LOGI(TAG, "peer dropped; rejoin %s attempt %d", s_room_name, s_rejoin_count);
            teardown_media();
            set_state(APP_STATE_IDLE);
            start_join_ex("room_name", s_room_name, true);
        }
    }
}

static void on_confirm_result(app_event_t *ev)
{
    if (ev->flag) {
        set_state(APP_STATE_TALKING);
        strlcpy(s_room_name, ev->msg ? ev->msg : "", sizeof(s_room_name));
        strlcpy(s_room_code, ev->msg2 ? ev->msg2 : "", sizeof(s_room_code));
        s_online_count = ev->arg0;
        /* note: s_rejoin_count intentionally survives confirm — the budget caps
         * repeated drop->rejoin cycles across Janus keepalive periods. It is
         * reset by user-initiated joins (start_join from nfc/mcp/op). */
        ESP_LOGI(TAG, "in room %s (%s) online=%d", s_room_name, s_room_code, s_online_count);
        play_tone(880, 80); play_tone(1170, 120);   /* room-entry cue */
        if (s_mcp_op.active &&
            (mcp_op_is("self.intercom.join") || mcp_op_is("self.intercom.switch"))) {
            mcp_op_finish_ok_room();
        }
    } else {
        ESP_LOGW(TAG, "confirm failed");
        if (s_mcp_op.active) {
            mcp_op_finish_err("confirm_failed", "confirm rejected");
        }
        /* the Janus leg may still be attached on a failed switch: restore the
         * pre-join state and drop residual media so reported state matches
         * actual media */
        teardown_media();
        set_state(s_state_before_join == APP_STATE_JOINING ||
                  s_state_before_join == APP_STATE_SWITCHING ?
                  APP_STATE_IDLE : s_state_before_join);
    }
}

static void on_cmd_press(void)
{
    s_cmd_press_tick = xTaskGetTickCount();
    if (s_state == APP_STATE_TALKING) {
        s_state_before_cmd = s_state;
        set_state(APP_STATE_COMMAND);
    }
    /* hold-to-talk toward the Linx agent in both linx and intercom modes;
     * the actual listen start is deferred in capture_task */
    if (s_wake_listen) {        /* command key takes over an open wake round */
        s_wake_listen = false;
        wake_word_enable(true);
        linx_send_listen("stop", "auto");
    }
    if (!s_command_active) {
        s_command_active = true;
        s_linx_listen_sent = false;
        cmd_capture_begin();   /* keeps PCM stats for diagnostics */
    }
}

static void on_cmd_release(void)
{
    TickType_t held = xTaskGetTickCount() - s_cmd_press_tick;
    if (!s_command_active) {
        return;
    }
    s_command_active = false;
    if (s_linx_listen_sent) {
        s_linx_listen_sent = false;
        linx_send_listen("stop", NULL);
        open_tts_window();     /* the agent reply belongs to this round */
    }
    if (s_state == APP_STATE_COMMAND) {
        set_state(s_state_before_cmd);
    }
    if (held < pdMS_TO_TICKS(500)) {
        cmd_capture_abort();
        TickType_t now = xTaskGetTickCount();
        if (s_last_tap_tick && now - s_last_tap_tick < pdMS_TO_TICKS(700)) {
            s_last_tap_tick = 0;
            ESP_LOGI(TAG, "double-tap: mute toggle");
            if (ctrl_client_is_connected()) {
                ctrl_send_set_mute(!s_muted);
            }
        } else {
            s_last_tap_tick = now;
            ESP_LOGI(TAG, "cmd tap (hold to speak, double-tap to mute)");
        }
        return;
    }
    int samples = cmd_capture_finish();
    ESP_LOGI(TAG, "command audio sent to linx (%d samples)", samples);
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
        start_join(ev->flag ? "room_name" : "invite", room);
    } else if (strcmp(event, "media_lost") == 0) {
        /* janus hung up the leg (e.g. keepalive in a silent room). The
         * server cleared the session but kept the resume target: drop the
         * dead peer and rejoin by room name if we were in a room. Guard on
         * TALKING so a peer DISCONNECTED that already started a rejoin does
         * not trigger a second one. */
        if (s_state != APP_STATE_TALKING) {
            return;
        }
        ESP_LOGW(TAG, "media lost, rejoin attempt %d", s_rejoin_count);
        teardown_media();
        set_state(APP_STATE_IDLE);
        if (s_room_name[0] && s_rejoin_count < MAX_MEDIA_REJOIN) {
            s_rejoin_count++;
            start_join_ex("room_name", s_room_name, true);
        }
    }
}

/* ---------- MCP call dispatch ---------- */

/* immediate reply honoring the call's transport (linx mcp vs ctrl op) */
static void mcp_reply_now(const char *id_raw, bool from_ctrl,
                          const char *result_json, bool is_error)
{
    if (from_ctrl) {
        ctrl_send_op_result(id_raw, !is_error, result_json);
    } else {
        linx_mcp_reply_result(id_raw, result_json, is_error);
    }
}

static void on_mcp_call(app_event_t *ev)
{
    cJSON *call = cJSON_Parse(ev->msg ? ev->msg : "");
    if (call == NULL) {
        return;
    }
    cJSON *id = cJSON_GetObjectItem(call, "id");
    cJSON *name = cJSON_GetObjectItem(call, "name");
    cJSON *args = cJSON_GetObjectItem(call, "arguments");
    bool from_ctrl = cJSON_IsTrue(cJSON_GetObjectItem(call, "ctrl"));
    char *id_raw;
    if (from_ctrl && cJSON_IsString(id)) {
        id_raw = strdup(id->valuestring);          /* bare op_id */
    } else {
        id_raw = id ? cJSON_PrintUnformatted(id) : strdup("null");
    }
    const char *tool = cJSON_IsString(name) ? name->valuestring : "";

    ESP_LOGI(TAG, "mcp call %s id=%s%s", tool, id_raw, from_ctrl ? " (ctrl)" : "");
    if (s_mcp_op.active) {
        mcp_reply_now(id_raw, from_ctrl, "{\"ok\":false,\"error\":\"busy\"}", true);
        goto done;
    }
    strlcpy(s_mcp_op.id, id_raw, sizeof(s_mcp_op.id));
    strlcpy(s_mcp_op.name, tool, sizeof(s_mcp_op.name));
    s_mcp_op.from_ctrl = from_ctrl;

    if (strcmp(tool, "self.intercom.join") == 0) {
        cJSON *code = args ? cJSON_GetObjectItem(args, "invite_code") : NULL;
        const char *c = cJSON_IsString(code) ? code->valuestring : NULL;
        if (c == NULL || strlen(c) != 6) {
            mcp_reply_now(id_raw, from_ctrl,
                "{\"ok\":false,\"error\":\"bad_request\",\"message\":\"invite_code 需要六位数字\"}", true);
            goto done;
        }
        s_mcp_op.active = true;
        start_join("invite", c);
        if (!s_mcp_op.active) { /* start_join may have rejected silently */
            goto done;
        }
    } else if (strcmp(tool, "self.intercom.switch") == 0) {
        cJSON *rn = args ? cJSON_GetObjectItem(args, "room_name") : NULL;
        const char *r = cJSON_IsString(rn) ? rn->valuestring : NULL;
        if (r == NULL || r[0] == 0) {
            mcp_reply_now(id_raw, from_ctrl,
                "{\"ok\":false,\"error\":\"bad_request\",\"message\":\"room_name required\"}", true);
            goto done;
        }
        s_mcp_op.active = true;
        start_join("room_name", r);
    } else if (strcmp(tool, "self.intercom.status") == 0) {
        if (s_state == APP_STATE_TALKING) {
            s_mcp_op.active = true;
            mcp_op_arm_timer();
            ctrl_send_room_info();
        } else {
            char js[160];
            snprintf(js, sizeof(js),
                     "{\"ok\":true,\"mode\":\"%s\",\"muted\":%s}",
                     in_intercom() ? "intercom" : "linx", s_muted ? "true" : "false");
            mcp_reply_now(id_raw, from_ctrl, js, false);
        }
    } else if (strcmp(tool, "self.intercom.set_mute") == 0) {
        cJSON *m = args ? cJSON_GetObjectItem(args, "muted") : NULL;
        if (!cJSON_IsBool(m)) {
            mcp_reply_now(id_raw, from_ctrl,
                "{\"ok\":false,\"error\":\"bad_request\",\"message\":\"muted must be boolean\"}", true);
            goto done;
        }
        s_mcp_op.active = true;
        mcp_op_arm_timer();
        ctrl_send_set_mute(cJSON_IsTrue(m));
    } else if (strcmp(tool, "self.intercom.leave") == 0) {
        s_mcp_op.active = true;
        if (s_state == APP_STATE_JOINING || s_state == APP_STATE_SWITCHING) {
            s_mcp_op.active = false;
            mcp_reply_now(id_raw, from_ctrl, "{\"ok\":false,\"error\":\"busy\",\"message\":\"join in flight\"}", true);
        } else if (s_state == APP_STATE_TALKING || s_state == APP_STATE_COMMAND) {
            mcp_op_arm_timer();
            set_state(APP_STATE_LEAVING);
            ctrl_send_leave();
        } else {
            /* already in linx mode: report success without a room op */
            s_mcp_op.active = false;
            mcp_reply_now(id_raw, from_ctrl, "{\"ok\":true,\"left\":false,\"mode\":\"linx\"}", false);
        }
    } else {
        if (from_ctrl) {
            mcp_reply_now(id_raw, true, "{\"ok\":false,\"error\":\"unknown_tool\"}", true);
        } else {
            linx_mcp_reply_error(id_raw, -32601, "unknown tool");
        }
    }
done:
    free(id_raw);
    cJSON_Delete(call);
}

static void on_room_info_result(app_event_t *ev)
{
    ESP_LOGI(TAG, "room %s online %d", ev->msg ? ev->msg : "?", ev->arg0);
    if (ev->flag && ev->msg) {
        strlcpy(s_room_name, ev->msg, sizeof(s_room_name));
        if (ev->msg2) {
            strlcpy(s_room_code, ev->msg2, sizeof(s_room_code));
        }
        s_online_count = ev->arg0;
    }
    if (mcp_op_is("self.intercom.status")) {
        if (ev->flag) {
            mcp_op_finish_ok_room();
        } else {
            mcp_op_finish_err("not_in_room", "当前不在房间");
        }
    }
}

static void on_mute_result(app_event_t *ev)
{
    if (ev->flag) {
        apply_mute(ev->arg0 != 0);
    }
    if (mcp_op_is("self.intercom.set_mute")) {
        if (ev->flag) {
            char js[96];
            snprintf(js, sizeof(js), "{\"ok\":true,\"muted\":%s}", s_muted ? "true" : "false");
            mcp_op_finish(true, js);
        } else {
            mcp_op_finish_err("mute_failed", "set_mute rejected");
        }
    }
}

static void on_leave_result(app_event_t *ev)
{
    teardown_media();
    s_room_name[0] = 0;
    s_room_code[0] = 0;
    s_online_count = 0;
    set_state(APP_STATE_IDLE);
    if (mcp_op_is("self.intercom.leave")) {
        if (ev->flag) {
            mcp_op_finish(true, "{\"ok\":true,\"left\":true,\"mode\":\"linx\"}");
            play_tone(660, 150);                   /* room-exit cue */
        } else {
            mcp_op_finish_err("leave_failed", "leave rejected");
        }
    }
}

/* ---------- linx channel events ---------- */

static void on_linx_connected(void)
{
    linx_send_hello();
}

static void on_linx_disconnected(void)
{
    /* platform loss does not touch the room; retry in background.
     * A pending op reply is session-scoped and undeliverable — drop it so the
     * op budget is not held by a dead session. */
    if (s_wake_listen) {
        s_wake_listen = false;
        wake_word_enable(true);
    }
    s_linx_playing = false;
    if (s_mcp_op.active && !s_mcp_op.from_ctrl) {
        /* linx-scoped reply is undeliverable; ctrl ops still owe the server a
         * result on the other channel, keep them pending */
        ESP_LOGW(TAG, "linx down with pending op %s; dropping reply", s_mcp_op.name);
        s_mcp_op.active = false;
        mcp_op_cancel_timer();
    }
    if (wifi_sta_is_connected() && s_linx_started) {
        schedule_linx_reconnect(5000);
    }
}

static void on_linx_tools_listed(void)
{
#if CONFIG_INTERCOM_LINX_ENABLE
    if (strlen(CONFIG_INTERCOM_LINX_SELFTEST_TEXT) > 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        linx_send_detect_text(CONFIG_INTERCOM_LINX_SELFTEST_TEXT);
        open_tts_window();
    }
#endif
}

static void on_wake(void)
{
    if (s_wake_listen || s_command_active || !linx_client_is_connected()) {
        wake_word_enable(true);
        return;
    }
    ESP_LOGI(TAG, "wake -> linx listen round");
    s_wake_listen = true;
    s_wake_tick = xTaskGetTickCount();
    linx_send_detect_text("你好小智");
    linx_send_listen("start", "auto");
    open_tts_window();          /* the agent reply belongs to this round */
}

static void on_linx_tts(app_event_t *ev)
{
    s_linx_playing = ev->flag;
    if (ev->flag) {
        /* tts start inside an open window (or linx mode) plays; otherwise the
         * gate already drops the audio frames */
        if (s_wake_listen) {
            /* the platform answered the wake round; stop streaming */
            s_wake_listen = false;
            wake_word_enable(true);
        }
        /* tts start is the platform's own signal that reply audio follows:
         * open the gate now rather than counting from request time — slow
         * agent rounds must not outlive the window before audio arrives */
        open_tts_window();
    } else {
        /* tts stop: the reply round is over; close the window in intercom */
        if (in_intercom()) {
            s_tts_window_until = esp_timer_get_time();
        }
        update_linx_gate();
    }
}

static void on_op_timeout(void)
{
    if (!s_mcp_op.active &&
        (s_state == APP_STATE_JOINING || s_state == APP_STATE_SWITCHING)) {
        /* non-op join in flight timed out: fall back to pre-join state */
        ESP_LOGW(TAG, "op timeout with no pending op; unwinding state");
        set_state(s_state_before_join);
        return;
    }
    if (!s_mcp_op.active) {
        return;
    }
    ESP_LOGW(TAG, "mcp op %s timed out", s_mcp_op.name);
    if (s_state == APP_STATE_JOINING || s_state == APP_STATE_SWITCHING) {
        set_state(s_state_before_join);
    }
    mcp_op_finish_err("timeout", "operation timed out, query status for actual state");
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
        case APP_EV_MUTE_RESULT:       on_mute_result(&ev); break;
        case APP_EV_LEAVE_RESULT:      on_leave_result(&ev); break;
        case APP_EV_NOTIFY:            on_notify(&ev); break;
        case APP_EV_ERROR_MSG:
            ESP_LOGW(TAG, "server error %s: %s", ev.msg ? ev.msg : "?", ev.msg2 ? ev.msg2 : "");
            /* an error while an op is in flight is most plausibly that op's
             * failure (join rejection, mute denied...); settle it instead of
             * leaving the caller to hit the watchdog */
            if (s_mcp_op.active) {
                mcp_op_finish_err(ev.msg ? ev.msg : "server_error",
                                  ev.msg2 ? ev.msg2 : "server rejected");
                if (s_state == APP_STATE_JOINING || s_state == APP_STATE_SWITCHING) {
                    set_state(s_state_before_join);
                }
            }
            break;
        case APP_EV_PEER_SDP:
            if (ev.msg) ctrl_send_sdp(ev.msg);
            break;
        case APP_EV_PEER_ICE:
            if (ev.msg) ctrl_send_ice_candidate(ev.msg);
            break;
        case APP_EV_PEER_STATE:        on_peer_state(&ev); break;
        case APP_EV_CMD_PRESS:         on_cmd_press(); break;
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
        case APP_EV_ROOM_INFO_RESULT:  on_room_info_result(&ev); break;
        case APP_EV_RECONNECT_TIMER:
            if (!ctrl_client_is_connected() && wifi_sta_is_connected()) {
                ctrl_client_connect();
            }
            break;
        case APP_EV_LINX_RECONNECT_TIMER:
            if (wifi_sta_is_connected() && !linx_client_is_connected()) {
                linx_bootstrap();
            }
            break;
        case APP_EV_LINX_CONNECTED:    on_linx_connected(); break;
        case APP_EV_LINX_DISCONNECTED: on_linx_disconnected(); break;
        case APP_EV_LINX_HELLO:
            ESP_LOGI(TAG, "linx session up");
            break;
        case APP_EV_LINX_TOOLS_LISTED: on_linx_tools_listed(); break;
        case APP_EV_LINX_TTS:          on_linx_tts(&ev); break;
        case APP_EV_LINX_ABORT:
            s_tts_window_until = esp_timer_get_time();
            if (s_wake_listen) {
                s_wake_listen = false;
                wake_word_enable(true);
            }
            update_linx_gate();
            if (!in_intercom()) {
                audio_rx_flush();
            }
            break;
        case APP_EV_MCP_CALL:          on_mcp_call(&ev); break;
        case APP_EV_WAKE:           on_wake(); break;
        case APP_EV_NFC:
            if (ev.msg) start_join("nfc", ev.msg);
            break;
        case APP_EV_OP_TIMEOUT:        on_op_timeout(); break;
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

static void linx_reconnect_timer_cb(void *arg)
{
    app_post(APP_EV_LINX_RECONNECT_TIMER, NULL, NULL, 0, false);
}

static void op_timeout_cb(void *arg)
{
    app_post(APP_EV_OP_TIMEOUT, NULL, NULL, 0, false);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_ERROR_CHECK(nvs_store_init());
    g_app_queue = xQueueCreate(24, sizeof(app_event_t));
    configASSERT(g_app_queue);

    ESP_ERROR_CHECK(ctrl_client_init());
#if CONFIG_INTERCOM_LINX_ENABLE
    ESP_ERROR_CHECK(linx_client_init());
    if (strlen(CONFIG_INTERCOM_LINX_CLIENT_ID) == 0) {
        nvs_store_get_client_id(s_linx_client_id, sizeof(s_linx_client_id));
    }
#endif
    ESP_ERROR_CHECK(cmd_capture_init());
    ESP_ERROR_CHECK(audio_dev_init());
    ESP_ERROR_CHECK(audio_codec_init());
    audio_proc_init();
    ESP_ERROR_CHECK(cmd_input_init());
    if (wake_word_init() != ESP_OK) {
        ESP_LOGW(TAG, "wake word disabled (model/init failure)");
    }
    rtc_peer_init();          /* DTLS cert pre-gen (slow, once) */
#if CONFIG_INTERCOM_SPEAKER_ENABLE
    audio_dev_set_volume(nvs_store_get_volume());
#else
    audio_dev_set_volume(0);  /* bench build: keep playback silent */
#endif
    s_muted = nvs_store_get_muted();

    const esp_timer_create_args_t tmr = {
        .callback = reconnect_timer_cb,
        .name = "ws_reconnect",
    };
    esp_timer_create(&tmr, &s_reconnect_timer);
    const esp_timer_create_args_t tmr2 = {
        .callback = linx_reconnect_timer_cb,
        .name = "linx_reconnect",
    };
    esp_timer_create(&tmr2, &s_linx_reconnect_timer);
    const esp_timer_create_args_t tmr3 = {
        .callback = op_timeout_cb,
        .name = "op_timeout",
    };
    esp_timer_create(&tmr3, &s_op_timer);

    xTaskCreatePinnedToCore(capture_task, "aud_cap", 40 * 1024, NULL, 9, NULL, 0);
    xTaskCreatePinnedToCore(app_task, "app", 8 * 1024, NULL, 6, NULL, 0);

    ESP_ERROR_CHECK(wifi_sta_start());
}
