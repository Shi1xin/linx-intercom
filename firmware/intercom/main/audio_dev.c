#include "audio_dev.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "board.h"
#include "audio_codec.h"

#define TAG "audio_dev"

#define RX_QUEUE_LEN     10          /* ~200 ms of jitter headroom */
#define RX_PKT_MAX       512         /* max encoded opus packet */
#define OUT_FRAME_MS     20
#define OUT_SAMPLES      (AUDIO_OUTPUT_SAMPLE_RATE * OUT_FRAME_MS / 1000)  /* 480 @24k */
#define DMA_DESC_NUM     6
#define DMA_FRAME_NUM    240
#define MIC_DMA_DESC_NUM 12   /* ~165 ms of queued RX headroom at 16 kHz */
#define MIC_FRAME_SAMPLES (AUDIO_INPUT_SAMPLE_RATE * 20 / 1000)
#define MIC_QUEUE_LEN     16
#define MIC_TASK_STACK    4096
#define MIC_TASK_PRIORITY 19  /* Preempt pc_task's DTLS work (priority 18). */
#define MIC_TASK_CORE     ((portNUM_PROCESSORS > 1) ? 1 : 0)

typedef struct {
    int count;
    int16_t pcm[MIC_FRAME_SAMPLES];
} mic_frame_t;

typedef struct {
    int      len;
    int      src;   /* AUDIO_SRC_LINX / AUDIO_SRC_ROOM */
    uint8_t  data[RX_PKT_MAX];
} rx_pkt_t;

static int s_rx_src = AUDIO_SRC_LINX;
static uint32_t s_rx_accepted[2];   /* admitted packets per source (LINX/ROOM) */
static volatile bool s_dec_reset_pending;   /* consumed by playback_task */

/* tone frames carry src==AUDIO_SRC_TONE and data[0..1]=freq*10, [2..3]=ms.
 * The playback task synthesizes them directly at the output rate, so tones
 * never touch the opus decoder and play regardless of the arbiter owner. */

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static portMUX_TYPE s_mic_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_mic_rx_overflows;
static uint32_t s_mic_dropped_samples;
static QueueHandle_t s_mic_queue;
static TaskHandle_t s_mic_task;
static mic_frame_t s_mic_pending;
static size_t s_mic_pending_offset;
static QueueHandle_t     s_rx_queue;
static TaskHandle_t      s_play_task;
static volatile int      s_volume = 70;
static int32_t          *s_out32;     /* OUT_SAMPLES int32 for I2S tx */
static int16_t          *s_pcm24;     /* resampled 24k pcm */

/* ---------- I2S ---------- */

static bool IRAM_ATTR on_mic_rx_overflow(i2s_chan_handle_t handle,
                                       i2s_event_data_t *event, void *ctx)
{
    (void)handle;
    (void)event;
    (void)ctx;
    portENTER_CRITICAL_ISR(&s_mic_stats_lock);
    ++s_mic_rx_overflows;
    portEXIT_CRITICAL_ISR(&s_mic_stats_lock);
    return false;
}

uint32_t audio_dev_mic_rx_overflows(void)
{
    portENTER_CRITICAL(&s_mic_stats_lock);
    uint32_t count = s_mic_rx_overflows;
    portEXIT_CRITICAL(&s_mic_stats_lock);
    return count;
}

static void mic_record_drop(uint32_t samples)
{
    portENTER_CRITICAL(&s_mic_stats_lock);
    s_mic_dropped_samples += samples;
    portEXIT_CRITICAL(&s_mic_stats_lock);
}

uint32_t audio_dev_mic_dropped_samples(void)
{
    portENTER_CRITICAL(&s_mic_stats_lock);
    uint32_t count = s_mic_dropped_samples;
    portEXIT_CRITICAL(&s_mic_stats_lock);
    return count;
}

uint32_t audio_dev_mic_stack_hwm(void)
{
    return s_mic_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_mic_task) : 0;
}

static esp_err_t i2s_tx_init(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = DMA_DESC_NUM,
        .dma_frame_num = DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_OUTPUT_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_SPK_GPIO_BCLK,
            .ws   = AUDIO_I2S_SPK_GPIO_LRCK,
            .dout = AUDIO_I2S_SPK_GPIO_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    return i2s_channel_enable(s_tx);
}

static esp_err_t i2s_rx_init(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_1,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = MIC_DMA_DESC_NUM,
        .dma_frame_num = DMA_FRAME_NUM,
        .auto_clear_after_cb = false,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_INPUT_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_MIC_GPIO_SCK,
            .ws   = AUDIO_I2S_MIC_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = AUDIO_I2S_MIC_GPIO_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    /* Select the INMP441's left slot; the S3 Philips default enables both. */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_LOGI(TAG, "mic i2s rx rate=%d slot=left",
             (int)std_cfg.clk_cfg.sample_rate_hz);
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    const i2s_event_callbacks_t callbacks = {
        .on_recv_q_ovf = on_mic_rx_overflow,
    };
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_rx, &callbacks, NULL));
    return i2s_channel_enable(s_rx);
}

