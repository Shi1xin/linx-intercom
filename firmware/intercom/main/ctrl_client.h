/* Control channel: WebSocket JSON client implementing docs/control-protocol.md */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Initialize client (does not connect). */
esp_err_t ctrl_client_init(void);

/* Connect to CONFIG_INTERCOM_CTRL_URL. Non-blocking; APP_EV_WS_CONNECTED /
 * APP_EV_WS_DISCONNECTED are posted on state change. */
esp_err_t ctrl_client_connect(void);
esp_err_t ctrl_client_disconnect(void);
bool      ctrl_client_is_connected(void);

/* Protocol messages. req ids are generated internally ("rN"). */
esp_err_t ctrl_send_hello(const char *device_id);
esp_err_t ctrl_send_join(const char *kind, const char *arg);
esp_err_t ctrl_send_sdp(const char *sdp);
esp_err_t ctrl_send_ice_candidate(const char *candidate);
esp_err_t ctrl_send_ice_completed(void);
esp_err_t ctrl_send_confirm(void);
esp_err_t ctrl_send_room_info(void);
esp_err_t ctrl_send_set_mute(bool muted);
esp_err_t ctrl_send_leave(void);
