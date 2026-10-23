#include "nvs_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define TAG "nvs_store"
#define NS  "intercom"
#define KEY_MUTED  "muted"
#define KEY_VOL    "vol"

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
