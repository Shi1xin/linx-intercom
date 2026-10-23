/* Internal event queue plumbing.
 * All producers (ws client task, peer task, button task, capture task) post
 * app_event_t to the single app queue; app_main serializes handling.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    APP_EV_WIFI_CONNECTED = 0,
    APP_EV_WIFI_DISCONNECTED,
    APP_EV_WS_CONNECTED,
    APP_EV_WS_DISCONNECTED,
    /* control protocol results, payload in evt->msg */
    APP_EV_HELLO_RESULT,
    APP_EV_JOIN_RESULT,
    APP_EV_SDP,            /* remote answer */
    APP_EV_ICE,            /* remote candidate or completed */
    APP_EV_CONFIRM_RESULT,
    APP_EV_ROOM_INFO_RESULT,
    APP_EV_MUTE_RESULT,
    APP_EV_LEAVE_RESULT,
    APP_EV_NOTIFY,
    APP_EV_ERROR_MSG,
    /* peer local outputs */
    APP_EV_PEER_SDP,       /* local SDP (offer) ready, msg = sdp text */
    APP_EV_PEER_ICE,       /* local trickle candidate, msg = "candidate:..." */
    APP_EV_PEER_STATE,     /* arg0 = esp_peer_state_t */
    /* buttons */
    APP_EV_CMD_PRESS,
    APP_EV_CMD_RELEASE,
    APP_EV_TOUCH_TAP,
    APP_EV_VOL_UP,
    APP_EV_VOL_DOWN,
    /* command capture finished, msg = join JSON target already built */
    APP_EV_CMD_RESULT,
    APP_EV_RECONNECT_TIMER,
} app_ev_type_t;

typedef struct {
    app_ev_type_t type;
    char *msg;      /* heap string, owned by handler (freed after use), may be NULL */
    char *msg2;     /* secondary string (e.g. join target arg, room name) */
    int   arg0;     /* numeric payload */
    bool  flag;     /* bool payload (ok / need_media / muted / completed) */
} app_event_t;

extern QueueHandle_t g_app_queue;

/* Post an event. msg/msg2 ownership transfers to the queue consumer. */
bool app_post(app_ev_type_t type, char *msg, char *msg2, int arg0, bool flag);

/* Convenience: duplicate str and post; safe to pass NULL. */
bool app_post_str(app_ev_type_t type, const char *msg, const char *msg2, int arg0, bool flag);
