/* Control channel client (docs/control-protocol.md).
 *
 * Runs inside the esp_websocket_client task; inbound JSON is parsed here and
 * forwarded to the app queue as app_event_t.
 */
#include "ctrl_client.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "app_events.h"

#define TAG "ctrl"

#define RX_BUF_SIZE 16384

static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t s_tx_lock;
static volatile bool s_connected;
static int s_req_seq;

/* reassembly buffer for fragmented text frames */
static char  *s_rx_buf;
static size_t s_rx_len;

static const char *next_req_id(char *buf, size_t n)
{
    snprintf(buf, n, "r%d", ++s_req_seq);
    return buf;
}

static esp_err_t ctrl_send_json(cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_FAIL;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (s_ws && s_connected) {
        int r = esp_websocket_client_send_text(s_ws, text, strlen(text), pdMS_TO_TICKS(3000));
        ret = (r >= 0) ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(s_tx_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send failed (%d)", ret);
    }
    free(text);
    return ret;
}

/* ---------- outbound protocol ---------- */

esp_err_t ctrl_send_hello(const char *device_id)
{
    char rid[16];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "token", CONFIG_INTERCOM_DEVICE_TOKEN);
    cJSON_AddStringToObject(root, "request_id", next_req_id(rid, sizeof(rid)));
    return ctrl_send_json(root);
}

esp_err_t ctrl_send_join(const char *kind, const char *arg)
{
    char rid[16];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "join");
    cJSON_AddStringToObject(root, "request_id", next_req_id(rid, sizeof(rid)));
    cJSON *target = cJSON_AddObjectToObject(root, "target");
    cJSON_AddStringToObject(target, "kind", kind);
    if (strcmp(kind, "nfc") == 0) {
        cJSON_AddStringToObject(target, "entry_id", arg);
    } else if (strcmp(kind, "device") == 0) {
        cJSON_AddStringToObject(target, "device_id", arg);
    } else if (strcmp(kind, "invite") == 0) {
        cJSON_AddStringToObject(target, "code", arg);
    } else { /* room_name */
        cJSON_AddStringToObject(target, "name", arg);
    }
    return ctrl_send_json(root);
}

esp_err_t ctrl_send_sdp(const char *sdp)
{
    char rid[16];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "sdp");
    cJSON_AddStringToObject(root, "request_id", next_req_id(rid, sizeof(rid)));
    cJSON_AddStringToObject(root, "sdp", sdp);
    return ctrl_send_json(root);
}

esp_err_t ctrl_send_ice_candidate(const char *candidate)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ice");
    cJSON *cand = cJSON_AddObjectToObject(root, "candidate");
    cJSON_AddStringToObject(cand, "candidate", candidate);
    cJSON_AddStringToObject(cand, "sdpMid", "0");
    cJSON_AddNumberToObject(cand, "sdpMLineIndex", 0);
    return ctrl_send_json(root);
}

esp_err_t ctrl_send_ice_completed(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ice");
    cJSON_AddBoolToObject(root, "completed", true);
    return ctrl_send_json(root);
}

esp_err_t ctrl_send_confirm(void)
{
    char rid[16];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "confirm");
    cJSON_AddStringToObject(root, "request_id", next_req_id(rid, sizeof(rid)));
    return ctrl_send_json(root);
}

esp_err_t ctrl_send_room_info(void)
{
    char rid[16];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "room_info");
    cJSON_AddStringToObject(root, "request_id", next_req_id(rid, sizeof(rid)));
    return ctrl_send_json(root);
}

esp_err_t ctrl_send_set_mute(bool muted)
{
    char rid[16];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "set_mute");
    cJSON_AddStringToObject(root, "request_id", next_req_id(rid, sizeof(rid)));
    cJSON_AddBoolToObject(root, "muted", muted);
    return ctrl_send_json(root);
}

esp_err_t ctrl_send_leave(void)
{
    char rid[16];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "leave");
    cJSON_AddStringToObject(root, "request_id", next_req_id(rid, sizeof(rid)));
    return ctrl_send_json(root);
}

esp_err_t ctrl_send_op_result(const char *op_id, bool ok, const char *result_json)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "op_result");
    cJSON_AddStringToObject(root, "op_id", op_id);
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", result_json ? result_json : "{}");
    return ctrl_send_json(root);
}

/* ---------- inbound dispatch ---------- */

static void post2(app_ev_type_t t, const cJSON *m1, const cJSON *m2, int arg0, bool flag)
{
    app_post_str(t, m1 ? m1->valuestring : NULL, m2 ? m2->valuestring : NULL, arg0, flag);
}

