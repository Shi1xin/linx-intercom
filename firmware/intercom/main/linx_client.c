/* Linx platform WebSocket + device-side MCP server.
 *
 * One esp_websocket_client task owns the connection; inbound text JSON is
 * dispatched here, binary frames are platform TTS opus packets.
 * Tool calls are forwarded to the app queue as APP_EV_MCP_CALL (msg = packed
 * JSON {"id":<raw>,"name":"..","arguments":{..}}); the app answers through
 * linx_mcp_reply_result() once the room op actually finished.
 */
#include "linx_client.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app_events.h"
#include "audio_dev.h"

#define TAG "linx"
#define RX_BUF_SIZE 16384
#define MCP_ID_MAX   48

static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t s_tx_lock;
static volatile bool s_connected;
static volatile bool s_play_gate = true;   /* default linx mode plays TTS */

static char  *s_rx_buf;
static size_t s_rx_len;

static char s_session_id[64];
static char s_device_id[24];
static char s_client_id[48];
static char s_token[280];
static char s_url[160];

static const char *k_tools_json =
    "[{\"name\":\"self.intercom.join\",\"description\":"
    "\"按六位数字邀请码让本设备加入同频对讲房间。成功后设备进入对讲模式。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"invite_code\":"
    "{\"type\":\"string\",\"description\":\"六位数字邀请码，逐位数字组合成字符串\"}},"
    "\"required\":[\"invite_code\"]}},"
    "{\"name\":\"self.intercom.switch\",\"description\":"
    "\"把本设备切换到已加入过的另一个编号房间。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"room_name\":"
    "{\"type\":\"string\",\"description\":\"房间名，例如 三号房\"}},"
    "\"required\":[\"room_name\"]}},"
    "{\"name\":\"self.intercom.status\",\"description\":"
    "\"查询本设备当前模式、房间、人数、邀请码与静音状态。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"self.intercom.set_mute\",\"description\":"
    "\"设置本设备对讲麦克风静音状态。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"muted\":"
    "{\"type\":\"boolean\",\"description\":\"true 静音，false 取消\"}},"
    "\"required\":[\"muted\"]}},"
    "{\"name\":\"self.intercom.leave\",\"description\":"
    "\"退出当前对讲房间，设备回到灵矽对话模式。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]";

/* ---------- low-level send ---------- */

static esp_err_t linx_send_text_n(const char *text, int len)
{
    esp_err_t ret = ESP_FAIL;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (s_ws && s_connected) {
        int r = esp_websocket_client_send_text(s_ws, text, len, pdMS_TO_TICKS(3000));
        ret = (r >= 0) ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(s_tx_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send failed (%d)", ret);
    }
    return ret;
}

static esp_err_t linx_send_json(cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = linx_send_text_n(text, strlen(text));
    free(text);
    return ret;
}

int linx_send_audio(const uint8_t *data, int size)
{
    int r = -1;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (s_ws && s_connected) {
        r = esp_websocket_client_send_bin(s_ws, (const char *)data, size,
                                          pdMS_TO_TICKS(500));
    }
    xSemaphoreGive(s_tx_lock);
    return r;
}

/* ---------- session messages ---------- */

esp_err_t linx_send_hello(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON *feat = cJSON_AddObjectToObject(root, "features");
    cJSON_AddBoolToObject(feat, "mcp", true);
    cJSON *ap = cJSON_AddObjectToObject(root, "audio_params");
    cJSON_AddStringToObject(ap, "format", "opus");
    cJSON_AddNumberToObject(ap, "sample_rate", 16000);
    cJSON_AddNumberToObject(ap, "channels", 1);
    cJSON_AddNumberToObject(ap, "frame_duration", 20);
    return linx_send_json(root);
}

esp_err_t linx_send_listen(const char *state, const char *mode)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", state);
    if (mode) {
        cJSON_AddStringToObject(root, "mode", mode);
    }
    if (s_session_id[0]) {
        cJSON_AddStringToObject(root, "session_id", s_session_id);
    }
    return linx_send_json(root);
}

esp_err_t linx_send_detect_text(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "detect");
    cJSON_AddStringToObject(root, "text", text);
    if (s_session_id[0]) {
        cJSON_AddStringToObject(root, "session_id", s_session_id);
    }
    return linx_send_json(root);
}

