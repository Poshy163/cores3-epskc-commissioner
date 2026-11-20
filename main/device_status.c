#include "device_status.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "log_ring.h"
#include "matter.h"
#include "otbr_rest_wire.h"
#include "power_log.h"
#include "thread.h"
#include "ui.h"
#include "wifi_ctl.h"

static const char *json_bool(bool value) { return value ? "true" : "false"; }

static const char *json_integer(char out[24], bool valid, int64_t value)
{
    if (valid) {
        snprintf(out, 24, "%" PRId64, value);
    } else {
        memcpy(out, "null", 5);
    }
    return out;
}

/* HTTPD has a 4 KB internal-RAM stack. Keep snapshots and escaping/formatting
 * buffers in per-request PSRAM instead of consuming most of that stack before
 * the driver reads and snprintf have even run. No shared scratch state. */
typedef struct {
    app_wifi_status_t wifi;
    thread_activity_t thread;
    log_ring_info_t logs;
    power_log_info_t power_info;
    power_log_sample_t power;
    char version[sizeof(((esp_app_desc_t *)0)->version) + 1];
    char idf[sizeof(((esp_app_desc_t *)0)->idf_ver) + 1];
    char quoted_version[(sizeof(((esp_app_desc_t *)0)->version) + 1) * 6 + 3];
    char quoted_idf[(sizeof(((esp_app_desc_t *)0)->idf_ver) + 1) * 6 + 3];
    char quoted_ip[sizeof(((app_wifi_status_t *)0)->ipv4) * 6 + 3];
    char quoted_ip6[sizeof(((app_wifi_status_t *)0)->ipv6) * 6 + 3];
    char elf[65], boot_id[19], reset[99], matter_state[99], role[63];
    char associated[24], disconnected[24], reason[24], rssi[24], channel[24];
    char power_age[24], percent[24], battery_mv[24], vbus_mv[24], temperature[24];
    char parent_rssi[24], direct_children[24];
} status_scratch_t;

