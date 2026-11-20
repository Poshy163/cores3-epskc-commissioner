#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_status_sdk.h"
#include "device_status.h"
#include "log_ring.h"
#include "matter.h"
#include "power_log.h"
#include "thread.h"
#include "ui.h"
#include "wifi_ctl.h"

static esp_app_desc_t app;
static app_wifi_status_t wifi;
static thread_activity_t thread_snapshot;
static log_ring_info_t logs;
static power_log_sample_t power;
static bool thread_ok, logs_ok, power_info_ok, power_get_ok;
static uint32_t latest_power;
static int timeout_reads, power_reads;
static int allocations, frees, live_allocations;
static bool allocation_fails, description_missing;
static const int64_t now_ms = INT64_C(987654321);
static const char *reset_reason;
static const char *matter_state;

const esp_app_desc_t *esp_app_get_description(void) { return description_missing ? NULL : &app; }
void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(size > 0 && size < DEVICE_STATUS_JSON_SIZE && caps == MALLOC_CAP_SPIRAM);
    ++allocations;
    if (allocation_fails) { return NULL; }
    void *p = malloc(size);
    assert(p); ++live_allocations; return p;
}
void heap_caps_free(void *p)
{
    assert(p && live_allocations > 0);
    ++frees; --live_allocations; free(p);
}
int64_t esp_timer_get_time(void) { return now_ms * 1000; }
size_t heap_caps_get_free_size(unsigned caps)
{
    assert(caps == MALLOC_CAP_INTERNAL || caps == MALLOC_CAP_SPIRAM);
    return caps == MALLOC_CAP_INTERNAL ? 123456 : 6000000;
}
size_t heap_caps_get_largest_free_block(unsigned caps)
{
    assert(caps == MALLOC_CAP_INTERNAL); return 65432;
}
size_t heap_caps_get_minimum_free_size(unsigned caps)
{
    assert(caps == MALLOC_CAP_INTERNAL); return 54321;
}
size_t ui_lvgl_stack_free(void) { return 2500; }
const char *ui_reset_reason_str(void) { return reset_reason; }
void app_wifi_get_status(app_wifi_status_t *out) { *out = wifi; }
bool thread_activity_get_with_timeout(thread_activity_t *out, uint32_t timeout_ms)
{
    assert(timeout_ms == 100);
    ++timeout_reads;
    if (thread_ok) { *out = thread_snapshot; }
    return thread_ok;
}
bool thread_activity_get(thread_activity_t *out)
{
    (void)out; assert(!"unbounded Thread read in status handler"); return false;
}
bool log_ring_info(log_ring_boot_t boot, log_ring_info_t *out)
{
    assert(boot == LOG_RING_CURRENT);
    if (logs_ok) { *out = logs; }
    return logs_ok;
}
bool power_log_info(power_log_info_t *out)
{
    if (power_info_ok) { *out = (power_log_info_t){ .latest_seq = latest_power }; }
    return power_info_ok;
}
bool power_log_get(uint32_t seq, power_log_sample_t *out)
{
    assert(power_info_ok && latest_power != 0 && seq == latest_power);
    ++power_reads;
    if (power_get_ok) { *out = power; }
    return power_get_ok;
}
esp_err_t power_read(power_status_t *out)
{
    (void)out; assert(!"status polling performed a live I2C read"); return -1;
}
matter_runtime_status_t matter_get_status(void) { return MATTER_RUNTIME_COMMISSIONED; }
const char *matter_status_string(matter_runtime_status_t state)
{
    assert(state == MATTER_RUNTIME_COMMISSIONED); return matter_state;
}
bool matter_is_commissioned(void) { return true; }
bool matter_is_commissioning_window_open(void) { return false; }
uint16_t matter_get_thread_border_router_endpoint_id(void) { return 1; }

static void valid_fixture(void)
{
    assert(live_allocations == 0);
    allocations = frees = 0;
    allocation_fails = description_missing = false;
    memset(&app, 0, sizeof(app));
    strcpy(app.version, "rev\"\\\n");
    strcpy(app.idf_ver, "v6.0.2\t\001");
    for (unsigned i = 0; i < 32; ++i) { app.app_elf_sha256[i] = (uint8_t)i; }
    wifi = (app_wifi_status_t){ .connected = true, .signal_valid = true,
        .rssi = -62, .channel = 11, .disconnect_count = UINT32_C(4000000000),
        .association_valid = true, .associated_ms = UINT64_C(1099511627776),
        .last_disconnect_valid = true, .last_disconnect_reason = 202,
        .last_disconnect_age_ms = UINT64_C(4294967297), .supervisor_on = true,
        .retry_step = 3, .reconnect_pending = false };
    strcpy(wifi.ipv4, "1\"\\\n.2");
    strcpy(wifi.ipv6, "fe80::1\t\001");
    thread_snapshot = (thread_activity_t){ .attached = true, .attach_duration_s = 12345,
        .known_routers = 23, .direct_children_valid = true, .direct_children = 13,
        .parent_rssi_valid = true, .parent_rssi = -69,
        .mac_tx_total = UINT32_MAX, .mac_rx_total = UINT32_C(4000000001),
        .ip_tx_success = 1001, .ip_rx_success = 2002, .ip_tx_failure = 3,
        .ip_rx_failure = 4, .parent_changes = 1234 };
    strcpy(thread_snapshot.role, "r\"\\\n");
    logs = (log_ring_info_t){ .boot_id = UINT64_C(0xfedcba9876543210),
        .boot_count = UINT32_MAX, .oldest_seq = 101, .latest_seq = 1124,
        .duplicate_mle_count = UINT32_C(4000000002) };
    power = (power_log_sample_t){ .seq = 42, .uptime_ms = now_ms - 12345,
        .valid = true, .percent = 73, .batt_mv = 4098, .vbus_mv = 5021,
        .pmic_temp_c = 41, .usb = true, .charging = false, .full = false };
    reset_reason = "SW\"\\\r\001";
    matter_state = "commissioned\"\\\n";
    thread_ok = logs_ok = power_info_ok = power_get_ok = true;
    latest_power = 42;
    timeout_reads = power_reads = 0;
}

