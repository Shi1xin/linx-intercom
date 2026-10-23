#include "cmd_input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "board.h"
#include "app_events.h"

#define TAG "cmd_input"

#define POLL_MS       20
#define DEBOUNCE_CNT  3    /* 60 ms */

typedef struct {
    gpio_num_t   pin;
    int          stable;   /* stable level (1 = released for pullup buttons) */
    int          cnt;
    app_ev_type_t press_ev;
    app_ev_type_t release_ev;
} btn_t;

static btn_t s_btns[] = {
    { CMD_BUTTON_GPIO,        1, 0, APP_EV_CMD_PRESS, APP_EV_CMD_RELEASE },
    { TOUCH_BUTTON_GPIO,      1, 0, 0,                APP_EV_TOUCH_TAP   },
    { VOLUME_UP_BUTTON_GPIO,  1, 0, APP_EV_VOL_UP,    0                  },
    { VOLUME_DOWN_BUTTON_GPIO,1, 0, APP_EV_VOL_DOWN,  0                  },
};

static volatile led_mode_t s_led = LED_OFF;
static int s_led_phase;

static void poll_task(void *arg)
{
    for (;;) {
        for (int i = 0; i < sizeof(s_btns) / sizeof(s_btns[0]); i++) {
            btn_t *b = &s_btns[i];
            int lvl = gpio_get_level(b->pin);
            if (lvl == b->stable) {
                b->cnt = 0;
                continue;
            }
            if (++b->cnt >= DEBOUNCE_CNT) {
                b->cnt = 0;
                int prev = b->stable;
                b->stable = lvl;
                bool pressed = (lvl == 0);
                if (pressed && b->press_ev) {
                    app_post(b->press_ev, NULL, NULL, 0, false);
                } else if (!pressed && prev == 0 && b->release_ev) {
                    app_post(b->release_ev, NULL, NULL, 0, false);
                }
            }
        }
        /* LED pattern, 20 ms per phase tick */
        s_led_phase++;
        int on = 0;
        switch (s_led) {
        case LED_ON:         on = 1; break;
        case LED_BLINK_SLOW: on = (s_led_phase % 50) < 25; break;
        case LED_BLINK_FAST: on = (s_led_phase % 10) < 5;  break;
        default:             on = 0; break;
        }
        gpio_set_level(BUILTIN_LED_GPIO, on);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void led_set(led_mode_t mode)
{
    s_led = mode;
}

esp_err_t cmd_input_init(void)
{
    uint64_t mask = (1ULL << CMD_BUTTON_GPIO) | (1ULL << TOUCH_BUTTON_GPIO) |
                    (1ULL << VOLUME_UP_BUTTON_GPIO) | (1ULL << VOLUME_DOWN_BUTTON_GPIO);
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_config_t led = {
        .pin_bit_mask = 1ULL << BUILTIN_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));

    xTaskCreatePinnedToCore(poll_task, "btn_poll", 3072, NULL, 5, NULL, 0);
    return ESP_OK;
}
