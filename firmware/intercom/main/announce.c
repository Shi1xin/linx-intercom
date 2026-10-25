/* Local fixed-vocabulary announcements. See announce.h. */
#include "announce.h"
#include <string.h>
#include "esp_log.h"
#include "audio_dev.h"
#include "announce_clips.h"

#define TAG "announce"
#define SEQ_CAP 448          /* chime + entry + room + invite + 6 digits + gaps */
#define HAO_FANG "\xE5\x8F\xB7\xE6\x88\xBF"  /* 号房 */

static opus_frame_t s_seq[SEQ_CAP];

/* numeral -> clip table; ASCII digits share the spoken digit clips */
static const struct {
    char        u8[4];      /* utf-8 char, nul-terminated */
    const opus_frame_t *clip;
    int         frames;
} k_num[] = {
    {"\xE9\x9B\xB6", clip_d0, CLIP_D0_FRAMES},  /* 零 */
    {"\xE4\xB8\x80", clip_d1, CLIP_D1_FRAMES},  /* 一 */
    {"\xE4\xBA\x8C", clip_d2, CLIP_D2_FRAMES},  /* 二 */
    {"\xE4\xB8\x89", clip_d3, CLIP_D3_FRAMES},  /* 三 */
    {"\xE5\x9B\x9B", clip_d4, CLIP_D4_FRAMES},  /* 四 */
    {"\xE4\xBA\x94", clip_d5, CLIP_D5_FRAMES},  /* 五 */
    {"\xE5\x85\xAD", clip_d6, CLIP_D6_FRAMES},  /* 六 */
    {"\xE4\xB8\x83", clip_d7, CLIP_D7_FRAMES},  /* 七 */
    {"\xE5\x85\xAB", clip_d8, CLIP_D8_FRAMES},  /* 八 */
    {"\xE4\xB9\x9D", clip_d9, CLIP_D9_FRAMES},  /* 九 */
    {"\xE5\x8D\x81", clip_d10, CLIP_D10_FRAMES},/* 十 */
};

/* match one char at s (rem bytes left); return numeral table index or -1.
 * Advances *s by the char width. */
static int numeral_at(const char **s, int rem)
{
    const char *p = *s;
    if (rem <= 0) {
        return -1;
    }
    if (*p >= '0' && *p <= '9') {
        *s = p + 1;
        return *p - '0';
    }
    for (int i = 0; i < (int)(sizeof(k_num) / sizeof(k_num[0])); i++) {
        int w = strlen(k_num[i].u8);
        if (rem >= w && memcmp(p, k_num[i].u8, w) == 0) {
            *s = p + w;
            return i;
        }
    }
    return -1;
}

static int append_clip(int n, const opus_frame_t *clip, int frames)
{
    if (n + frames > SEQ_CAP) {
        return n;
    }
    memcpy(s_seq + n, clip, frames * sizeof(opus_frame_t));
    return n + frames;
}

static int append_digit(int n, int d)
{
    return append_clip(n, k_num[d].clip, k_num[d].frames);
}

/* whole-sentence entry clips for "<one numeral>号房" (一..十) */
static const struct {
    const opus_frame_t *clip;
    int                 frames;
} k_entry[] = {
    {clip_entry1,  CLIP_ENTRY1_FRAMES},   /* 一 */
    {clip_entry2,  CLIP_ENTRY2_FRAMES},   /* 二 */
    {clip_entry3,  CLIP_ENTRY3_FRAMES},   /* 三 */
    {clip_entry4,  CLIP_ENTRY4_FRAMES},   /* 四 */
    {clip_entry5,  CLIP_ENTRY5_FRAMES},   /* 五 */
    {clip_entry6,  CLIP_ENTRY6_FRAMES},   /* 六 */
    {clip_entry7,  CLIP_ENTRY7_FRAMES},   /* 七 */
    {clip_entry8,  CLIP_ENTRY8_FRAMES},   /* 八 */
    {clip_entry9,  CLIP_ENTRY9_FRAMES},   /* 九 */
    {clip_entry10, CLIP_ENTRY10_FRAMES},  /* 十 */
};

/* "<numerals>号房" -> numeral indices via out (cap entries); returns
 * numeral count or -1 when the name does not fully parse. */
static int parse_named_room(const char *name, int *out, int cap)
{
    int len = strlen(name);
    const int suffix = 6;                 /* sizeof("号房") - 1 */
    if (len <= suffix ||
        memcmp(name + len - suffix, HAO_FANG, suffix) != 0) {
        return -1;
    }
    int cnt = 0;
    const char *p = name;
    const char *end = name + len - suffix;
    while (p < end) {
        if (cnt >= cap) {
            return -1;
        }
        int d = numeral_at(&p, (int)(end - p));
        if (d < 0) {
            return -1;
        }
        out[cnt++] = d;
    }
    return cnt;
}

/* "<numerals>号房" -> per-numeral clips + 号房 splice (multi-numeral
 * fallback; single-numeral names use the whole-sentence entry clips). */
static int try_named_room(int n, const char *name)
{
    int idx[8];
    int cnt = parse_named_room(name, idx, 8);
    if (cnt <= 0) {
        return -1;
    }
    for (int i = 0; i < cnt; i++) {
        n = append_digit(n, idx[i]);
    }
    return append_clip(n, clip_hao_fang, CLIP_HAO_FANG_FRAMES);
}

void announce_room_entry(const char *room_name, const char *invite_code)
{
    int n = append_clip(0, clip_chime, CLIP_CHIME_FRAMES);
    int idx[8];
    int cnt = room_name ? parse_named_room(room_name, idx, 8) : -1;
    if (cnt == 1 && idx[0] >= 1 && idx[0] <= 10) {
        /* "欢迎进入X号房" rendered as one sentence — no mid-phrase splice */
        n = append_clip(n, k_entry[idx[0] - 1].clip,
                        k_entry[idx[0] - 1].frames);
    } else if (cnt > 0) {
        /* multi-numeral "十一号房"+ : per-digit splice fallback */
        n = append_clip(n, clip_entered, CLIP_ENTERED_FRAMES);
        n = append_clip(n, clip_gap, CLIP_GAP_FRAMES);
        n = try_named_room(n, room_name);
    } else {
        /* name not decomposable: whole "欢迎进入房间" sentence */
        n = append_clip(n, clip_welcome_room, CLIP_WELCOME_ROOM_FRAMES);
    }
    ESP_LOGI(TAG, "entry room=%s code=%s frames=%d",
             room_name ? room_name : "?", invite_code ? invite_code : "-", n);
    if (!audio_rx_play_frames(s_seq, n)) {
        ESP_LOGW(TAG, "announcer busy, entry cue dropped");
    }
}

static void announce_two(const opus_frame_t *phrase, int frames,
                         const char *what)
{
    int n = append_clip(0, clip_chime, CLIP_CHIME_FRAMES);
    n = append_clip(n, phrase, frames);
    if (!audio_rx_play_frames(s_seq, n)) {
        ESP_LOGW(TAG, "announcer busy, %s cue dropped", what);
    }
}

void announce_leave_done(void)
{
    ESP_LOGI(TAG, "leave announce");
    announce_two(clip_left, CLIP_LEFT_FRAMES, "leave");
}

void announce_op_failed(void)
{
    ESP_LOGI(TAG, "op failed announce");
    announce_two(clip_failed, CLIP_FAILED_FRAMES, "fail");
}

void announce_offline(void)
{
    ESP_LOGI(TAG, "offline announce");
    announce_two(clip_offline, CLIP_OFFLINE_FRAMES, "offline");
}
