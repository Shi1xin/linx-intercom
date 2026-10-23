#include "cmd_capture.h"
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "board.h"

#define TAG "cmd_cap"

#define BUF_SAMPLES  (AUDIO_INPUT_SAMPLE_RATE * CMD_CAPTURE_SECONDS)
#define MIN_SAMPLES  (AUDIO_INPUT_SAMPLE_RATE * CMD_CAPTURE_MIN_MS / 1000)

static int16_t   *s_buf;      /* PSRAM */
static int        s_wr;       /* samples written */
static int        s_last_len; /* samples of last finished capture */
static bool       s_active;

esp_err_t cmd_capture_init(void)
{
    s_buf = heap_caps_malloc(BUF_SAMPLES * sizeof(int16_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        s_buf = malloc(BUF_SAMPLES * sizeof(int16_t));
    }
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "capture buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "capture buffer %d samples", BUF_SAMPLES);
    return ESP_OK;
}

void cmd_capture_begin(void)
{
    s_wr = 0;
    s_last_len = 0;
    s_active = true;
}

void cmd_capture_feed(const int16_t *pcm, int samples)
{
    if (!s_active || s_buf == NULL) {
        return;
    }
    int room = BUF_SAMPLES - s_wr;
    if (samples > room) {
        samples = room;
    }
    if (samples > 0) {
        memcpy(s_buf + s_wr, pcm, samples * sizeof(int16_t));
        s_wr += samples;
    }
}

int cmd_capture_finish(void)
{
    s_active = false;
    s_last_len = s_wr;
    s_wr = 0;
    if (s_last_len < MIN_SAMPLES) {
        s_last_len = 0;
    }
    return s_last_len;
}

const int16_t *cmd_capture_data(void)
{
    return s_buf;
}

int cmd_capture_samples(void)
{
    return s_last_len;
}

void cmd_capture_abort(void)
{
    s_active = false;
    s_wr = 0;
    s_last_len = 0;
}

bool cmd_recognize(const int16_t *pcm, int samples, cmd_result_t *out)
{
    /* Stub: any sufficiently long utterance maps to the configured target.
     * Real implementation hook: run keyword/intent model here, or stream the
     * PCM to a cloud recognizer, then fill out->kind/arg. */
    (void)pcm;
    if (out == NULL || samples <= 0) {
        return false;
    }
    strlcpy(out->kind, CONFIG_INTERCOM_CMD_TARGET_KIND, sizeof(out->kind));
    strlcpy(out->arg, CONFIG_INTERCOM_CMD_TARGET_ARG, sizeof(out->arg));
    if (out->arg[0] == 0) {
        return false;
    }
    ESP_LOGI(TAG, "stub recognize: %d samples -> join %s:%s",
             samples, out->kind, out->arg);
    return true;
}
