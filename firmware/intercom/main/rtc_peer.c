#include "rtc_peer.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "app_events.h"
#include "audio_dev.h"
#include "board.h"

#define TAG "rtc_peer"

#define MAX_PENDING_CANDIDATES 16

typedef struct cand_node {
    char *cand;
    struct cand_node *next;
} cand_node_t;

static esp_peer_handle_t s_peer;
static TaskHandle_t      s_pc_task;
static SemaphoreHandle_t s_send_lock;
static volatile bool     s_loop_run;
static volatile bool     s_peer_connected;
static volatile bool     s_remote_sdp_set;
static cand_node_t      *s_pending_head;
static int               s_pending_num;
static int               s_local_cand_sent;
static bool              s_offer_sent;

/* ICE servers configured from Kconfig (TURN) */
static esp_peer_ice_server_cfg_t s_ice_servers[1];
static char s_turn_url[96];
static char s_turn_user[32];
static char s_turn_pass[32];

/* ---------- callbacks (run in pc task context) ---------- */

static int peer_on_state(esp_peer_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "peer state %d", state);
    if (state == ESP_PEER_STATE_CONNECTED) {
        s_peer_connected = true;
    } else if (state == ESP_PEER_STATE_DISCONNECTED ||
               state == ESP_PEER_STATE_CONNECT_FAILED ||
               state == ESP_PEER_STATE_CLOSED) {
        s_peer_connected = false;
    }
    app_post(APP_EV_PEER_STATE, NULL, NULL, (int)state, false);
    return 0;
}

/* Extract the nth "a=candidate:" line out of an SDP, return heap string in
 * bare "candidate:..." form or NULL. */
static char *sdp_get_candidate_line(const char *sdp, int index)
{
    const char *p = sdp;
    int n = 0;
    while ((p = strstr(p, "a=candidate:")) != NULL) {
        if (n == index) {
            const char *end = strstr(p, "\r\n");
            if (end == NULL) {
                end = strchr(p, '\n');
            }
            size_t len = end ? (size_t)(end - p) : strlen(p);
            /* strip "a=" prefix */
            const char *start = p + 2;
            size_t blen = len - 2;
            char *out = malloc(blen + 1);
            if (out) {
                memcpy(out, start, blen);
                out[blen] = 0;
            }
            return out;
        }
        n++;
        p += 2;
    }
    return NULL;
}

static int sdp_candidate_count(const char *sdp)
{
    int n = 0;
    const char *p = sdp;
    while ((p = strstr(p, "a=candidate:")) != NULL) {
        n++;
        p += 2;
    }
    return n;
}

static int peer_on_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (msg->type != ESP_PEER_MSG_TYPE_SDP || msg->data == NULL) {
        return 0;
    }
    const char *sdp = (const char *)msg->data;
    if (!s_offer_sent) {
        /* First SDP emission is the offer (may already embed candidates) */
        s_offer_sent = true;
        s_local_cand_sent = sdp_candidate_count(sdp);
        app_post_str(APP_EV_PEER_SDP, sdp, NULL, 0, false);
    } else {
        /* Later emissions: diff candidates and trickle them */
        int total = sdp_candidate_count(sdp);
        for (int i = s_local_cand_sent; i < total; i++) {
            char *cand = sdp_get_candidate_line(sdp, i);
            if (cand) {
                app_post(APP_EV_PEER_ICE, cand, NULL, 0, false);
            }
        }
        s_local_cand_sent = total;
    }
    return 0;
}

static int peer_on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx)
{
    ESP_LOGI(TAG, "remote audio codec %d rate %lu ch %d",
             info->codec, (unsigned long)info->sample_rate, info->channel);
    return 0;
}

static int peer_on_audio_data(esp_peer_audio_frame_t *frame, void *ctx)
{
    audio_rx_enqueue(frame->data, frame->size);
    return 0;
}

/* ---------- pending remote candidates ---------- */

static void flush_pending_candidates(void)
{
    cand_node_t *n = s_pending_head;
    while (n) {
        esp_peer_msg_t msg = {
            .type = ESP_PEER_MSG_TYPE_CANDIDATE,
            .data = (uint8_t *)n->cand,
            .size = strlen(n->cand),
        };
        esp_peer_send_msg(s_peer, &msg);
        cand_node_t *nx = n->next;
        free(n->cand);
        free(n);
        n = nx;
    }
    s_pending_head = NULL;
    s_pending_num = 0;
}

esp_err_t rtc_peer_add_remote_candidate(const char *candidate)
{
    if (s_peer == NULL || candidate == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_remote_sdp_set) {
        if (s_pending_num >= MAX_PENDING_CANDIDATES) {
            return ESP_ERR_NO_MEM;
        }
        cand_node_t *n = calloc(1, sizeof(cand_node_t));
        if (n == NULL) {
            return ESP_ERR_NO_MEM;
        }
        n->cand = strdup(candidate);
        if (n->cand == NULL) {
            free(n);
            return ESP_ERR_NO_MEM;
        }
        /* append to keep order */
        if (s_pending_head == NULL) {
            s_pending_head = n;
        } else {
            cand_node_t *tail = s_pending_head;
            while (tail->next) tail = tail->next;
            tail->next = n;
        }
        s_pending_num++;
        return ESP_OK;
    }
    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_CANDIDATE,
        .data = (uint8_t *)candidate,
        .size = strlen(candidate),
    };
    return esp_peer_send_msg(s_peer, &msg) == ESP_PEER_ERR_NONE ? ESP_OK : ESP_FAIL;
}

