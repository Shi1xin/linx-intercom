#ifndef ANNOUNCE_H
#define ANNOUNCE_H

/* Local spoken announcements for state changes that carry no platform TTS
 * reply (cloud ops, NFC taps, summon, resume). Clips are fixed-vocabulary
 * 48 kHz Opus streamed by the playback task — see announce_clips.h and
 * tools/announce/gen_clips.py. */

/* "已进入<digits>号房" when the name parses, else "已进入房间，邀请码<d>" */
void announce_room_entry(const char *room_name, const char *invite_code);

/* "已退出对讲房间" */
void announce_leave_done(void);

/* "操作未完成" */
void announce_op_failed(void);

#endif /* ANNOUNCE_H */
