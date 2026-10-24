#include "wake_trace.h"

#if CONFIG_INTERCOM_WAKE_PCM_TRACE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

#define TAG "wake_trace"
#define TRACE_SAMPLES (16000 * 8)

static int16_t *s_ring;
static int16_t *s_snapshot;
static size_t s_write;
static size_t s_filled;
static size_t s_snapshot_samples;
static atomic_bool s_ready;
static atomic_bool s_requested;
static atomic_bool s_busy;
static SemaphoreHandle_t s_upload;

static void prepare_snapshot(void)
{
    if (!s_requested || s_busy || s_filled == 0) {
        return;
    }
    s_requested = false;
    s_busy = true;
    size_t start = s_filled == TRACE_SAMPLES ? s_write : 0;
    size_t first = TRACE_SAMPLES - start;
    if (first > s_filled) {
        first = s_filled;
    }
    memcpy(s_snapshot, s_ring + start, first * sizeof(int16_t));
    memcpy(s_snapshot + first, s_ring, (s_filled - first) * sizeof(int16_t));
    s_snapshot_samples = s_filled;
    xSemaphoreGive(s_upload);
}

void wake_trace_feed(const int16_t *pcm, size_t samples)
{
    if (!s_ready || !pcm) {
        return;
    }
    while (samples > 0) {
        size_t take = TRACE_SAMPLES - s_write;
        if (take > samples) {
            take = samples;
        }
        memcpy(s_ring + s_write, pcm, take * sizeof(int16_t));
        s_write = (s_write + take) % TRACE_SAMPLES;
        s_filled += take;
        if (s_filled > TRACE_SAMPLES) {
            s_filled = TRACE_SAMPLES;
        }
        pcm += take;
        samples -= take;
    }
    prepare_snapshot();
}

void wake_trace_request(void)
{
    if (s_ready) {
        s_requested = true;
    }
}

static void dump_snapshot_serial(const char *hash)
{
    static unsigned sequence;
    unsigned id = ++sequence;
    printf("PCMTRACE BEGIN id=%u samples=%u sha256=%s\n",
           id, (unsigned)s_snapshot_samples, hash);
    /* small writes: limits contention with the periodic capture diagnostics
     * printing on the same console */
    for (size_t off = 0; off < s_snapshot_samples; off += 64) {
        size_t count = s_snapshot_samples - off;
        if (count > 64) {
            count = 64;
        }
        char hex[64 * 4 + 1];
        for (size_t i = 0; i < count; ++i) {
            uint16_t sample = (uint16_t)s_snapshot[off + i];
            snprintf(hex + i * 4, 5, "%02x%02x",
                     (unsigned)(sample & 0xff), (unsigned)(sample >> 8));
        }
        printf("PCMTRACE DATA id=%u offset=%u hex=%s\n", id, (unsigned)off, hex);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    printf("PCMTRACE END id=%u\n", id);
}

static void upload_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_upload, portMAX_DELAY);
        size_t bytes = s_snapshot_samples * sizeof(int16_t);
        unsigned char digest[32];
        char hash[65];
        char sample_count[24];
        if (mbedtls_sha256((const unsigned char *)s_snapshot, bytes, digest, 0) != 0) {
            ESP_LOGE(TAG, "PCM digest failed");
            s_busy = false;
            continue;
        }
        for (int i = 0; i < 32; ++i) {
            snprintf(hash + i * 2, 3, "%02x", digest[i]);
        }
        snprintf(sample_count, sizeof(sample_count), "%u", (unsigned)s_snapshot_samples);
        esp_http_client_config_t cfg = {
            .url = CONFIG_INTERCOM_WAKE_PCM_TRACE_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 5000,
            .disable_auto_redirect = true,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_err_t err = ESP_ERR_NO_MEM;
        int status = 0;
        if (client) {
            esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
            esp_http_client_set_header(client, "Authorization",
                                       "Bearer " CONFIG_INTERCOM_WAKE_PCM_TRACE_TOKEN);
            esp_http_client_set_header(client, "X-Linx-PCM-Samples", sample_count);
            esp_http_client_set_header(client, "X-Linx-PCM-SHA256", hash);
            esp_http_client_set_post_field(client, (const char *)s_snapshot, (int)bytes);
            err = esp_http_client_perform(client);
            status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);
        }
        ESP_LOGI(TAG, "PCM samples=%u sha256=%s status=%d err=%s",
                 (unsigned)s_snapshot_samples, hash, status, esp_err_to_name(err));
        if (err != ESP_OK || status != 200) {
            dump_snapshot_serial(hash);
        }
        s_busy = false;
    }
}

esp_err_t wake_trace_init(void)
{
    if (strncmp(CONFIG_INTERCOM_WAKE_PCM_TRACE_URL, "http://", 7) != 0 ||
        strlen(CONFIG_INTERCOM_WAKE_PCM_TRACE_TOKEN) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ring = heap_caps_malloc(TRACE_SAMPLES * sizeof(int16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_snapshot = heap_caps_malloc(TRACE_SAMPLES * sizeof(int16_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_upload = xSemaphoreCreateBinary();
    if (!s_ring || !s_snapshot || !s_upload) {
        free(s_ring);
        free(s_snapshot);
        s_ring = NULL;
        s_snapshot = NULL;
        if (s_upload) {
            vSemaphoreDelete(s_upload);
            s_upload = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(upload_task, "wake_trace", 6 * 1024, NULL, 3, NULL) != pdPASS) {
        free(s_ring);
        free(s_snapshot);
        s_ring = NULL;
        s_snapshot = NULL;
        vSemaphoreDelete(s_upload);
        s_upload = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGW(TAG, "PCM trace enabled; unmute requests an eight-second input snapshot");
    return ESP_OK;
}
#endif
