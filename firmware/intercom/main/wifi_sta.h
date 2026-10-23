/* Wi-Fi STA connect + reconnect, exposes lowercase colon MAC device id */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Start Wi-Fi STA with Kconfig ssid/password.
 * Posts APP_EV_WIFI_CONNECTED / APP_EV_WIFI_DISCONNECTED to the app queue. */
esp_err_t   wifi_sta_start(void);

/* Device id: sta MAC, lowercase colon form, e.g. "98:a3:16:cd:05:ac" */
const char *wifi_sta_device_id(void);

bool        wifi_sta_is_connected(void);
