/* Linx platform OTA gateway (docs/xrobot/platform/OTA).
 * POSTs device identity, returns websocket url + device token once bound,
 * or an activation code while unbound. */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool bound;                    /* websocket.token present */
    char ws_url[160];              /* websocket.url */
    char token[280];               /* websocket.token (JWT) */
    char activation_code[12];      /* activation.code when unbound */
    char firmware_version[24];     /* firmware.version when offered */
    char firmware_url[256];        /* firmware.url when offered */
} linx_ota_result_t;

/* Blocking HTTP POST (~1-3 s). Requires Wi-Fi + SNTP-independent TLS bundle. */
esp_err_t linx_ota_fetch(const char *ota_url, const char *device_id,
                         const char *client_id, const char *board_type,
                         linx_ota_result_t *out);
