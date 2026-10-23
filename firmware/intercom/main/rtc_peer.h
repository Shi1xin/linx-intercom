/* esp_peer PeerConnection wrapper: SDP/ICE plumbed through the app queue */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* One-time init: pre-generates DTLS cert material. Call after wifi up. */
esp_err_t rtc_peer_init(void);

/* Create PeerConnection + start main-loop task. Idempotent. */
esp_err_t rtc_peer_start(void);

/* Begin ICE gathering and emit local SDP via APP_EV_PEER_SDP /
 * trickle candidates via APP_EV_PEER_ICE. */
esp_err_t rtc_peer_create_offer(void);

/* Apply remote answer SDP, then flush buffered remote candidates. */
esp_err_t rtc_peer_set_remote_sdp(const char *sdp);

/* Remote trickle candidate ("candidate:..." bare form). Buffered until the
 * answer is applied. */
esp_err_t rtc_peer_add_remote_candidate(const char *candidate);

/* Encoded opus frame -> peer. Safe to call from the capture task. */
int       rtc_peer_send_audio(const uint8_t *data, int size, uint32_t pts);

bool      rtc_peer_is_connected(void);
bool      rtc_peer_is_running(void);

/* disconnect + close + join loop task */
void      rtc_peer_stop(void);
