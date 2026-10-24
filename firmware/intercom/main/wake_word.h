#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* WakeNet "你好小智" detection on the live mic stream.
 * capture_task feeds 16 kHz mono int16 frames directly into the AFE
 * front-end; a fetch task pulls detection results and posts APP_EV_WAKE on
 * a hit. Detection pauses while a listen round is open and is re-armed by
 * wake_word_enable(true). */

esp_err_t wake_word_init(void);
void      wake_word_feed(const int16_t *pcm, int n);
void      wake_word_enable(bool on);