static void dispatch_message(cJSON *root)
{
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        return;
    }
    const char *t = type->valuestring;

    if (strcmp(t, "hello_result") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *muted = cJSON_GetObjectItem(root, "mute");
        if (!muted) {
            muted = cJSON_GetObjectItem(root, "muted");
        }
        cJSON *resume = cJSON_GetObjectItem(root, "resume_room");
        const char *rname = NULL, *rcode = NULL;
        if (cJSON_IsObject(resume)) {
            cJSON *n = cJSON_GetObjectItem(resume, "name");
            cJSON *c = cJSON_GetObjectItem(resume, "invite_code");
            rname = cJSON_IsString(n) ? n->valuestring : NULL;
            rcode = cJSON_IsString(c) ? c->valuestring : NULL;
        }
        app_post_str(APP_EV_HELLO_RESULT, rname, rcode,
                     cJSON_IsBool(muted) ? cJSON_IsTrue(muted) : 0,
                     cJSON_IsTrue(ok));
    } else if (strcmp(t, "join_result") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *need = cJSON_GetObjectItem(root, "need_media");
        cJSON *room = cJSON_GetObjectItem(root, "room");
        cJSON *err = cJSON_GetObjectItem(root, "error");
        const char *m1 = NULL;
        if (cJSON_IsObject(room)) {
            cJSON *n = cJSON_GetObjectItem(room, "name");
            m1 = cJSON_IsString(n) ? n->valuestring : NULL;
        }
        if (!cJSON_IsTrue(ok) && cJSON_IsObject(err)) {
            cJSON *m = cJSON_GetObjectItem(err, "message");
            m1 = cJSON_IsString(m) ? m->valuestring : "join failed";
        }
        app_post_str(APP_EV_JOIN_RESULT, m1, NULL,
                     cJSON_IsTrue(need), cJSON_IsTrue(ok));
    } else if (strcmp(t, "sdp") == 0) {
        cJSON *sdp = cJSON_GetObjectItem(root, "sdp");
        if (cJSON_IsString(sdp)) {
            app_post_str(APP_EV_SDP, sdp->valuestring, NULL, 0, true);
        }
    } else if (strcmp(t, "ice") == 0) {
        cJSON *completed = cJSON_GetObjectItem(root, "completed");
        if (cJSON_IsTrue(completed)) {
            app_post(APP_EV_ICE, NULL, NULL, 0, true);
        } else {
            cJSON *cand = cJSON_GetObjectItem(root, "candidate");
            if (cJSON_IsObject(cand)) {
                cJSON *cs = cJSON_GetObjectItem(cand, "candidate");
                if (cJSON_IsString(cs)) {
                    app_post_str(APP_EV_ICE, cs->valuestring, NULL, 0, false);
                }
            }
        }
    } else if (strcmp(t, "confirm_result") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *room = cJSON_GetObjectItem(root, "room");
        cJSON *n = cJSON_IsObject(room) ? cJSON_GetObjectItem(room, "name") : NULL;
        cJSON *c = cJSON_IsObject(room) ? cJSON_GetObjectItem(room, "invite_code") : NULL;
        cJSON *o = cJSON_IsObject(room) ? cJSON_GetObjectItem(room, "online_count") : NULL;
        app_post_str(APP_EV_CONFIRM_RESULT, cJSON_IsString(n) ? n->valuestring : NULL,
                     cJSON_IsString(c) ? c->valuestring : NULL,
                     cJSON_IsNumber(o) ? o->valueint : 0, cJSON_IsTrue(ok));
    } else if (strcmp(t, "room_info_result") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *name = cJSON_GetObjectItem(root, "name");
        cJSON *cnt = cJSON_GetObjectItem(root, "online_count");
        cJSON *code = cJSON_GetObjectItem(root, "invite_code");
        app_post_str(APP_EV_ROOM_INFO_RESULT,
                     cJSON_IsString(name) ? name->valuestring : NULL,
                     cJSON_IsString(code) ? code->valuestring : NULL,
                     cJSON_IsNumber(cnt) ? cnt->valueint : 0, cJSON_IsTrue(ok));
    } else if (strcmp(t, "mute_result") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *muted = cJSON_GetObjectItem(root, "mute");
        if (!muted) {
            muted = cJSON_GetObjectItem(root, "muted");
        }
        app_post(APP_EV_MUTE_RESULT, NULL, NULL,
                 cJSON_IsTrue(muted), cJSON_IsTrue(ok));
    } else if (strcmp(t, "leave_result") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        app_post(APP_EV_LEAVE_RESULT, NULL, NULL, 0, cJSON_IsTrue(ok));
    } else if (strcmp(t, "notify") == 0) {
        cJSON *ev = cJSON_GetObjectItem(root, "event");
        cJSON *room = cJSON_GetObjectItem(root, "room");
        cJSON *n = NULL, *c = NULL;
        if (cJSON_IsObject(room)) {
            n = cJSON_GetObjectItem(room, "name");
            c = cJSON_GetObjectItem(room, "invite_code");
        }
        bool summon = cJSON_IsString(ev) && strcmp(ev->valuestring, "summon") == 0;
        /* summon: use invite code (device has no membership yet);
           others: use room name for display */
        const char *m2;
        bool is_name;
        if (summon && cJSON_IsString(c)) {
            m2 = c->valuestring; is_name = false;
        } else if (cJSON_IsString(n)) {
            m2 = n->valuestring; is_name = true;
        } else {
            m2 = cJSON_IsString(c) ? c->valuestring : NULL; is_name = false;
        }
        app_post_str(APP_EV_NOTIFY, cJSON_IsString(ev) ? ev->valuestring : NULL,
                     m2, 0, is_name);
    } else if (strcmp(t, "op") == 0) {
        /* server-initiated op (cloud MCP). Translate to the packed MCP call
         * format so the app runs it through the same tool dispatcher; the
         * reply goes back over this channel via op_result. */
        cJSON *opid = cJSON_GetObjectItem(root, "op_id");
        cJSON *op = cJSON_GetObjectItem(root, "op");
        cJSON *arg = cJSON_GetObjectItem(root, "op_arg");
        cJSON *muted = cJSON_GetObjectItem(root, "mute");
        if (!muted) {
            muted = cJSON_GetObjectItem(root, "muted");
        }
        if (!cJSON_IsString(opid) || !cJSON_IsString(op)) {
            return;
        }
        const char *tool = NULL;
        const char *akey = NULL;
        if (strcmp(op->valuestring, "join") == 0) {
            tool = "self.intercom.join"; akey = "invite_code";
        } else if (strcmp(op->valuestring, "switch") == 0) {
            tool = "self.intercom.switch"; akey = "room_name";
        } else if (strcmp(op->valuestring, "status") == 0) {
            tool = "self.intercom.status";
        } else if (strcmp(op->valuestring, "set_mute") == 0) {
            tool = "self.intercom.set_mute";
        } else if (strcmp(op->valuestring, "leave") == 0) {
            tool = "self.intercom.leave";
        }
        if (tool == NULL) {
            ctrl_send_op_result(opid->valuestring, false,
                                "{\"ok\":false,\"error\":\"bad_request\"}");
            return;
        }
        cJSON *call = cJSON_CreateObject();
        cJSON_AddStringToObject(call, "id", opid->valuestring);
        cJSON_AddStringToObject(call, "name", tool);
        cJSON *args = cJSON_AddObjectToObject(call, "arguments");
        if (akey && cJSON_IsString(arg)) {
            cJSON_AddStringToObject(args, akey, arg->valuestring);
        }
        if (cJSON_IsBool(muted)) {
            cJSON_AddBoolToObject(args, "muted", cJSON_IsTrue(muted));
        }
        cJSON_AddBoolToObject(call, "ctrl", true);
        char *packed = cJSON_PrintUnformatted(call);
        cJSON_Delete(call);
        if (packed) {
            app_post(APP_EV_MCP_CALL, packed, NULL, 0, false);
        }
    } else if (strcmp(t, "error") == 0) {
        cJSON *e = cJSON_GetObjectItem(root, "error");
        cJSON *code = cJSON_IsObject(e) ? cJSON_GetObjectItem(e, "code") : NULL;
        cJSON *msg = cJSON_IsObject(e) ? cJSON_GetObjectItem(e, "message") : NULL;
        post2(APP_EV_ERROR_MSG, cJSON_IsString(code) ? code : NULL,
              cJSON_IsString(msg) ? msg : NULL, 0, false);
    } else {
        ESP_LOGW(TAG, "unhandled type %s", t);
    }
}

