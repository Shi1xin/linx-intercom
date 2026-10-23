/* Board pin map: bread-compact-wifi (ESP32-S3 N16R8)
 * Same IO layout as linx-esp32/main/boards/bread-compact-wifi/config.h
 */
#pragma once

#include "driver/gpio.h"

#define AUDIO_INPUT_SAMPLE_RATE   16000   /* mic capture + opus encode rate */
#define AUDIO_OUTPUT_SAMPLE_RATE  24000   /* MAX98357 I2S playback rate */
#define AUDIO_WIRE_SAMPLE_RATE    48000   /* opus RTP clock rate in SDP */

/* Simplex I2S: INMP441 mic */
#define AUDIO_I2S_MIC_GPIO_WS     GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK    GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN    GPIO_NUM_6

/* Simplex I2S: MAX98357 speaker */
#define AUDIO_I2S_SPK_GPIO_DOUT   GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK   GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK   GPIO_NUM_16

#define CMD_BUTTON_GPIO           GPIO_NUM_0   /* BOOT key, command hold-to-talk */
#define TOUCH_BUTTON_GPIO         GPIO_NUM_47  /* touch key, mute toggle */
#define VOLUME_UP_BUTTON_GPIO     GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO   GPIO_NUM_39
#define BUILTIN_LED_GPIO          GPIO_NUM_48

/* OLED SSD1306 I2C (unused this phase) */
#define DISPLAY_SDA_PIN           GPIO_NUM_41
#define DISPLAY_SCL_PIN           GPIO_NUM_42
