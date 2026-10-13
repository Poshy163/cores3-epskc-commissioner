#include "power_log.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static power_log_sample_t *s_samples;
static uint64_t s_boot_id;
static uint32_t s_latest, s_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Called once by app_main before the UI and HTTP server start. */
esp_err_t power_log_init(void)
{
    if (s_samples) {
        return ESP_OK;
    }
    s_samples = heap_caps_calloc(POWER_LOG_CAPACITY, sizeof(*s_samples), MALLOC_CAP_SPIRAM);
    if (!s_samples) {
        return ESP_ERR_NO_MEM;
    }
    esp_fill_random(&s_boot_id, sizeof(s_boot_id));
    return ESP_OK;
}

void power_log_note(const power_status_t *ps, esp_err_t result, unsigned screen_percent)
{
    if (!s_samples || !ps) {
        return;
    }
    bool valid = result == ESP_OK;
    power_log_sample_t row = {
        .uptime_ms = esp_timer_get_time() / 1000,
        .percent = valid ? ps->percent : -1,
        .batt_mv = valid ? ps->batt_mv : -1,
        .vbus_mv = valid ? ps->vbus_mv : -1,
        .vsys_mv = valid ? ps->vsys_mv : -1,
        .pmic_temp_c = valid && ps->pmic_temp_c != INT_MIN ? ps->pmic_temp_c : INT16_MIN,
        .screen_percent = screen_percent > 100 ? 100 : screen_percent,
        .raw_st1 = valid ? ps->raw_st1 : 0,
        .raw_st2 = valid ? ps->raw_st2 : 0,
        .valid = valid,
        .present = valid && ps->present,
        .usb = valid && ps->vbus,
        .charging = valid && ps->charging,
        .full = valid && ps->full,
        .estimated = valid && ps->percent_estimated,
    };
    portENTER_CRITICAL(&s_lock);
    row.seq = ++s_latest;
    s_samples[(row.seq - 1) % POWER_LOG_CAPACITY] = row;
    if (s_count < POWER_LOG_CAPACITY) {
        ++s_count;
    }
    portEXIT_CRITICAL(&s_lock);
}

bool power_log_info(power_log_info_t *out)
{
    if (!s_samples || !out) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    *out = (power_log_info_t) {
        .boot_id = s_boot_id, .latest_seq = s_latest, .count = s_count,
        .oldest_seq = s_count ? s_latest - s_count + 1 : 0,
    };
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool power_log_get(uint32_t seq, power_log_sample_t *out)
{
    if (!s_samples || !out || seq == 0) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool found = seq <= s_latest && seq > s_latest - s_count;
    if (found) {
        *out = s_samples[(seq - 1) % POWER_LOG_CAPACITY];
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

static const char *number(int value, int invalid, char *buffer)
{
    if (value == invalid) {
        return "null";
    }
    snprintf(buffer, 12, "%d", value);
    return buffer;
}

bool power_log_parse_uint(const char *value, uint32_t maximum, uint32_t *out)
{
    if (!value || !*value || !out) {
        return false;
    }
    uint64_t n = 0;
    for (; *value; ++value) {
        if (*value < '0' || *value > '9') {
            return false;
        }
        n = n * 10 + (*value - '0');
        if (n > maximum) {
            return false;
        }
    }
    *out = (uint32_t)n;
    return true;
}

size_t power_log_json(const power_log_sample_t *s, char *out, size_t cap)
{
    if (!s || !out || cap == 0) {
        return 0;
    }
    char pct[12], batt[12], bus[12], sys[12], temp[12];
    int n = snprintf(out, cap,
        "{\"seq\":%"PRIu32",\"uptime_ms\":%"PRId64",\"valid\":%s,"
        "\"percent\":%s,\"batt_mv\":%s,\"vbus_mv\":%s,\"vsys_mv\":%s,"
        "\"pmic_temp_c\":%s,\"present\":%s,\"usb\":%s,\"charging\":%s,"
        "\"full\":%s,\"estimated\":%s,\"screen_percent\":%u,\"raw_st1\":%u,\"raw_st2\":%u}",
        s->seq, s->uptime_ms, s->valid ? "true" : "false",
        number(s->percent, -1, pct), number(s->batt_mv, -1, batt),
        number(s->vbus_mv, -1, bus), number(s->vsys_mv, -1, sys),
        number(s->pmic_temp_c, INT16_MIN, temp), s->present ? "true" : "false",
        s->usb ? "true" : "false", s->charging ? "true" : "false",
        s->full ? "true" : "false", s->estimated ? "true" : "false",
        s->screen_percent, s->raw_st1, s->raw_st2);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