static void emit(bool pending)
{
    char out[DEVICE_STATUS_JSON_SIZE];
    int reads_before = timeout_reads;
    int allocations_before = allocations, frees_before = frees;
    size_t n = device_status_json(out, sizeof(out), pending);
    assert(n > 0 && n == strlen(out) && timeout_reads == reads_before + 1);
    assert(allocations == allocations_before + 1 && frees == frees_before + 1 && live_allocations == 0);
    puts(out);
}

int main(void)
{
    valid_fixture();
    emit(true);
    assert(power_reads == 1);

    valid_fixture();
    thread_ok = logs_ok = power_info_ok = false;
    wifi.signal_valid = wifi.association_valid = wifi.last_disconnect_valid = false;
    wifi.connected = false;
    emit(false);
    assert(power_reads == 0);

    valid_fixture();
    thread_snapshot.direct_children_valid = thread_snapshot.parent_rssi_valid = false;
    power.valid = false;
    emit(false);

    valid_fixture();
    power.percent = power.batt_mv = power.vbus_mv = -1;
    power.pmic_temp_c = INT16_MIN;
    power.uptime_ms = now_ms + 1;
    emit(false);

    valid_fixture();
    latest_power = 0;
    emit(false);
    assert(power_reads == 0);

    valid_fixture();
    power_get_ok = false;
    emit(false);

    valid_fixture();
    memset(app.version, 'V', sizeof(app.version));
    memset(app.idf_ver, 'I', sizeof(app.idf_ver));
    emit(false);

    valid_fixture();
    power.uptime_ms = now_ms - 30000;
    emit(false);

    valid_fixture();
    power.uptime_ms = now_ms - 30001;
    emit(false);

    valid_fixture();
    power.uptime_ms = -1;
    emit(false);

    valid_fixture();
    char complete[DEVICE_STATUS_JSON_SIZE];
    size_t needed = device_status_json(complete, sizeof(complete), false);
    assert(needed > 512);
    const size_t capacities[] = { 1, 2, 20, 512, needed };
    for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); ++i) {
        char out[DEVICE_STATUS_JSON_SIZE];
        memset(out, '!', sizeof(out));
        assert(device_status_json(out, capacities[i], false) == 0);
        assert(out[0] == '\0' && out[capacities[i]] == '!');
        assert(live_allocations == 0);
    }
    char out[DEVICE_STATUS_JSON_SIZE];
    memset(out, '!', sizeof(out));
    assert(device_status_json(out, needed + 1, false) == needed && out[needed + 1] == '!');
    assert(live_allocations == 0);
    assert(strcmp(out, complete) == 0);
    int before = allocations;
    assert(device_status_json(NULL, sizeof(out), false) == 0 && allocations == before);
    out[0] = '!';
    assert(device_status_json(out, 0, false) == 0 && out[0] == '!' && allocations == before);
    allocation_fails = true;
    int reads_before = timeout_reads;
    assert(device_status_json(out, sizeof(out), false) == 0 && out[0] == '\0');
    assert(live_allocations == 0 && allocations == before + 1 && timeout_reads == reads_before);
    allocation_fails = false;
    description_missing = true;
    assert(device_status_json(out, sizeof(out), false) == 0 && out[0] == '\0' && live_allocations == 0);
    description_missing = false;
    reset_reason = NULL;
    assert(device_status_json(out, sizeof(out), false) == 0 && out[0] == '\0');
    assert(live_allocations == 0);
    char too_long[200];
    memset(too_long, 'x', sizeof(too_long) - 1); too_long[sizeof(too_long) - 1] = '\0';
    reset_reason = too_long;
    assert(device_status_json(out, sizeof(out), false) == 0 && out[0] == '\0');
    assert(live_allocations == 0 && allocations == frees + 1); /* the one injected OOM */
    return 0;
}