esp_err_t linx_send_abort(const char *reason)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "abort");
    cJSON_AddStringToObject(root, "reason", reason);
    if (s_session_id[0]) {
        cJSON_AddStringToObject(root, "session_id", s_session_id);
    }
    return linx_send_json(root);
}

/* ---------- MCP replies ---------- */

esp_err_t linx_mcp_reply_result(const char *id_raw, const char *text_json, bool is_error)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON *p = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(p, "jsonrpc", "2.0");
    cJSON_AddRawToObject(p, "id", (id_raw && id_raw[0]) ? id_raw : "null");
    cJSON *res = cJSON_AddObjectToObject(p, "result");
    cJSON *content = cJSON_AddArrayToObject(res, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text_json ? text_json : "{}");
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(res, "isError", is_error);
    return linx_send_json(root);
}

esp_err_t linx_mcp_reply_error(const char *id_raw, int code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON *p = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(p, "jsonrpc", "2.0");
    cJSON_AddRawToObject(p, "id", (id_raw && id_raw[0]) ? id_raw : "null");
    cJSON *e = cJSON_AddObjectToObject(p, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return linx_send_json(root);
}

/* ---------- inbound MCP ---------- */

static void mcp_initialize_reply(cJSON *payload, cJSON *id)
{
    char *id_raw = id ? cJSON_PrintUnformatted(id) : NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON *p = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(p, "jsonrpc", "2.0");
    cJSON_AddRawToObject(p, "id", id_raw ? id_raw : "null");
    cJSON *res = cJSON_AddObjectToObject(p, "result");
    cJSON *params = cJSON_GetObjectItem(payload, "params");
    cJSON *pv = params ? cJSON_GetObjectItem(params, "protocolVersion") : NULL;
    cJSON_AddStringToObject(res, "protocolVersion",
                            cJSON_IsString(pv) ? pv->valuestring : "2024-11-05");
    cJSON *caps = cJSON_AddObjectToObject(res, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    cJSON *info = cJSON_AddObjectToObject(res, "serverInfo");
    cJSON_AddStringToObject(info, "name", "linx-intercom");
    cJSON_AddStringToObject(info, "version", "0.2.0");
    linx_send_json(root);
    free(id_raw);
}

static void mcp_tools_list_reply(cJSON *id)
{
    char *id_raw = id ? cJSON_PrintUnformatted(id) : NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON *p = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(p, "jsonrpc", "2.0");
    cJSON_AddRawToObject(p, "id", id_raw ? id_raw : "null");
    cJSON *res = cJSON_AddObjectToObject(p, "result");
    cJSON_AddRawToObject(res, "tools", k_tools_json);
    linx_send_json(root);
    free(id_raw);
    app_post(APP_EV_LINX_TOOLS_LISTED, NULL, NULL, 0, false);
}

static void handle_mcp(cJSON *payload)
{
    cJSON *method = cJSON_GetObjectItem(payload, "method");
    cJSON *id = cJSON_GetObjectItem(payload, "id");
    if (!cJSON_IsString(method)) {
        return;
    }
    if (strcmp(method->valuestring, "initialize") == 0) {
        mcp_initialize_reply(payload, id);
    } else if (strcmp(method->valuestring, "tools/list") == 0) {
        mcp_tools_list_reply(id);
    } else if (strcmp(method->valuestring, "tools/call") == 0) {
        /* forward packed call to the app task; it owns the reply */
        cJSON *params = cJSON_GetObjectItem(payload, "params");
        cJSON *out = cJSON_CreateObject();
        cJSON_AddItemToObject(out, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
        if (cJSON_IsObject(params)) {
            cJSON *name = cJSON_GetObjectItem(params, "name");
            cJSON *args = cJSON_GetObjectItem(params, "arguments");
            cJSON_AddItemToObject(out, "name",
                cJSON_IsString(name) ? cJSON_CreateString(name->valuestring)
                                     : cJSON_CreateString(""));
            cJSON_AddItemToObject(out, "arguments",
                args ? cJSON_Duplicate(args, true) : cJSON_CreateObject());
        }
        char *packed = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
        if (packed) {
            app_post(APP_EV_MCP_CALL, packed, NULL, 0, false); /* ownership moves */
        }
    } else {
        /* notifications have no id: ignore silently */
        if (id != NULL) {
            char *id_raw = cJSON_PrintUnformatted(id);
            linx_mcp_reply_error(id_raw, -32601, "unsupported method");
            free(id_raw);
        }
    }
}

/* ---------- inbound text dispatch ---------- */

static void handle_text(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "bad json: %.80s", data);
        return;
    }
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    const char *t = type->valuestring;
    if (strcmp(t, "hello") == 0) {
        cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid)) {
            strlcpy(s_session_id, sid->valuestring, sizeof(s_session_id));
        }
        ESP_LOGI(TAG, "platform hello session=%s", s_session_id);
        app_post(APP_EV_LINX_HELLO, NULL, NULL, 0, true);
    } else if (strcmp(t, "mcp") == 0) {
        handle_mcp(cJSON_GetObjectItem(root, "payload"));
    } else if (strcmp(t, "tts") == 0) {
        cJSON *st = cJSON_GetObjectItem(root, "state");
        bool start = cJSON_IsString(st) && strcmp(st->valuestring, "start") == 0;
        bool stop = cJSON_IsString(st) &&
            (strcmp(st->valuestring, "stop") == 0 || strcmp(st->valuestring, "sentence_start") == 0);
        app_post(APP_EV_LINX_TTS, NULL, NULL, 0, start);
        (void)stop;
    } else if (strcmp(t, "abort") == 0) {
        app_post(APP_EV_LINX_ABORT, NULL, NULL, 0, false);
    } else if (strcmp(t, "goodbye") == 0) {
        ESP_LOGI(TAG, "platform closed session (goodbye)");
    } else if (strcmp(t, "stt") == 0 || strcmp(t, "llm") == 0 ||
               strcmp(t, "listen") == 0) {
        ESP_LOGI(TAG, "%s", t);
    } else {
        ESP_LOGI(TAG, "unhandled linx type %s", t);
    }
    cJSON_Delete(root);
}

