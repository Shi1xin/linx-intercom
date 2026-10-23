/* Opus codec + resample glue (esp_audio_codec / esp_audio_effects) */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t audio_codec_init(void);

/* Encode one 20 ms 16 kHz mono PCM frame (320 int16 samples) to opus.
 * out_buf must be >= 512B. Returns encoded byte count or <0. */
int  audio_opus_encode(const int16_t *pcm, int samples, uint8_t *out_buf, int out_cap);

/* Decode one opus packet to 48 kHz mono int16 PCM.
 * Returns decoded sample count or <0. */
int  audio_opus_decode(const uint8_t *pkt, int len, int16_t *pcm_out, int cap_samples);

/* Resample 48 kHz mono int16 -> AUDIO_OUTPUT_SAMPLE_RATE via esp_ae_rate_cvt.
 * in_samples: sample count; out_samples: in=capacity, out=actual. */
void audio_downsample_48k_to_out(const int16_t *in, uint32_t in_samples,
                                 int16_t *out, uint32_t *out_samples);

void audio_decoder_reset(void);
