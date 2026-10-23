/* Command voice capture (PSRAM ring buffer) + recognition stub.
 * Flow: GPIO0 held -> cmd_capture_begin(); capture task feeds 20 ms frames;
 * released -> cmd_capture_finish() -> cmd_recognize() placeholder emits a join
 * target, which app_main turns into a ws join message. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define CMD_CAPTURE_SECONDS  5
#define CMD_CAPTURE_MIN_MS   300

esp_err_t cmd_capture_init(void);
void      cmd_capture_begin(void);
void      cmd_capture_feed(const int16_t *pcm, int samples);
/* Returns captured sample count (0 if below minimum). */
int       cmd_capture_finish(void);
/* Access captured PCM (valid until next begin). */
const int16_t *cmd_capture_data(void);
int       cmd_capture_samples(void);
void      cmd_capture_abort(void);

/* Recognition result → join target. */
typedef struct {
    char kind[16];   /* nfc | device | invite | room_name */
    char arg[64];
} cmd_result_t;

/* Placeholder recognizer: ignores audio, returns Kconfig-configured target.
 * Returns true when a command was "recognized". */
bool      cmd_recognize(const int16_t *pcm, int samples, cmd_result_t *out);