esp_err_t rtc_peer_set_remote_sdp(const char *sdp)
{
    if (s_peer == NULL || sdp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)sdp,
        .size = strlen(sdp),
    };
    int ret = esp_peer_send_msg(s_peer, &msg);
    if (ret == ESP_PEER_ERR_NONE) {
        s_remote_sdp_set = true;
        flush_pending_candidates();
    }
    return ret == ESP_PEER_ERR_NONE ? ESP_OK : ESP_FAIL;
}

/* ---------- lifecycle ---------- */

static void pc_task(void *arg)
{
    esp_peer_handle_t peer = (esp_peer_handle_t)arg;
    while (s_loop_run) {
        esp_peer_main_loop(peer);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_pc_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t rtc_peer_init(void)
{
    return esp_peer_pre_generate_cert() == ESP_PEER_ERR_NONE ? ESP_OK : ESP_FAIL;
}

esp_err_t rtc_peer_start(void)
{
    if (s_peer) {
        return ESP_OK;
    }
    uint8_t server_num = 0;
    if (strlen(CONFIG_INTERCOM_TURN_URL) > 0) {
        strlcpy(s_turn_url, CONFIG_INTERCOM_TURN_URL, sizeof(s_turn_url));
        strlcpy(s_turn_user, CONFIG_INTERCOM_TURN_USER, sizeof(s_turn_user));
        strlcpy(s_turn_pass, CONFIG_INTERCOM_TURN_PASS, sizeof(s_turn_pass));
        s_ice_servers[0].stun_url = s_turn_url;
        s_ice_servers[0].user = s_turn_user;
        s_ice_servers[0].psw = s_turn_pass;
        server_num = 1;
    }

    esp_peer_default_cfg_t peer_default = {
        .agent_recv_timeout = 100,
        .rtp_cfg = {
            .audio_recv_jitter = { .cache_size = 64 * 1024 },
            .send_pool_size = 128 * 1024,
            .send_queue_num = 64,
        },
    };
    esp_peer_cfg_t cfg = {
        .server_lists = server_num ? s_ice_servers : NULL,
        .server_num = server_num,
        .role = ESP_PEER_ROLE_CONTROLLING,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
        .audio_info = {
            .codec = ESP_PEER_AUDIO_CODEC_OPUS,
            .sample_rate = AUDIO_WIRE_SAMPLE_RATE,
            .channel = 1,
        },
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = false,
        .no_auto_reconnect = true,
        .on_state = peer_on_state,
        .on_msg = peer_on_msg,
        .on_audio_info = peer_on_audio_info,
        .on_audio_data = peer_on_audio_data,
        .ctx = NULL,
        .extra_cfg = &peer_default,
        .extra_size = sizeof(peer_default),
    };
    int ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &s_peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed %d", ret);
        return ESP_FAIL;
    }
    s_remote_sdp_set = false;
    s_offer_sent = false;
    s_local_cand_sent = 0;
    s_pending_head = NULL;
    s_pending_num = 0;
    if (s_send_lock == NULL) {
        s_send_lock = xSemaphoreCreateMutex();
    }
    s_loop_run = true;
    /* janus_demo schedule: pc_task stack 25k, prio 18, core 1 */
    if (xTaskCreatePinnedToCore(pc_task, "pc_task", 25 * 1024, s_peer, 18, &s_pc_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "pc_task create failed");
        esp_peer_close(s_peer);
        s_peer = NULL;
        s_loop_run = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t rtc_peer_create_offer(void)
{
    if (s_peer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = esp_peer_new_connection(s_peer);
    return ret == ESP_PEER_ERR_NONE ? ESP_OK : ESP_FAIL;
}

int rtc_peer_send_audio(const uint8_t *data, int size, uint32_t pts)
{
    int ret = ESP_PEER_ERR_WRONG_STATE;
    if (s_send_lock) {
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
    }
    if (s_peer != NULL && s_peer_connected) {
        esp_peer_audio_frame_t frame = {
            .pts = pts,
            .data = (uint8_t *)data,
            .size = size,
        };
        ret = esp_peer_send_audio(s_peer, &frame);
    }
    if (s_send_lock) {
        xSemaphoreGive(s_send_lock);
    }
    return ret;
}

bool rtc_peer_is_connected(void)
{
    return s_peer_connected;
}

bool rtc_peer_is_running(void)
{
    return s_peer != NULL;
}

void rtc_peer_stop(void)
{
    if (s_peer == NULL) {
        return;
    }
    esp_peer_handle_t peer = s_peer;
    /* stop loop first so close() does not race an in-flight main_loop;
     * esp_peer_close performs disconnect internally. */
    s_loop_run = false;
    for (int i = 0; i < 100 && s_pc_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_send_lock) {
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
    }
    s_peer = NULL;
    s_peer_connected = false;
    esp_peer_close(peer);
    if (s_send_lock) {
        xSemaphoreGive(s_send_lock);
    }
    /* drop buffered candidates */
    cand_node_t *n = s_pending_head;
    while (n) {
        cand_node_t *nx = n->next;
        free(n->cand);
        free(n);
        n = nx;
    }
    s_pending_head = NULL;
    s_pending_num = 0;
    s_remote_sdp_set = false;
}
