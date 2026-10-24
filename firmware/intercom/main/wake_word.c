/* ESP-SR WakeNet via AFE front-end, same architecture as xiaozhi's
 * AfeAudioEngine: mic frames feed AFE directly; a fetch task blocks on
 * fetch_with_delay and checks wakeup_state from the fetch result. */
#include "wake_word.h"

#include <string.h>
#include <stdatomic.h>

#include "app_events.h"
#include "wake_trace.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vad.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "wake"

static const esp_afe_sr_iface_t *s_afe_iface;
static esp_afe_sr_data_t *s_afe;
static srmodel_list_t *s_models;
static atomic_bool s_enabled;
static atomic_bool s_mic_hold;
static int16_t *s_feed_buf;
static int s_feed_len;
static int s_feed_chunk;
static int s_detect_count;

#if CONFIG_INTERCOM_WAKE_SELFTEST
#include "inj_pcm.h"
#endif

/* Feed mono 16 kHz samples; AFE consumes get_feed_chunksize() samples per
 * call, so accumulate mic frames until one full chunk is ready. */
static void feed_samples(const int16_t *pcm, int n)
{
    while (n > 0) {
        int room = s_feed_chunk - (s_feed_len % s_feed_chunk);
        int take = n < room ? n : room;
        memcpy(s_feed_buf + s_feed_len, pcm, take * sizeof(int16_t));
        s_feed_len += take;
        pcm += take;
        n -= take;
        if (s_feed_len % s_feed_chunk == 0) {
            for (int off = 0; off + s_feed_chunk <= s_feed_len; off += s_feed_chunk) {
                s_afe_iface->feed(s_afe, s_feed_buf + off);
                wake_trace_feed(s_feed_buf + off, s_feed_chunk);
            }
            s_feed_len = 0;
        }
    }
}

void wake_word_feed(const int16_t *pcm, int n)
{
    if (!s_afe || s_mic_hold || !s_enabled) {
        return;
    }
    feed_samples(pcm, n);
}

void wake_word_enable(bool en)
{
    if (!s_afe_iface || !s_afe) {
        return;
    }
    s_enabled = en;
    if (en) {
        s_afe_iface->enable_wakenet(s_afe);
    } else {
        s_afe_iface->disable_wakenet(s_afe);
    }
}

#if CONFIG_INTERCOM_WAKE_SELFTEST
static void inject_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));
    s_mic_hold = true;
    vTaskDelay(pdMS_TO_TICKS(200)); /* let in-flight mic frames flush */
    s_feed_len = 0;
    ESP_LOGI(TAG, "wake: inject %d samples", inj_pcm_n);
    for (int off = 0; off < inj_pcm_n; off += s_feed_chunk) {
        int n = inj_pcm_n - off < s_feed_chunk ? inj_pcm_n - off : s_feed_chunk;
        memset(s_feed_buf, 0, s_feed_chunk * sizeof(int16_t));
        memcpy(s_feed_buf, inj_pcm + off, n * sizeof(int16_t));
        s_afe_iface->feed(s_afe, s_feed_buf);
        vTaskDelay(pdMS_TO_TICKS(s_feed_chunk * 1000 / 16000));
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    s_feed_len = 0;           /* drain before unholding mic feeds */
    s_mic_hold = false;
    ESP_LOGI(TAG, "wake: inject done");
    vTaskDelete(NULL);
}
#endif

static void fetch_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "wake: fetch task up, feed_chunk=%d fetch_chunk=%d",
             s_feed_chunk, s_afe_iface->get_fetch_chunksize(s_afe));
    int last_log = 0;
    for (;;) {
        afe_fetch_result_t *res = s_afe_iface->fetch_with_delay(s_afe, portMAX_DELAY);
        if (!res || res->ret_value == ESP_FAIL) {
            continue;
        }
        s_detect_count++;
        if (s_detect_count - last_log >= 100) {
            last_log = s_detect_count;
            ESP_LOGI(TAG, "wake alive det=%d en=%d vol=%.1f", s_detect_count,
                     s_enabled ? 1 : 0, res->data_volume);
        }
        if (res->wakeup_state == WAKENET_DETECTED && res->wake_word_index > 0) {
            ESP_LOGI(TAG, "wake word detected (idx %d vol=%.1f)",
                     res->wake_word_index, res->data_volume);
            s_enabled = false;
            s_afe_iface->disable_wakenet(s_afe);
            app_post(APP_EV_WAKE, NULL, NULL, 0, false);
        }
    }
}

esp_err_t wake_word_init(void)
{
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "srmodel init failed (model partition missing?)");
        return ESP_FAIL;
    }
    char *name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "nihaoxiaozhi");
    if (!name) {
        ESP_LOGE(TAG, "no nihaoxiaozhi model in partition");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "wake model=%s", name);

    afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }
    cfg->aec_init = false;
    cfg->ns_init = false;
    cfg->vad_init = true;
    cfg->vad_mode = VAD_MODE_0;
    cfg->vad_min_noise_ms = 100;
    cfg->wakenet_init = true;
    cfg->wakenet_model_name = name;
    cfg->agc_init = false;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_afe_iface = esp_afe_handle_from_config(cfg);
    ESP_LOGI(TAG, "AFE config wakenet_mode=%d", (int)cfg->wakenet_mode);
    if (s_afe_iface) {
        s_afe = s_afe_iface->create_from_config(cfg);
    }
    afe_config_free(cfg);
    if (!s_afe_iface || !s_afe) {
        ESP_LOGE(TAG, "AFE create failed");
        return ESP_FAIL;
    }
    s_afe_iface->print_pipeline(s_afe);

    s_feed_chunk = s_afe_iface->get_feed_chunksize(s_afe);
    s_feed_buf = heap_caps_malloc(s_feed_chunk * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (!s_feed_buf) {
        return ESP_ERR_NO_MEM;
    }

    s_enabled = true;
    s_afe_iface->enable_wakenet(s_afe);
    esp_err_t trace_err = wake_trace_init();
    if (trace_err != ESP_OK) {
        ESP_LOGW(TAG, "PCM trace init failed: %s", esp_err_to_name(trace_err));
    }

    BaseType_t ok = xTaskCreatePinnedToCore(fetch_task, "wake_fetch", 6 * 1024,
                                            NULL, 5, NULL, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_INTERCOM_WAKE_SELFTEST
    xTaskCreate(inject_task, "wake_inj", 4 * 1024, NULL, 4, NULL);
#endif
    return ESP_OK;
}
