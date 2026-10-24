#pragma once

#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"

#if CONFIG_INTERCOM_WAKE_PCM_TRACE
esp_err_t wake_trace_init(void);
/* Called only by the mono 16 kHz AFE feed producer, after each feed call. */
void wake_trace_feed(const int16_t *pcm, size_t samples);
/* Coalesces one request; the next feed snapshots the last eight input seconds. */
void wake_trace_request(void);
#else
static inline esp_err_t wake_trace_init(void) { return ESP_OK; }
static inline void wake_trace_feed(const int16_t *pcm, size_t samples)
{
    (void)pcm;
    (void)samples;
}
static inline void wake_trace_request(void) {}
#endif
