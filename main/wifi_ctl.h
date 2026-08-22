/*
 * Wi-Fi control surface for the touch UI. Implemented in main.c, which owns
 * the Wi-Fi event handling.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char ssid[33];
    int  rssi;
    bool secured;
} wifi_scan_rec_t;

/* All three block; call from a worker task, never the LVGL task. */

/* Scan and return up to `max` unique SSIDs, strongest first. */
int app_wifi_scan(wifi_scan_rec_t *out, int max);

/* Set credentials (persisted to NVS by the Wi-Fi driver) and wait for an IP. */
esp_err_t app_wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms);

/* Disconnect and erase the stored credentials. */
esp_err_t app_wifi_leave(void);

/* Whole-device actions, also used by the Settings screen. */
void app_reboot(void);
/* Thread credentials, Wi-Fi credentials, device name and settings, then reboot. */
void app_factory_reset(void);
