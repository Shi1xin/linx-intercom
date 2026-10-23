/* Application state machine shared across modules */
#pragma once

typedef enum {
    APP_STATE_IDLE = 0,   /* hello done, no active room */
    APP_STATE_JOINING,    /* join sent, waiting media */
    APP_STATE_TALKING,    /* media established */
    APP_STATE_COMMAND,    /* command key held, uplink paused, capturing */
    APP_STATE_SWITCHING,  /* room switch in progress */
    APP_STATE_LEAVING,    /* leave sent */
} app_state_t;

const char *app_state_name(app_state_t s);
