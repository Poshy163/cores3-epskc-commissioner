#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
typedef unsigned EventBits_t;
typedef struct { struct { unsigned char ssid[32], password[64]; } sta; } wifi_config_t;
typedef struct { char ssid[33]; int rssi; int authmode; } wifi_ap_record_t;
typedef struct { char ssid[33]; int rssi; bool secured; } wifi_scan_rec_t;
typedef struct { int unused; } wifi_scan_config_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NO_MEM 4
#define WIFI_STORAGE_RAM 0
#define WIFI_STORAGE_FLASH 1
#define WIFI_IF_STA 0
#define WIFI_AUTH_OPEN 0
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 999999
#define pdMS_TO_TICKS(x) (x)
#define WIFI_CONNECTED_BIT 1
#define WIFI_FAIL_BIT 2
#define WIFI_DISCONNECTED_BIT 4
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)

static void *s_wifi_op_lock = (void *)1;
static int s_wifi_events;
static bool s_join_active, s_wifi_hold, s_have_credentials;
static int s_retries;
static int storage, fail_storage_ram, fail_storage_flash, fail_set_at, set_calls;
static int disconnects, connects, fail_scan, fail_count, fail_records, fail_alloc;
static int clear_calls, outstanding_scan, unlocks, fail_snapshot;
static EventBits_t verdict;
static wifi_config_t current, persisted;
static wifi_ap_record_t ap_records[4];

static int xSemaphoreTake(void *lock, uint32_t timeout) { return lock != NULL; }
static void xSemaphoreGive(void *lock) { unlocks++; }
static void cancel_reconnect(void) {}
static void wifi_disconnect_for_config_switch(void) { disconnects++; s_wifi_hold = true; }
static const char *esp_err_to_name(int err) { return "injected"; }
static esp_err_t esp_wifi_set_storage(int which) {
    if ((which == WIFI_STORAGE_RAM && fail_storage_ram) ||
        (which == WIFI_STORAGE_FLASH && fail_storage_flash)) return ESP_FAIL;
    storage = which;
    return ESP_OK;
}
static esp_err_t esp_wifi_get_config(int interface, wifi_config_t *cfg) {
    if (fail_snapshot) return ESP_FAIL;
    *cfg = current;
    return ESP_OK;
}
static esp_err_t esp_wifi_set_config(int interface, wifi_config_t *cfg) {
    if (++set_calls == fail_set_at) return ESP_FAIL;
    current = *cfg;
    if (storage == WIFI_STORAGE_FLASH) persisted = *cfg;
    return ESP_OK;
}
static esp_err_t esp_wifi_connect(void) { connects++; return ESP_OK; }
static EventBits_t xEventGroupClearBits(int group, EventBits_t bits) { return 0; }
static EventBits_t xEventGroupWaitBits(int group, EventBits_t bits, int clear, int all, unsigned timeout) {
    return verdict;
}
static esp_err_t esp_wifi_scan_start(wifi_scan_config_t *cfg, bool block) {
    outstanding_scan = !fail_scan;
    return fail_scan ? ESP_FAIL : ESP_OK;
}
static esp_err_t esp_wifi_scan_get_ap_num(uint16_t *n) {
    *n = 4;
    return fail_count ? ESP_FAIL : ESP_OK;
}
static esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *out) {
    if (fail_records) return ESP_FAIL;
    memcpy(out, ap_records, *n * sizeof(*out));
    outstanding_scan = 0;
    return ESP_OK;
}
static esp_err_t esp_wifi_clear_ap_list(void) {
    clear_calls++;
    outstanding_scan = 0;
    return ESP_OK;
}
static void *test_calloc(size_t n, size_t size) { return fail_alloc ? NULL : calloc(n, size); }
#define calloc test_calloc
#include "wifi_production.inc"
#undef calloc

static void reset(void) {
    storage = WIFI_STORAGE_FLASH;
    fail_storage_ram = fail_storage_flash = fail_set_at = set_calls = 0;
    disconnects = connects = fail_scan = fail_count = fail_records = fail_alloc = 0;
    clear_calls = outstanding_scan = unlocks = fail_snapshot = 0;
    s_wifi_hold = s_join_active = false;
    s_have_credentials = true;
    s_retries = 0;
    verdict = WIFI_CONNECTED_BIT;
    memset(&current, 0, sizeof(current));
    strcpy((char *)current.sta.ssid, "previous");
    strcpy((char *)current.sta.password, "previous-password");
    persisted = current;
    memset(ap_records, 0, sizeof(ap_records));
    strcpy(ap_records[0].ssid, "one");
    strcpy(ap_records[1].ssid, "one");
    strcpy(ap_records[2].ssid, "two");
}

