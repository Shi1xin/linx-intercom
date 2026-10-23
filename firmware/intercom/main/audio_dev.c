#include "audio_dev.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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

typedef struct {
    int      len;
    int      src;   /* AUDIO_SRC_LINX / AUDIO_SRC_ROOM */
    uint8_t  data[RX_PKT_MAX];
} rx_pkt_t;

static int s_rx_src = AUDIO_SRC_LINX;

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static QueueHandle_t     s_rx_queue;
static TaskHandle_t      s_play_task;
static volatile int      s_volume = 70;
static int32_t          *s_out32;     /* OUT_SAMPLES int32 for I2S tx */
static int16_t          *s_pcm24;     /* resampled 24k pcm */

/* ---------- I2S ---------- */

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
        .dma_desc_num = DMA_DESC_NUM,
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
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    return i2s_channel_enable(s_rx);
}

int audio_dev_mic_read(int16_t *out, int samples)
{
    static int32_t raw[AUDIO_INPUT_SAMPLE_RATE * 20 / 1000]; /* 320 */
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

/* ---------- downlink playback ---------- */

void audio_rx_set_source(int src)
{
    if (src != s_rx_src) {
        s_rx_src = src;
        audio_rx_flush();          /* drop the other source's stale frames */
        audio_decoder_reset();     /* opus decoder state is per-stream */
    }
}

void audio_rx_enqueue_src(int src, const uint8_t *data, int size)
{
    if (src != s_rx_src) {
        return;   /* arbitration: only the active source feeds the decoder */
    }
    audio_rx_enqueue(data, size);
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
        if (pkt.src != s_rx_src) {
            continue;   /* queued before a source switch; decoder never sees it */
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
    if (s_rx_queue == NULL || s_out32 == NULL || s_pcm24 == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreatePinnedToCore(playback_task, "aud_play", 30 * 1024, NULL, 10, &s_play_task, 1);
    return s_play_task ? ESP_OK : ESP_FAIL;
}
