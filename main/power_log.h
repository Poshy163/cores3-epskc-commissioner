#pragma once

#include <stddef.h>
#include "power.h"

/* 24 hours at the normal 10-second polling interval. RAM survives USB
 * reconnection, not reboot; the PC recorder supplies durable storage. */
#define POWER_LOG_CAPACITY 8640

typedef struct {
    uint32_t seq;
    int64_t uptime_ms;
    int16_t percent, batt_mv, vbus_mv, vsys_mv, pmic_temp_c;
    uint8_t screen_percent, raw_st1, raw_st2;
    bool valid, present, usb, charging, full, estimated;
} power_log_sample_t;

typedef struct {
    uint64_t boot_id;
    uint32_t oldest_seq, latest_seq, count;
} power_log_info_t;

esp_err_t power_log_init(void);
void power_log_note(const power_status_t *status, esp_err_t result, unsigned screen_percent);
bool power_log_info(power_log_info_t *out);
bool power_log_get(uint32_t seq, power_log_sample_t *out);

/* JSON row with null for unavailable measurements. Returns zero on overflow. */
size_t power_log_json(const power_log_sample_t *sample, char *out, size_t cap);
bool power_log_parse_uint(const char *value, uint32_t maximum, uint32_t *out);
