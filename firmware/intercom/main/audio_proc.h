/* Uplink mic processing: DC removal + gentle high-pass now; AEC hook point.
 * Drop-in for esp-sr AFE later: keep signature (mic pcm in-place). */
#pragma once

#include <stdint.h>

void audio_proc_init(void);
/* Process one mic frame in place (int16 mono @16kHz). */
void audio_proc_mic(int16_t *pcm, int samples);