/* Physical mic path: a dedicated acquisition task reads I2S into a bounded
 * PCM FIFO; audio_dev_mic_read() is the single consumer. */

static int mic_read_i2s(int16_t *out, int samples)
{
    static int32_t raw[MIC_FRAME_SAMPLES]; /* 320 */
    size_t bytes_read = 0;
    if (samples > (int)(sizeof(raw) / sizeof(raw[0]))) {
        samples = sizeof(raw) / sizeof(raw[0]);
    }
    if (i2s_channel_read(s_rx, raw, samples * sizeof(int32_t), &bytes_read,
                         portMAX_DELAY) != ESP_OK) {
        return -1;
    }
    int got = bytes_read / sizeof(int32_t);
    for (int i = 0; i < got; i++) {
        int32_t v = raw[i] >> 12;
        out[i] = v > INT16_MAX ? INT16_MAX : (v < -INT16_MAX ? -INT16_MAX : (int16_t)v);
    }
    return got;
}

static void mic_enqueue_frame(const mic_frame_t *frame)
{
    if (xQueueSend(s_mic_queue, frame, 0) == pdTRUE) {
        return;
    }
    mic_frame_t discard;
    if (xQueueReceive(s_mic_queue, &discard, 0) == pdTRUE) {
        mic_record_drop((uint32_t)discard.count);
    }
    if (xQueueSend(s_mic_queue, frame, 0) != pdTRUE) {
        mic_record_drop((uint32_t)frame->count);
    }
}

static void mic_capture_task(void *arg)
{
    (void)arg;
    mic_frame_t frame;
    for (;;) {
        int n = mic_read_i2s(frame.pcm, MIC_FRAME_SAMPLES);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        frame.count = n;
        mic_enqueue_frame(&frame);
        vTaskDelay(1);
    }
}

int audio_dev_mic_read(int16_t *out, int samples)
{
    if (s_mic_queue == NULL || out == NULL || samples <= 0) {
        return -1;
    }
    if (s_mic_pending_offset >= (size_t)s_mic_pending.count) {
        if (xQueueReceive(s_mic_queue, &s_mic_pending, portMAX_DELAY) != pdTRUE) {
            return -1;
        }
        s_mic_pending_offset = 0;
        if (s_mic_pending.count <= 0 || s_mic_pending.count > MIC_FRAME_SAMPLES) {
            s_mic_pending_offset = 0;
            s_mic_pending.count = 0;
            return -1;
        }
    }
    int remaining = s_mic_pending.count - (int)s_mic_pending_offset;
    int take = samples < remaining ? samples : remaining;
    memcpy(out, s_mic_pending.pcm + s_mic_pending_offset,
           take * sizeof(int16_t));
    s_mic_pending_offset += take;
    return take;
}

/* ---------- downlink playback ---------- */

void audio_rx_set_source(int src)
{
    if (src != s_rx_src) {
        s_rx_src = src;
        ESP_LOGI(TAG, "audio rx source=%d", src);
        audio_rx_flush();          /* drop the other source's stale frames */
        s_dec_reset_pending = true; /* decoder reset runs on the playback task:
                                     * resetting here would race an in-flight
                                     * audio_opus_decode() on the other core */
    }
}

int audio_rx_current_source(void) { return s_rx_src; }

void audio_rx_mark_decoder_reset(void) { s_dec_reset_pending = true; }

static void audio_rx_enqueue_tagged(int src, const uint8_t *data, int size)
{
    if (s_rx_queue == NULL || size <= 0 || size > RX_PKT_MAX) {
        return;
    }
    rx_pkt_t pkt = { .len = size, .src = src };
    memcpy(pkt.data, data, size);
    if (xQueueSend(s_rx_queue, &pkt, 0) != pdTRUE) {
        rx_pkt_t drop;
        xQueueReceive(s_rx_queue, &drop, 0);
        xQueueSend(s_rx_queue, &pkt, 0);
    }
}

void audio_rx_enqueue_src(int src, const uint8_t *data, int size)
{
    if (src != s_rx_src && src != AUDIO_SRC_TONE) {
        return;   /* arbitration: only the active source feeds the decoder */
    }
    if ((src == AUDIO_SRC_LINX || src == AUDIO_SRC_ROOM) &&
        size > 0 && size <= RX_PKT_MAX) {
        if (++s_rx_accepted[src] % 250 == 0) {
            ESP_LOGI(TAG, "audio rx accepted src=%d n=%lu",
                     src, (unsigned long)s_rx_accepted[src]);
        }
    }
    audio_rx_enqueue_tagged(src, data, size);
}