/* ---------- ws events ---------- */

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "linx ws connected");
        s_connected = true;
        s_rx_len = 0;
        s_session_id[0] = 0;
        app_post(APP_EV_LINX_CONNECTED, NULL, NULL, 0, false);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "linx ws disconnected");
        s_connected = false;
        app_post(APP_EV_LINX_DISCONNECTED, NULL, NULL, 0, false);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code == 0x2) {
            /* binary = platform TTS opus frame */
            if (s_play_gate) {
                audio_rx_enqueue((const uint8_t *)e->data_ptr, e->data_len);
            }
            break;
        }
        if (e->op_code != 0x1 && e->op_code != 0x0) {
            break;
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
            ESP_LOGW(TAG, "linx frame too large %d", (int)e->payload_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "linx ws error");
        break;
    default:
        break;
    }
}

/* ---------- lifecycle ---------- */

esp_err_t linx_client_init(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    s_rx_buf = heap_caps_malloc(RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_rx_buf == NULL) {
        s_rx_buf = malloc(RX_BUF_SIZE);
    }
    return (s_tx_lock && s_rx_buf) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t linx_client_connect(const char *url, const char *token,
                              const char *device_id, const char *client_id)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        s_connected = false;
    }
    strlcpy(s_url, url, sizeof(s_url));
    strlcpy(s_token, token, sizeof(s_token));
    strlcpy(s_device_id, device_id, sizeof(s_device_id));
    strlcpy(s_client_id, client_id, sizeof(s_client_id));

    /* upgrade plain ws to wss for the public endpoint */
    char uri[176];
    if (strncmp(url, "ws://", 5) == 0) {
        snprintf(uri, sizeof(uri), "wss://%s", url + 5);
    } else {
        strlcpy(uri, url, sizeof(uri));
    }

    char auth[320];
    snprintf(auth, sizeof(auth), "Bearer %s", s_token);
    static char hdr_block[768];
    snprintf(hdr_block, sizeof(hdr_block),
             "Authorization: %s\r\nDevice-Id: %s\r\nClient-Id: %s\r\n"
             "Protocol-Version: 1\r\n",
             auth, s_device_id, s_client_id);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .headers = hdr_block,
        .buffer_size = 4096,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 15000,
        .ping_interval_sec = 25,
        .task_stack = 8192,
        .task_prio = 8,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    return esp_websocket_client_start(s_ws);
}

esp_err_t linx_client_disconnect(void)
{
    if (s_ws) {
        s_connected = false;
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    return ESP_OK;
}

bool linx_client_is_connected(void)
{
    return s_connected;
}

void linx_set_playback_gate(bool allow)
{
    s_play_gate = allow;
}
