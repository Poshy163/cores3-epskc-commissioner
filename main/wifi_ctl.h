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

/* Read-only link telemetry. No credentials, SSID or radio identifiers are
 * included. Counts cover all station disconnect events since this boot,
 * including deliberate leaves and provisioning attempts. */
typedef struct {
    bool connected;       /* station association; an IP address may follow */
    bool signal_valid;    /* RSSI/channel read without a racing link event */
    int rssi;
    uint8_t channel;
    uint32_t disconnect_count;
    uint16_t last_disconnect_reason; /* zero if event supplied no reason */
    bool association_valid;
    uint64_t associated_ms;
    bool last_disconnect_valid;
    uint64_t last_disconnect_age_ms;
    bool supervisor_on;
    int retry_step;
    bool reconnect_pending;
    char ipv4[16];         /* "-" when unavailable */
    char ipv6[48];
} app_wifi_status_t;

/* Safe before provisioning, during reconnects, and from an HTTP handler.
 * Event metadata is copied under a short lock; radio/timer reads occur
 * outside it. Missing ages and signal values have explicit validity flags. */
void app_wifi_get_status(app_wifi_status_t *out);

/* All three block; call from a worker task, never the LVGL task. */

/* Scan and return up to `max` unique SSIDs, strongest first. */
int app_wifi_scan(wifi_scan_rec_t *out, int max);

/* Set credentials (persisted to NVS by the Wi-Fi driver) and wait for an IP. */
esp_err_t app_wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms);

/* Disconnect and erase the stored credentials. */
esp_err_t app_wifi_leave(void);

/*
 * Match Wi-Fi power management to the power source. On external power the
 * radio never dozes, which keeps the backbone latency of a border router in
 * the milliseconds; on battery it returns to modem sleep. Cheap and
 * idempotent, so a periodic caller can pass the latest VBUS reading.
 */
void app_wifi_set_power_profile(bool external_power);

/* Publish a changed device name without a reboot: Matter NodeLabel and TBR
 * name, then the MeshCoP service instance. Blocks briefly on the OpenThread
 * and Matter locks, so call it from a worker, never the LVGL task. */
void app_apply_device_name(const char *name);

/* Whole-device actions, also used by the Settings screen. */
void app_reboot(void);
/* Thread credentials, Wi-Fi credentials, device name and settings, then reboot. */
void app_factory_reset(void);
