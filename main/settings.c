#include "settings.h"

#include <string.h>

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

/* Ignore invalid persisted booleans and retain the existing setting. */
static void settings_load_bool(nvs_handle_t h, const char *key, bool *out)
{
    uint8_t value;
    esp_err_t err = nvs_get_u8(h, key, &value);
    if (err == ESP_OK && value <= 1) {
        *out = value != 0;
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "ignoring invalid saved %s", key);
    }
}

settings_t *settings_get(void)
{
    return &s_cfg;
}

esp_err_t settings_get_device_name(char *name, size_t name_size)
{
    if (name == NULL || name_size < sizeof(SETTINGS_DEFAULT_DEVICE_NAME)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(name, SETTINGS_DEFAULT_DEVICE_NAME, sizeof(SETTINGS_DEFAULT_DEVICE_NAME));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    char stored[SETTINGS_DEVICE_NAME_BUFFER_SIZE];
    size_t stored_size = sizeof(stored);
    err = nvs_get_str(h, "host", stored, &stored_size);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK || stored_size <= 1 || stored_size > sizeof(stored) ||
        stored_size > name_size) {
        ESP_LOGW(TAG, "ignoring invalid saved device name");
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    memcpy(name, stored, stored_size);
    return ESP_OK;
}

esp_err_t settings_set_device_name(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = strlen(name);
    if (length == 0 || length > SETTINGS_DEVICE_NAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "host", name);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
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
    settings_load_bool(h, "router", &s_cfg.prefer_router);
    if (nvs_get_u8(h, "channel", &v) == ESP_OK && (v == 0 || (v >= 11 && v <= 26))) {
        s_cfg.new_net_channel = v;
    }
    if (nvs_get_u8(h, "sharemin", &v) == ESP_OK && (v == 2 || v == 5 || v == 10)) {
        s_cfg.share_minutes = v;
    }
    settings_load_bool(h, "keepawake", &s_cfg.keep_awake_powered);
    settings_load_bool(h, "restapi", &s_cfg.rest_api);
    settings_load_bool(h, "restepskc", &s_cfg.rest_epskc);
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not open NVS to save settings");
        return;
    }
    esp_err_t err = nvs_set_u8(h, "bright", s_cfg.brightness);
    if (err == ESP_OK) { err = nvs_set_u8(h, "sleep", s_cfg.sleep_idx); }
    if (err == ESP_OK) { err = nvs_set_u8(h, "router", s_cfg.prefer_router ? 1 : 0); }
    if (err == ESP_OK) { err = nvs_set_u8(h, "channel", s_cfg.new_net_channel); }
    if (err == ESP_OK) { err = nvs_set_u8(h, "sharemin", s_cfg.share_minutes); }
    if (err == ESP_OK) { err = nvs_set_u8(h, "keepawake", s_cfg.keep_awake_powered ? 1 : 0); }
    if (err == ESP_OK) { err = nvs_set_u8(h, "restapi", s_cfg.rest_api ? 1 : 0); }
    if (err == ESP_OK) { err = nvs_set_u8(h, "restepskc", s_cfg.rest_epskc ? 1 : 0); }
    if (err == ESP_OK) { err = nvs_commit(h); }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not save settings: %s", esp_err_to_name(err));
    }
}

esp_err_t settings_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
