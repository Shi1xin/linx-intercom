/* I2S audio device: INMP441 mic RX @16kHz, MAX98357 speaker TX @24kHz,
 * downlink playback queue + render task. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t audio_dev_init(void);

/* Read mic samples from the bounded acquisition FIFO (16-bit mono, 16 kHz).
 * Blocks until at least one frame is available; may return a partial frame.
 * an over-size request yields at most one queued frame, a smaller request
 * leaves the tail for the next call. Single consumer (capture_task).
 * Returns sample count or <0 on error. */
int  audio_dev_mic_read(int16_t *out, int samples);

/* Cumulative RX DMA queue overflows since boot, sampled under the ISR lock. */
uint32_t audio_dev_mic_rx_overflows(void);

/* Samples discarded by the bounded PCM queue, cumulative since boot. */
uint32_t audio_dev_mic_dropped_samples(void);

/* Remaining acquisition-task stack high-water mark, in bytes. */
uint32_t audio_dev_mic_stack_hwm(void);

/* Feed an encoded downlink frame (called from peer on_audio_data ctx).
 * Copies data; drops when queue is full (jitter). */
void audio_rx_enqueue(const uint8_t *data, int size);

/* downlink arbitration: exactly one source feeds the decoder at a time.
 * AUDIO_SRC_LINX = platform TTS (16k opus), AUDIO_SRC_ROOM = Janus (48k opus) */
#define AUDIO_SRC_LINX 0
#define AUDIO_SRC_ROOM 1
#define AUDIO_SRC_TONE 2   /* local cue, synthesized in playback task */
void audio_rx_set_source(int src);
void audio_rx_enqueue_src(int src, const uint8_t *data, int size);
int  audio_rx_current_source(void);
void audio_rx_play_tone(int freq_hz, int ms);
void audio_decoder_reset(void);
void audio_rx_mark_decoder_reset(void);

/* Local spoken announcement: a caller-built sequence of raw 48 kHz Opus
 * frames (announce_clips.h). The playback task streams them back-to-back
 * ahead of the rx queue, so they play in every mode regardless of the
 * linx/room source gate. Single producer (app task) only.
 * Returns false when both clip slots are busy. */
typedef struct {
    const uint8_t *data;
    uint16_t len;
} opus_frame_t;
bool audio_rx_play_frames(const opus_frame_t *frames, int nframes);

/* Drop all queued downlink frames (used on room switch with need_media=false
 * and on peer teardown). */
void audio_rx_flush(void);

/* Volume 0-100 */
void audio_dev_set_volume(int vol);
int  audio_dev_get_volume(void);