void audio_rx_play_tone(int freq_hz, int ms)
{
    uint8_t hdr[4] = {
        (uint8_t)((freq_hz * 10) & 0xff), (uint8_t)((freq_hz * 10) >> 8),
        (uint8_t)(ms & 0xff), (uint8_t)(ms >> 8),
    };
    audio_rx_enqueue_tagged(AUDIO_SRC_TONE, hdr, sizeof(hdr));
}

void audio_rx_enqueue(const uint8_t *data, int size)
{
    if (s_rx_queue == NULL || size <= 0 || size > RX_PKT_MAX) {
        return;
    }
    rx_pkt_t pkt = { .len = size, .src = s_rx_src };
    memcpy(pkt.data, data, size);
    if (xQueueSend(s_rx_queue, &pkt, 0) != pdTRUE) {
        /* queue full: drop oldest then push newest to bound latency */
        rx_pkt_t drop;
        xQueueReceive(s_rx_queue, &drop, 0);
        xQueueSend(s_rx_queue, &pkt, 0);
    }
}

void audio_rx_flush(void)
{
    if (s_rx_queue) {
        xQueueReset(s_rx_queue);
    }
}

static void playback_write(const int16_t *pcm, int samples)
{
    int64_t vol_factor = (int64_t)s_volume * s_volume * 65536 / 10000;
    for (int i = 0; i < samples; i++) {
        int64_t t = (int64_t)pcm[i] * vol_factor;
        s_out32[i] = t > INT32_MAX ? INT32_MAX : (t < INT32_MIN ? INT32_MIN : (int32_t)t);
    }
    size_t written = 0;
    i2s_channel_write(s_tx, s_out32, samples * sizeof(int32_t), &written, portMAX_DELAY);
}

static void playback_task(void *arg)
{
    rx_pkt_t pkt;
    int16_t pcm48[4096];
    ESP_LOGI(TAG, "playback task up");
    for (;;) {
        if (xQueueReceive(s_rx_queue, &pkt, pdMS_TO_TICKS(100)) != pdTRUE) {
            /* underflow: keep DMA fed with silence */
            memset(s_pcm24, 0, OUT_SAMPLES * sizeof(int16_t));
            playback_write(s_pcm24, OUT_SAMPLES);
            continue;
        }
        if (pkt.src == AUDIO_SRC_TONE) {
            int freq = (pkt.data[0] | (pkt.data[1] << 8)) / 10;
            int ms   = (pkt.data[2] | (pkt.data[3] << 8));
            int total = AUDIO_OUTPUT_SAMPLE_RATE * ms / 1000;
            if (freq <= 0 || total <= 0) {
                continue;
            }
            for (int done = 0; done < total;) {
                int n = total - done;
                if (n > OUT_SAMPLES) n = OUT_SAMPLES;
                for (int i = 0; i < n; i++) {
                    int idx = done + i;
                    float env = 1.0f - (float)idx / total;
                    s_pcm24[i] = (int16_t)(sinf(2.0f * 3.1415926f * freq * idx /
                                              AUDIO_OUTPUT_SAMPLE_RATE) * 12000 * env);
                }
                playback_write(s_pcm24, n);
                done += n;
            }
            continue;
        }
        if (pkt.src != s_rx_src) {
            continue;   /* queued before a source switch; decoder never sees it */
        }
        if (s_dec_reset_pending) {   /* same-thread reset: safe vs decode */
            s_dec_reset_pending = false;
            audio_decoder_reset();
        }
        int dec_samples = audio_opus_decode(pkt.data, pkt.len, pcm48, sizeof(pcm48) / sizeof(pcm48[0]));
        if (dec_samples <= 0) {
            memset(s_pcm24, 0, OUT_SAMPLES * sizeof(int16_t));
            playback_write(s_pcm24, OUT_SAMPLES);
            continue;
        }
        uint32_t out_samples = OUT_SAMPLES;
        audio_downsample_48k_to_out(pcm48, dec_samples, s_pcm24, &out_samples);
        playback_write(s_pcm24, out_samples);
    }
}

void audio_dev_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
}

int audio_dev_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_dev_init(void)
{
    ESP_ERROR_CHECK(i2s_tx_init());
    ESP_ERROR_CHECK(i2s_rx_init());
    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_pkt_t));
    s_out32 = heap_caps_malloc(OUT_SAMPLES * sizeof(int32_t), MALLOC_CAP_8BIT);
    s_pcm24 = heap_caps_malloc(OUT_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
    s_mic_queue = xQueueCreateWithCaps(MIC_QUEUE_LEN, sizeof(mic_frame_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_rx_queue == NULL || s_out32 == NULL || s_pcm24 == NULL ||
        s_mic_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    const UBaseType_t stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    const UBaseType_t stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(playback_task, "aud_play",
                                                    30 * 1024, NULL, 10,
                                                    &s_play_task, 1, stack_caps);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ok = xTaskCreatePinnedToCoreWithCaps(mic_capture_task, "aud_mic",
                                         MIC_TASK_STACK, NULL, MIC_TASK_PRIORITY,
                                         &s_mic_task, MIC_TASK_CORE,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
