/* NVS persistence: muted flag + speaker volume */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t nvs_store_init(void);
bool      nvs_store_get_muted(void);
esp_err_t nvs_store_set_muted(bool muted);
int       nvs_store_get_volume(void);        /* 0-100, default 70 */
esp_err_t nvs_store_set_volume(int volume);
esp_err_t nvs_store_get_client_id(char *out, size_t n); /* uuid v4, persistent */
