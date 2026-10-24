/* Bounded recovery for a platform TTS round whose stop event never arrives.
 * Pure function so host tests compile it directly. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* audio keeps the round alive: frames flow continuously while speech plays;
 * a >5s silence means the stream is gone. */
#define TTS_IDLE_US      (5 * 1000 * 1000)
/* before the first frame arrives the wider reply window still applies
 * (slow agent rounds legitimately take seconds to start audio). */
#define TTS_DEAD_US      (25 * 1000 * 1000)

/* true when a playing round should be force-closed:
 * frames began then stopped for TTS_IDLE_US, or the start produced no audio
 * within TTS_DEAD_US. `started` and `last_frame` are esp_timer usecs;
 * last_frame may predate started (older round) — then only the dead bound
 * applies. */
static inline bool tts_round_expired(int64_t now, int64_t started,
                                     int64_t last_frame)
{
    if (last_frame > started) {
        return now - last_frame > TTS_IDLE_US;
    }
    return now - started > TTS_DEAD_US;
}
