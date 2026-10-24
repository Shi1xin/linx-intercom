/* See clip_sched.h. Pure C: the only ESP dependency is the opus_frame_t
 * type from audio_dev.h, so host tests compile this file directly. */
#include "clip_sched.h"
#include <string.h>
#include <stdint.h>

#define barrier() __sync_synchronize()

/* mailbox: producer writes n/frames between odd/even gen bumps (seqlock).
 * single writer keeps the odd window short; reader retries a torn take. */
static struct {
    volatile uint32_t gen;   /* odd = writer active, even = published */
    int n;
    opus_frame_t frames[CLIP_FRAMES_CAP];
} s_box;

/* consumer-private cursor and buffer */
static struct {
    int idx, n;
    uint32_t gen;
    opus_frame_t frames[CLIP_FRAMES_CAP];
} s_cur;
static uint32_t s_seen_gen;
static int s_done_frames;

bool clip_sched_post(const opus_frame_t *frames, int nframes)
{
    if (frames == NULL || nframes <= 0 || nframes > CLIP_FRAMES_CAP) {
        return false;
    }
    uint32_t g = s_box.gen + 1;
    s_box.gen = g;                 /* odd: writer active */
    barrier();
    memcpy(s_box.frames, frames, nframes * sizeof(opus_frame_t));
    s_box.n = nframes;
    barrier();
    s_box.gen = g + 1;             /* even: published */
    return true;
}

void clip_sched_flush(void)
{
    /* an empty post both retires pending messages and tells the consumer
     * to drop its cursor — ordering is preserved by the same seqlock. */
    uint32_t g = s_box.gen + 1;
    s_box.gen = g;
    barrier();
    s_box.n = 0;
    barrier();
    s_box.gen = g + 1;
}

const opus_frame_t *clip_sched_next(void)
{
    /* take a newly published message, retrying if a write landed mid-copy */
    for (int tries = 0; tries < 4; tries++) {
        uint32_t g = s_box.gen;
        if (g == s_seen_gen || (g & 1)) {
            break;                 /* nothing new, or writer active now */
        }
        barrier();
        int n = s_box.n;
        memcpy(s_cur.frames, s_box.frames, n * sizeof(opus_frame_t));
        barrier();
        if (s_box.gen != g) {
            continue;              /* torn take: writer moved on, retry */
        }
        s_seen_gen = g;
        s_done_frames = s_cur.n;   /* old sequence ended (done or cut) */
        s_cur.n = n;
        s_cur.idx = 0;
        s_cur.gen = g;
        break;
    }
    if (s_cur.idx >= s_cur.n) {
        s_done_frames = s_cur.n;
        return NULL;
    }
    return &s_cur.frames[s_cur.idx++];
}

uint32_t clip_sched_cur_gen(void)
{
    return s_cur.gen;
}

int clip_sched_done_frames(void)
{
    return s_done_frames;
}
