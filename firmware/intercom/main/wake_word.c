#include "wake_word.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "app_events.h"
#include "sdkconfig.h"

#define WAKE_FEED_MS   20
#define WAKE_FRAME     (16000 * WAKE_FEED_MS / 1000)   /* 320 samples */
#define WAKE_QUEUE_LEN 10

static const char *TAG = "wake";

static const esp_wn_iface_t *s_wn;
static model_iface_data_t   *s_model;
static srmodel_list_t       *s_models;
static QueueHandle_t         s_q;
static volatile bool         s_enabled = true;
static volatile bool         s_mic_hold;         /* injection owns the queue */
static int                   s_det_count;
static int                   s_drop_count;

static void wake_task(void *arg)
{
    int chunk = s_wn->get_samp_chunksize(s_model);
    int16_t *buf = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *frame = malloc(WAKE_FRAME * sizeof(int16_t));
    int fill = 0;
    ESP_LOGI(TAG, "wakenet task up: chunk=%d", chunk);
    for (;;) {
        if (xQueueReceive(s_q, frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const int16_t *p = frame;
        int n = WAKE_FRAME;
        while (n > 0) {
            if (!s_enabled) {
                fill = 0;
                break;
            }
            int take = chunk - fill;
            if (take > n) take = n;
            memcpy(buf + fill, p, take * sizeof(int16_t));
            fill += take;
            p += take;
            n -= take;
            if (fill == chunk) {
                fill = 0;
                int r = s_wn->detect(s_model, buf);
                if (++s_det_count % 100 == 0) {
                    ESP_LOGI(TAG, "wake alive det=%d drop=%d en=%d",
                             s_det_count, s_drop_count, (int)s_enabled);
                }
                if (r > 0) {
                    ESP_LOGI(TAG, "wake word detected (idx %d)", r);
                    s_enabled = false;      /* app re-arms after the round */
                    app_post(APP_EV_WAKE, NULL, NULL, 0, false);
                }
            }
        }
    }
}

#if CONFIG_INTERCOM_WAKE_SELFTEST
#include "inj_pcm.h"
/* one-shot digital injection of a clean utterance through the same queue +
 * chunk path the mic uses — isolates model/pipeline from room acoustics */
static void inj_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(8000));
    s_mic_hold = true;               /* stop mic frames from interleaving */
    vTaskDelay(pdMS_TO_TICKS(100));  /* let the queue drain stale mic audio */
    xQueueReset(s_q);
    ESP_LOGI(TAG, "inject %d samples", inj_pcm_n);
    for (int i = 0; i + WAKE_FRAME <= inj_pcm_n; i += WAKE_FRAME) {
        while (xQueueSend(s_q, inj_pcm + i, pdMS_TO_TICKS(100)) != pdTRUE) {}
        vTaskDelay(pdMS_TO_TICKS(WAKE_FEED_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(300));  /* tail chunks still in flight */
    s_mic_hold = false;
    ESP_LOGI(TAG, "inject done");
    vTaskDelete(NULL);
}
#endif

esp_err_t wake_word_init(void)
{
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "model partition missing");
        return ESP_FAIL;
    }
    char *name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "nihaoxiaozhi");
    if (!name) {
        ESP_LOGE(TAG, "nihaoxiaozhi model not packed");
        return ESP_FAIL;
    }
    s_wn = esp_wn_handle_from_name(name);
    if (!s_wn) {
        ESP_LOGE(TAG, "no wakenet iface for %s", name);
        return ESP_FAIL;
    }
    s_model = s_wn->create(name, DET_MODE_95);   /* model in PSRAM: keeps detect
                                                  * off the flash bus so core-0
                                                  * audio code is not stalled */
    if (!s_model) {
        ESP_LOGE(TAG, "wakenet create failed");
        return ESP_FAIL;
    }
    s_q = xQueueCreate(WAKE_QUEUE_LEN, WAKE_FRAME * sizeof(int16_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(wake_task, "wake", 6 * 1024, NULL,
                                4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_INTERCOM_WAKE_SELFTEST
    xTaskCreate(inj_task, "wake_inj", 3 * 1024, NULL, 3, NULL);
#endif
    ESP_LOGI(TAG, "wakenet up: model=%s", name);
    return ESP_OK;
}

void wake_word_feed(const int16_t *pcm, int n)
{
    if (s_mic_hold || !s_q || n != WAKE_FRAME) {
        return;
    }
    if (xQueueSend(s_q, pcm, 0) != pdTRUE) {
        s_drop_count++;          /* detect task behind: frame dropped */
    }
}

void wake_word_enable(bool on)
{
    s_enabled = on;
}
