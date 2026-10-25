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

/* "<numerals>号房" -> per-numeral clips + 号房; returns frames appended or -1
 * when the name does not fully parse. */
static int try_named_room(int n, const char *name)
{
    int len = strlen(name);
    const int suffix = 6;                 /* sizeof("号房") - 1 */
    if (len <= suffix ||
        memcmp(name + len - suffix, HAO_FANG, suffix) != 0) {
        return -1;
    }
    /* all chars before the suffix must be numerals */
    int idx[8], cnt = 0;
    const char *p = name;
    const char *end = name + len - suffix;
    while (p < end) {
        if (cnt >= (int)(sizeof(idx) / sizeof(idx[0]))) {
            return -1;
        }
        int d = numeral_at(&p, (int)(end - p));
        if (d < 0) {
            return -1;
        }
        idx[cnt++] = d;
    }
    for (int i = 0; i < cnt; i++) {
        n = append_digit(n, idx[i]);
    }
    return append_clip(n, clip_hao_fang, CLIP_HAO_FANG_FRAMES);
}

static int append_invite(int n, const char *code)
{
    if (code == NULL || strlen(code) != 6) {
        return n;
    }
    n = append_clip(n, clip_invite, CLIP_INVITE_FRAMES);
    n = append_clip(n, clip_gap, CLIP_GAP_FRAMES);
    for (const char *p = code; *p; p++) {
        if (*p < '0' || *p > '9') {
            return n;
        }
        n = append_digit(n, *p - '0');
        n = append_clip(n, clip_gap, CLIP_GAP_FRAMES);
    }
    return n;
}

void announce_room_entry(const char *room_name, const char *invite_code)
{
    int n = append_clip(0, clip_chime, CLIP_CHIME_FRAMES);
    n = append_clip(n, clip_entered, CLIP_ENTERED_FRAMES);
    n = append_clip(n, clip_gap, CLIP_GAP_FRAMES);
    int with_name = (room_name && room_name[0])
                    ? try_named_room(n, room_name) : -1;
    if (with_name > 0) {
        n = with_name;
    } else {
        /* name not decomposable: "欢迎进入房间" + invite digits as identity */
        n = append_clip(n, clip_room, CLIP_ROOM_FRAMES);
        n = append_invite(n, invite_code);
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