static void handle_text(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "bad json: %.80s", data);
        return;
    }
    dispatch_message(root);
    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws connected");
        s_connected = true;
        s_rx_len = 0;
        app_post(APP_EV_WS_CONNECTED, NULL, NULL, 0, false);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "ws disconnected");
        s_connected = false;
        s_rx_len = 0;
        app_post(APP_EV_WS_DISCONNECTED, NULL, NULL, 0, false);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code != 0x1 && e->op_code != 0x0) {
            break; /* text frames + continuations only */
        }
        if (e->payload_offset == 0) {
            s_rx_len = 0;
        }
        if (e->payload_offset + e->data_len <= RX_BUF_SIZE - 1) {
            memcpy(s_rx_buf + e->payload_offset, e->data_ptr, e->data_len);
            s_rx_len = e->payload_offset + e->data_len;
            if (s_rx_len >= e->payload_len) {
                s_rx_buf[s_rx_len] = 0;
                handle_text(s_rx_buf, s_rx_len);
                s_rx_len = 0;
            }
        } else {
            ESP_LOGW(TAG, "ws frame too large %d", (int)e->payload_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "ws error");
        break;
    default:
        break;
    }
}

esp_err_t ctrl_client_init(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    s_rx_buf = heap_caps_malloc(RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_rx_buf == NULL) {
        s_rx_buf = malloc(RX_BUF_SIZE);
    }
    return (s_tx_lock && s_rx_buf) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ctrl_client_connect(void)
{
    if (s_ws) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        s_connected = false;
        xSemaphoreGive(s_tx_lock);
    }
    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_INTERCOM_CTRL_URL,
        .buffer_size = 4096,
        .disable_auto_reconnect = true, /* we manage reconnect via app timer */
        .network_timeout_ms = 10000,
        .ping_interval_sec = 30,
        .task_stack = 6144,
        .task_prio = 8,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    return esp_websocket_client_start(s_ws);
}

esp_err_t ctrl_client_disconnect(void)
{
    if (s_ws) {
        s_connected = false;
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        xSemaphoreGive(s_tx_lock);
    }
    return ESP_OK;
}

bool ctrl_client_is_connected(void)
{
    return s_connected;
}
