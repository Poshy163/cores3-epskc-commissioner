#include "settings.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";
#define NS "cfg"

const uint32_t settings_sleep_ms[4] = { 30000, 60000, 300000, 0 };

static settings_t s_cfg = {
    .brightness = 100,
    .sleep_idx = 1,
    .prefer_router = false,
    .new_net_channel = 0,
    .share_minutes = 5,
    .keep_awake_powered = true,
    .rest_api = true,
    .rest_epskc = true,
};

settings_t *settings_get(void)
{
    return &s_cfg;
}

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;   /* first boot: defaults stand */
    }
    uint8_t v;
    if (nvs_get_u8(h, "bright", &v) == ESP_OK && v >= 10 && v <= 100) {
        s_cfg.brightness = v;
    }
    if (nvs_get_u8(h, "sleep", &v) == ESP_OK && v < 4) {
        s_cfg.sleep_idx = v;
    }
    if (nvs_get_u8(h, "router", &v) == ESP_OK) {
        s_cfg.prefer_router = v != 0;
    }
    if (nvs_get_u8(h, "channel", &v) == ESP_OK && (v == 0 || (v >= 11 && v <= 26))) {
        s_cfg.new_net_channel = v;
    }
    if (nvs_get_u8(h, "sharemin", &v) == ESP_OK && (v == 2 || v == 5 || v == 10)) {
        s_cfg.share_minutes = v;
    }
    if (nvs_get_u8(h, "keepawake", &v) == ESP_OK) {
        s_cfg.keep_awake_powered = v != 0;
    }
    if (nvs_get_u8(h, "restapi", &v) == ESP_OK) {
        s_cfg.rest_api = v != 0;
    }
    if (nvs_get_u8(h, "restepskc", &v) == ESP_OK) {
        s_cfg.rest_epskc = v != 0;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not open NVS to save settings");
        return;
    }
    nvs_set_u8(h, "bright", s_cfg.brightness);
    nvs_set_u8(h, "sleep", s_cfg.sleep_idx);
    nvs_set_u8(h, "router", s_cfg.prefer_router ? 1 : 0);
    nvs_set_u8(h, "channel", s_cfg.new_net_channel);
    nvs_set_u8(h, "sharemin", s_cfg.share_minutes);
    nvs_set_u8(h, "keepawake", s_cfg.keep_awake_powered ? 1 : 0);
    nvs_set_u8(h, "restapi", s_cfg.rest_api ? 1 : 0);
    nvs_set_u8(h, "restepskc", s_cfg.rest_epskc ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void settings_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}
