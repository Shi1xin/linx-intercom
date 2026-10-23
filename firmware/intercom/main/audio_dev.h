/* I2S audio device: INMP441 mic RX @16kHz, MAX98357 speaker TX @24kHz,
 * downlink playback queue + render task. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t audio_dev_init(void);

/* Read one 20 ms mic frame (returns 16-bit mono samples at 16 kHz).
 * Blocks until `samples` are captured. Returns sample count or <0 on error. */
int  audio_dev_mic_read(int16_t *out, int samples);

/* Feed an encoded downlink frame (called from peer on_audio_data ctx).
 * Copies data; drops when queue is full (jitter). */
void audio_rx_enqueue(const uint8_t *data, int size);

/* downlink arbitration: exactly one source feeds the decoder at a time.
 * AUDIO_SRC_LINX = platform TTS (16k opus), AUDIO_SRC_ROOM = Janus (48k opus) */
#define AUDIO_SRC_LINX 0
#define AUDIO_SRC_ROOM 1
void audio_rx_set_source(int src);
void audio_rx_enqueue_src(int src, const uint8_t *data, int size);
void audio_decoder_reset(void);

/* Drop all queued downlink frames (used on room switch with need_media=false
 * and on peer teardown). */
void audio_rx_flush(void);

/* Volume 0-100 */
void audio_dev_set_volume(int vol);
int  audio_dev_get_volume(void);
