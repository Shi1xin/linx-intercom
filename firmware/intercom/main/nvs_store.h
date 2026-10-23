/* NVS persistence: muted flag + speaker volume */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t nvs_store_init(void);
bool      nvs_store_get_muted(void);
esp_err_t nvs_store_set_muted(bool muted);
int       nvs_store_get_volume(void);        /* 0-100, default 70 */
esp_err_t nvs_store_set_volume(int volume);