size_t device_status_json(char *out, size_t capacity, bool reboot_pending)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    out[0] = '\0';
    status_scratch_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    if (s == NULL) {
        return 0;
    }
    memset(s, 0, sizeof(*s));
    size_t length = 0;
    app_wifi_get_status(&s->wifi);
    /* Health polling must not pin the single HTTP task behind a busy or
     * unavailable Thread stack. Missing data is reported, never fabricated. */
    const bool thread_valid = thread_activity_get_with_timeout(&s->thread, 100);
    const bool logs_valid = log_ring_info(LOG_RING_CURRENT, &s->logs);
    const bool power_available = power_log_info(&s->power_info) && s->power_info.latest_seq != 0 &&
                                 power_log_get(s->power_info.latest_seq, &s->power);
    /* A sampler may run while we wait for the Thread lock. Read the clock
     * after copying power, so that genuinely newer sample is not labelled as
     * future-dated merely because it followed the beginning of this request. */
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const bool power_valid = power_available && s->power.valid;
    const bool power_age_valid = power_available && s->power.uptime_ms >= 0 && now_ms >= s->power.uptime_ms;
    const int64_t power_age_ms = power_age_valid ? now_ms - s->power.uptime_ms : 0;
    const bool power_fresh = power_valid && power_age_valid && power_age_ms <= 30000;
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc == NULL) {
        goto done;
    }
    memcpy(s->version, desc->version, sizeof(desc->version));
    memcpy(s->idf, desc->idf_ver, sizeof(desc->idf_ver));
    if (!otbr_rest_quote_json_string(s->version, s->quoted_version, sizeof(s->quoted_version)) ||
        !otbr_rest_quote_json_string(s->idf, s->quoted_idf, sizeof(s->quoted_idf)) ||
        !otbr_rest_quote_json_string(s->wifi.ipv4, s->quoted_ip, sizeof(s->quoted_ip)) ||
        !otbr_rest_quote_json_string(s->wifi.ipv6, s->quoted_ip6, sizeof(s->quoted_ip6))) {
        goto done;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        s->elf[2 * i] = hex[desc->app_elf_sha256[i] >> 4];
        s->elf[2 * i + 1] = hex[desc->app_elf_sha256[i] & 15];
    }
    memcpy(s->boot_id, "null", 5);
    if (logs_valid) {
        snprintf(s->boot_id, sizeof(s->boot_id), "\"%016" PRIx64 "\"", s->logs.boot_id);
    }
    if (!otbr_rest_quote_json_string(ui_reset_reason_str(), s->reset, sizeof(s->reset)) ||
        !otbr_rest_quote_json_string(matter_status_string(matter_get_status()), s->matter_state, sizeof(s->matter_state)) ||
        !otbr_rest_quote_json_string(thread_valid ? s->thread.role : "unavailable", s->role, sizeof(s->role))) {
        goto done;
    }
    int n = snprintf(out, capacity,
        "{\"schema\":1,\"boot_id\":%s,\"boot_count\":%" PRIu32 ","
        "\"reset_reason\":%s,\"uptime_ms\":%" PRId64 ","
        "\"firmware\":{\"version\":%s,\"idf\":%s,\"elf_sha256\":\"%s\"},"
        "\"wifi\":{\"connected\":%s,\"rssi\":%s,\"channel\":%s,\"ipv4\":%s,\"ipv6\":%s,"
        "\"associated_ms\":%s,\"disconnect_count\":%" PRIu32 ","
        "\"last_disconnect_reason\":%s,\"last_disconnect_age_ms\":%s,"
        "\"supervisor_on\":%s,\"retry_step\":%d,\"reconnect_pending\":%s},"
        "\"heap\":{\"internal_free\":%u,\"internal_largest\":%u,\"internal_minimum\":%u,"
        "\"psram_free\":%u,\"lvgl_min_stack_free\":%u},"
        "\"thread\":{\"available\":%s,\"role\":%s,\"attached\":%s,\"attach_duration_s\":%" PRIu32 ","
        "\"known_routers\":%u,\"direct_children\":%s,\"parent_rssi\":%s,"
        "\"mac_tx\":%" PRIu32 ",\"mac_rx\":%" PRIu32 ","
        "\"ip_tx_success\":%" PRIu32 ",\"ip_rx_success\":%" PRIu32 ","
        "\"ip_tx_failure\":%" PRIu32 ",\"ip_rx_failure\":%" PRIu32 ",\"parent_changes\":%u},"
        "\"matter\":{\"status\":%s,\"commissioned\":%s,\"commissioning_window_open\":%s,\"endpoint\":%u},"
        "\"power\":{\"available\":%s,\"valid\":%s,\"fresh\":%s,\"age_ms\":%s,\"percent\":%s,"
        "\"battery_mv\":%s,\"vbus_mv\":%s,\"pmic_temp_c\":%s,\"usb\":%s,\"charging\":%s,\"full\":%s},"
        "\"logs\":{\"available\":%s,\"oldest_seq\":%" PRIu32 ",\"latest_seq\":%" PRIu32 ","
        "\"duplicate_mle_count\":%" PRIu32 "},\"remote\":{\"enabled\":true,\"reboot_pending\":%s}}",
        s->boot_id, s->logs.boot_count, s->reset, now_ms, s->quoted_version, s->quoted_idf, s->elf,
        json_bool(s->wifi.connected),
        json_integer(s->rssi, s->wifi.signal_valid, s->wifi.rssi), json_integer(s->channel, s->wifi.signal_valid, s->wifi.channel),
        s->quoted_ip, s->quoted_ip6,
        json_integer(s->associated, s->wifi.association_valid, (int64_t) s->wifi.associated_ms), s->wifi.disconnect_count,
        json_integer(s->reason, s->wifi.last_disconnect_valid, s->wifi.last_disconnect_reason),
        json_integer(s->disconnected, s->wifi.last_disconnect_valid, (int64_t) s->wifi.last_disconnect_age_ms),
        json_bool(s->wifi.supervisor_on), s->wifi.retry_step, json_bool(s->wifi.reconnect_pending),
        (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned) ui_lvgl_stack_free(),
        json_bool(thread_valid), s->role, json_bool(thread_valid && s->thread.attached), s->thread.attach_duration_s,
        (unsigned) s->thread.known_routers,
        json_integer(s->direct_children, thread_valid && s->thread.direct_children_valid, s->thread.direct_children),
        json_integer(s->parent_rssi, thread_valid && s->thread.parent_rssi_valid, s->thread.parent_rssi),
        s->thread.mac_tx_total, s->thread.mac_rx_total, s->thread.ip_tx_success, s->thread.ip_rx_success,
        s->thread.ip_tx_failure, s->thread.ip_rx_failure, (unsigned) s->thread.parent_changes,
        s->matter_state, json_bool(matter_is_commissioned()), json_bool(matter_is_commissioning_window_open()),
        (unsigned) matter_get_thread_border_router_endpoint_id(),
        json_bool(power_available), json_bool(power_valid), json_bool(power_fresh),
        json_integer(s->power_age, power_age_valid, power_age_ms),
        json_integer(s->percent, power_valid && s->power.percent >= 0, s->power.percent),
        json_integer(s->battery_mv, power_valid && s->power.batt_mv >= 0, s->power.batt_mv),
        json_integer(s->vbus_mv, power_valid && s->power.vbus_mv >= 0, s->power.vbus_mv),
        json_integer(s->temperature, power_valid && s->power.pmic_temp_c != INT16_MIN, s->power.pmic_temp_c),
        json_bool(power_valid && s->power.usb), json_bool(power_valid && s->power.charging), json_bool(power_valid && s->power.full),
        json_bool(logs_valid), s->logs.oldest_seq, s->logs.latest_seq, s->logs.duplicate_mle_count, json_bool(reboot_pending));
    if (n < 0 || (size_t) n >= capacity) {
        out[0] = '\0';
    } else {
        length = (size_t) n;
    }
done:
    heap_caps_free(s);
    return length;
}
