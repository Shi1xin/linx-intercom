/* Single-producer announcement clip scheduler.
 *
 * Producer (app task) posts frame sequences through a seqlock mailbox;
 * the playback task owns the play cursor and its own frame buffer, so a
 * flush or re-arm can never touch memory a decode is using. Latest post
 * wins: a newer state replaces any queued or playing cue.
 */
#pragma once

#include <stdbool.h>
#include "audio_dev.h"   /* opus_frame_t */

#define CLIP_FRAMES_CAP 384

/* post a sequence; returns false only for invalid input (NULL, 0, >CAP). */
bool clip_sched_post(const opus_frame_t *frames, int nframes);

/* drop the playing cue and any pending post. */
void clip_sched_flush(void);

/* playback-task cursor: next frame to decode, or NULL when idle. */
const opus_frame_t *clip_sched_next(void);

/* generation of the sequence the cursor is on; changes at every take, so
 * the playback task can spot a mid-play switch and reset the decoder. */
uint32_t clip_sched_cur_gen(void);

/* frames in the most recently finished sequence (for logging). */
int clip_sched_done_frames(void);
