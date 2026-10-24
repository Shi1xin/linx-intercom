/* Linx platform WebSocket client + device-side MCP server.
 * Docs: platform/websocket, mcp/hardware-mcp. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t linx_client_init(void);

/* Connect wss with the device headers; posts APP_EV_LINX_* events. */
esp_err_t linx_client_connect(const char *url, const char *token,
                              const char *device_id, const char *client_id);
esp_err_t linx_client_disconnect(void);
bool      linx_client_is_connected(void);

/* Session messages */
esp_err_t linx_send_hello(void);
esp_err_t linx_send_listen(const char *state, const char *mode); /* start|stop|detect */
esp_err_t linx_send_detect_text(const char *text);
esp_err_t linx_send_abort(const char *reason);

/* One opus 16k/20ms frame upstream (binary ws message). */
int       linx_send_audio(const uint8_t *data, int size);

/* MCP replies. id_raw is the exact JSON text of the rpc id (number or string). */
esp_err_t linx_mcp_reply_result(const char *id_raw, const char *text_json, bool is_error);
esp_err_t linx_mcp_reply_error(const char *id_raw, int code, const char *message);

/* Downlink TTS gating: app decides whether a platform audio frame may enter
 * the playback queue (idle-linx mode always; intercom only during an open
 * command/reply window). */
void      linx_set_playback_gate(bool allow);
