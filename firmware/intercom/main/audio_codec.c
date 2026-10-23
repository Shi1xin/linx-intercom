#include "audio_codec.h"
#include <string.h>
#include "esp_log.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_audio_enc.h"
#include "esp_audio_dec.h"
#include "esp_audio_types.h"
#include "esp_ae_rate_cvt.h"
#include "esp_ae_types.h"
#include "board.h"

#define TAG "audio_codec"

static void *s_enc;
static void *s_dec;
static esp_ae_rate_cvt_handle_t s_cvt;
static int   s_enc_in_size;    /* bytes of PCM per frame */
static int   s_enc_out_size;   /* recommended out bytes per frame */

esp_err_t audio_codec_init(void)
{
    /* Opus encoder: 16 kHz mono VOIP, 20 ms frames.
     * Wire format stays standard WebRTC opus/48000/2: opus packets are
     * self-describing for bandwidth, the far end decodes to its own rate. */
    esp_opus_enc_config_t enc_cfg = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = 32000,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity = 5,
        .enable_fec = true,
        .enable_dtx = false,
        .enable_vbr = true,
    };
    if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &s_enc) != ESP_AUDIO_ERR_OK || s_enc == NULL) {
        ESP_LOGE(TAG, "opus enc open failed");
        return ESP_FAIL;
    }
    esp_opus_enc_get_frame_size(s_enc, &s_enc_in_size, &s_enc_out_size);
    ESP_LOGI(TAG, "opus enc frame in=%dB out=%dB", s_enc_in_size, s_enc_out_size);

    /* Opus decoder: 48 kHz mono (wire rate) */
    esp_opus_dec_cfg_t dec_cfg = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_48K,
        .channel = ESP_AUDIO_MONO,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID, /* count as 60ms max */
        .self_delimited = false,
    };
    if (esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &s_dec) != ESP_AUDIO_ERR_OK || s_dec == NULL) {
        ESP_LOGE(TAG, "opus dec open failed");
        return ESP_FAIL;
    }

    /* 48k -> 24k downlink resampler */
    esp_ae_rate_cvt_cfg_t cvt_cfg = {
        .src_rate = ESP_AUDIO_SAMPLE_RATE_48K,
        .dest_rate = AUDIO_OUTPUT_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    if (esp_ae_rate_cvt_open(&cvt_cfg, &s_cvt) != ESP_AE_ERR_OK || s_cvt == NULL) {
        ESP_LOGE(TAG, "rate_cvt open failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

int audio_opus_encode(const int16_t *pcm, int samples, uint8_t *out_buf, int out_cap)
{
    if (s_enc == NULL) {
        return -1;
    }
    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)(samples * sizeof(int16_t)),
    };
    esp_audio_enc_out_frame_t out = {
        .buffer = out_buf,
        .len = (uint32_t)out_cap,
        .encoded_bytes = 0,
    };
    if (esp_opus_enc_process(s_enc, &in, &out) != ESP_AUDIO_ERR_OK) {
        return -1;
    }
    return (int)out.encoded_bytes;
}

int audio_opus_decode(const uint8_t *pkt, int len, int16_t *pcm_out, int cap_samples)
{
    if (s_dec == NULL) {
        return -1;
    }
    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)pkt,
        .len = (uint32_t)len,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t frame = {
        .buffer = (uint8_t *)pcm_out,
        .len = (uint32_t)(cap_samples * sizeof(int16_t)),
        .needed_size = 0,
        .decoded_size = 0,
    };
    esp_audio_dec_info_t info = {0};
    if (esp_opus_dec_decode(s_dec, &raw, &frame, &info) != ESP_AUDIO_ERR_OK) {
        return -1;
    }
    return (int)(frame.decoded_size / sizeof(int16_t));
}

void audio_downsample_48k_to_out(const int16_t *in, uint32_t in_samples,
                                 int16_t *out, uint32_t *out_samples)
{
    if (s_cvt == NULL || in == NULL || out == NULL || out_samples == NULL) {
        if (out_samples) *out_samples = 0;
        return;
    }
    esp_ae_sample_t in_s = (esp_ae_sample_t)in;
    esp_ae_sample_t out_s = (esp_ae_sample_t)out;
    uint32_t n = *out_samples;
    if (esp_ae_rate_cvt_process(s_cvt, in_s, in_samples, out_s, &n) != ESP_AE_ERR_OK) {
        *out_samples = 0;
        return;
    }
    *out_samples = n;
}

void audio_decoder_reset(void)
{
    if (s_dec) {
        esp_opus_dec_reset(s_dec);
    }
    if (s_cvt) {
        esp_ae_rate_cvt_reset(s_cvt);
    }
}
