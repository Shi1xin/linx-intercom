#include "audio_proc.h"

/* First-order DC-blocking high-pass: y[n] = x[n] - x[n-1] + R*y[n-1].
 * AEC TODO: wire esp-sr AFE (mic + far-end reference) here when echo becomes
 * audible; the playback side should export the last-rendered frame as
 * reference. */
void audio_proc_init(void) { }

void audio_proc_mic(int16_t *pcm, int samples)
{
    static int32_t x_prev, y_prev;
    for (int i = 0; i < samples; i++) {
        int32_t x = pcm[i];
        int32_t y = x - x_prev + (y_prev * 32505 >> 15); /* R = 0.992 */
        x_prev = x;
        y_prev = y;
        if (y > INT16_MAX) y = INT16_MAX;
        if (y < -INT16_MAX) y = -INT16_MAX;
        pcm[i] = (int16_t)y;
    }
}
