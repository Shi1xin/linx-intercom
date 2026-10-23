#include "nvs_store.h"
#include <stdio.h>
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define TAG "nvs_store"
#define NS  "intercom"
#define KEY_MUTED  "muted"
#define KEY_VOL    "vol"
#define KEY_LINXCID "linx_cid"

static nvs_handle_t s_nvs;

esp_err_t nvs_store_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    return nvs_open(NS, NVS_READWRITE, &s_nvs);
}

bool nvs_store_get_muted(void)
{
    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, KEY_MUTED, &v) != ESP_OK) {
        return false;
    }
    return v != 0;
}

esp_err_t nvs_store_set_muted(bool muted)
{
    esp_err_t ret = nvs_set_u8(s_nvs, KEY_MUTED, muted ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(s_nvs);
    }
    return ret;
}

int nvs_store_get_volume(void)
{
    uint8_t v = 70;
    nvs_get_u8(s_nvs, KEY_VOL, &v);
    if (v > 100) {
        v = 100;
    }
    return v;
}

esp_err_t nvs_store_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    esp_err_t ret = nvs_set_u8(s_nvs, KEY_VOL, (uint8_t)volume);
    if (ret == ESP_OK) {
        ret = nvs_commit(s_nvs);
    }
    return ret;
}

/* Persistent platform client-id (uuid v4); generated once on first boot. */
esp_err_t nvs_store_get_client_id(char *out, size_t n)
{
    size_t len = n;
    if (nvs_get_str(s_nvs, KEY_LINXCID, out, &len) == ESP_OK && out[0]) {
        return ESP_OK;
    }
    uint32_t r[4];
    for (int i = 0; i < 4; i++) {
        r[i] = esp_random();
    }
    snprintf(out, n, "%08lx-%04lx-%04lx-%04lx-%04lx%08lx",
             (unsigned long)r[0], (unsigned long)(r[1] & 0xffff),
             (unsigned long)((r[1] >> 16 & 0x0fff) | 0x4000),
             (unsigned long)((r[2] & 0x3fff) | 0x8000),
             (unsigned long)(r[2] >> 16 & 0xffff), (unsigned long)r[3]);
    esp_err_t ret = nvs_set_str(s_nvs, KEY_LINXCID, out);
    if (ret == ESP_OK) {
        ret = nvs_commit(s_nvs);
    }
    return ret;
}
