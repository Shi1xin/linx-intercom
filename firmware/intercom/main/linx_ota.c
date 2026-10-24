#include "linx_ota.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"

#define TAG "linx_ota"

#define OTA_RESP_MAX 8192

typedef struct {
    char  *buf;
    size_t len;
} resp_ctx_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && ctx->buf) {
        if (ctx->len + evt->data_len < OTA_RESP_MAX - 1) {
            memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
        }
    }
    return ESP_OK;
}

static void str_into(cJSON *obj, const char *key, char *dst, size_t n)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(v)) {
        strlcpy(dst, v->valuestring, n);
    }
}

esp_err_t linx_ota_fetch(const char *ota_url, const char *device_id,
                         const char *client_id, const char *board_type,
                         linx_ota_result_t *out)
{
    memset(out, 0, sizeof(*out));

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "version", 0);
    cJSON_AddStringToObject(body, "mac_address", device_id);
    cJSON_AddStringToObject(body, "uuid", client_id);
    cJSON *app = cJSON_AddObjectToObject(body, "application");
    cJSON_AddStringToObject(app, "name", "linx-intercom-fw");
    cJSON_AddStringToObject(app, "version", "0.2.0");
    cJSON_AddStringToObject(app, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(app, "elf_sha256", esp_app_get_elf_sha256_str());
    cJSON *board = cJSON_AddObjectToObject(body, "board");
    cJSON_AddStringToObject(board, "type", board_type);
    cJSON_AddStringToObject(board, "ssid", "");
    cJSON_AddNumberToObject(board, "rssi", 0);
    cJSON_AddStringToObject(body, "chip_model_name", "esp32s3");
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    resp_ctx_t ctx = {
        .buf = heap_caps_malloc(OTA_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .len = 0,
    };
    if (ctx.buf == NULL) {
        ctx.buf = malloc(OTA_RESP_MAX);
    }
    if (ctx.buf == NULL) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = ota_url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_evt,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t hc = esp_http_client_init(&cfg);
    esp_err_t err = ESP_FAIL;
    if (hc == NULL) {
        goto done;
    }
    esp_http_client_set_header(hc, "Content-Type", "application/json");
    esp_http_client_set_header(hc, "Device-Id", device_id);
    esp_http_client_set_header(hc, "Client-Id", client_id);
    esp_http_client_set_header(hc, "Activation-Version", "1");
    esp_http_client_set_header(hc, "User-Agent", "linx-intercom-fw/0.2.0");
    esp_http_client_set_post_field(hc, payload, strlen(payload));

    err = esp_http_client_perform(hc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota http: %s", esp_err_to_name(err));
        goto done;
    }
    int status = esp_http_client_get_status_code(hc);
    ctx.buf[ctx.len] = 0;
    ESP_LOGI(TAG, "ota status=%d len=%d", status, (int)ctx.len);
    if (status != 200) {
        err = ESP_FAIL;
        goto done;
    }
    {
        cJSON *resp = cJSON_ParseWithLength(ctx.buf, ctx.len);
        if (resp == NULL) {
            err = ESP_FAIL;
            goto done;
        }
        cJSON *ws = cJSON_GetObjectItem(resp, "websocket");
        if (cJSON_IsObject(ws)) {
            str_into(ws, "url", out->ws_url, sizeof(out->ws_url));
            str_into(ws, "token", out->token, sizeof(out->token));
        }
        cJSON *act = cJSON_GetObjectItem(resp, "activation");
        if (cJSON_IsObject(act)) {
            str_into(act, "code", out->activation_code, sizeof(out->activation_code));
        }
        cJSON *fw = cJSON_GetObjectItem(resp, "firmware");
        if (cJSON_IsObject(fw)) {
            str_into(fw, "version", out->firmware_version, sizeof(out->firmware_version));
            str_into(fw, "url", out->firmware_url, sizeof(out->firmware_url));
        }
        out->bound = out->token[0] != 0;
        cJSON_Delete(resp);
        err = ESP_OK;
    }
done:
    esp_http_client_cleanup(hc);
    free(payload);
    free(ctx.buf);
    return err;
}
