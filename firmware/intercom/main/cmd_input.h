/* Buttons (GPIO0 command, GPIO47 touch, GPIO40/39 volume) + status LED */
#pragma once

#include "esp_err.h"

typedef enum {
    LED_OFF = 0,
    LED_ON,
    LED_BLINK_SLOW,    /* connecting / joining */
    LED_BLINK_FAST,    /* command capture */
} led_mode_t;

esp_err_t cmd_input_init(void);
void      led_set(led_mode_t mode);