int main(void) {
    /* Force external API failures through the actual production branches. */
    reset(); fail_storage_ram = 1;
    assert(app_wifi_join("candidate", "candidate-pass", 20000) != ESP_OK);
    assert(set_calls == 0 && disconnects == 0 && !s_join_active && unlocks == 1);
    assert(strcmp((char *)persisted.sta.ssid, "previous") == 0);

    reset(); fail_storage_flash = 1;
    assert(app_wifi_join("candidate", "candidate-pass", 20000) != ESP_OK);
    assert(strcmp((char *)persisted.sta.ssid, "previous") == 0);

    reset(); fail_set_at = 2;
    assert(app_wifi_join("candidate", "candidate-pass", 20000) != ESP_OK);
    assert(strcmp((char *)persisted.sta.ssid, "previous") == 0);

    reset(); verdict = WIFI_FAIL_BIT;
    assert(app_wifi_join("candidate", "candidate-pass", 20000) != ESP_OK);
    assert(strcmp((char *)current.sta.ssid, "previous") == 0);
    assert(strcmp((char *)persisted.sta.ssid, "previous") == 0);
    assert(!s_wifi_hold && !s_join_active && storage == WIFI_STORAGE_FLASH);

    reset(); verdict = 0; fail_set_at = 2;
    assert(app_wifi_join("candidate", "candidate-pass", 20000) != ESP_OK);
    assert(s_wifi_hold && !s_join_active);
    assert(strcmp((char *)persisted.sta.ssid, "previous") == 0);

    reset(); memset(&current, 0, sizeof(current)); persisted = current; verdict = 0;
    assert(app_wifi_join("candidate", "candidate-pass", 20000) != ESP_OK);
    assert(s_wifi_hold && !s_have_credentials && current.sta.ssid[0] == 0);
    assert(persisted.sta.ssid[0] == 0);

    reset(); fail_snapshot = 1;
    assert(app_wifi_join("candidate", "candidate-pass", 20000) != ESP_OK);
    assert(set_calls == 0 && disconnects == 0 && storage == WIFI_STORAGE_FLASH);

    reset();
    assert(app_wifi_join("candidate", "candidate-pass", 20000) == ESP_OK);
    assert(strcmp((char *)persisted.sta.ssid, "candidate") == 0);

    reset(); char ssid32[33], psk64[65];
    memset(ssid32, 's', 32); ssid32[32] = 0;
    memset(psk64, 'a', 64); psk64[64] = 0;
    assert(app_wifi_join(ssid32, psk64, 20000) == ESP_OK);
    assert(memcmp(persisted.sta.ssid, ssid32, 32) == 0);
    assert(memcmp(persisted.sta.password, psk64, 64) == 0);

    reset();
    assert(app_wifi_join("123456789012345678901234567890123", "password", 20000) == ESP_ERR_INVALID_ARG);
    assert(app_wifi_join("candidate", "12345678901234567890123456789012345678901234567890123456789012345", 20000) == ESP_ERR_INVALID_ARG);
    assert(app_wifi_join(NULL, "password", 20000) == ESP_ERR_INVALID_ARG);
    assert(app_wifi_join("candidate", NULL, 20000) == ESP_ERR_INVALID_ARG);
    assert(set_calls == 0);

    wifi_scan_rec_t results[4];
    for (int failure = 0; failure < 4; failure++) {
        reset();
        fail_scan = failure == 0; fail_count = failure == 1;
        fail_alloc = failure == 2; fail_records = failure == 3;
        assert(app_wifi_scan(results, 4) == 0);
        assert(outstanding_scan == 0 && unlocks == 1);
    }
    reset();
    assert(app_wifi_scan(results, 4) == 2);
    assert(strcmp(results[0].ssid, "one") == 0 && strcmp(results[1].ssid, "two") == 0);
    assert(outstanding_scan == 0);
    reset();
    assert(app_wifi_scan(results, 1) == 1 && outstanding_scan == 0);
    assert(app_wifi_scan(NULL, 1) == 0);
#ifdef TEST_UDP_PORT
    uint16_t port = 0;
    assert(parse_udp_port("1", &port) && port == 1);
    assert(parse_udp_port("65535", &port) && port == 65535);
    const char *invalid[] = {"0", "65536", "-1", "1oops", "", " 123", "99999999999999999999"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        assert(!parse_udp_port(invalid[i], &port));
    }
    assert(!parse_udp_port(NULL, &port) && !parse_udp_port("1", NULL));
#endif
    puts("Wi-Fi production regression checks passed (18 scenarios + UDP port boundaries)");
    return 0;
}
