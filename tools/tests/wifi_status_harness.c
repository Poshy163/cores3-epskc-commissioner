#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wifi_ctl.h"

typedef int portMUX_TYPE;
typedef int esp_event_base_t;
typedef int esp_netif_t;
typedef void *esp_timer_handle_t;
typedef struct { char ssid[33]; int rssi; uint8_t primary; } wifi_ap_record_t;
typedef struct { uint16_t reason; } wifi_event_sta_disconnected_t;
typedef struct { struct { struct { unsigned part[4]; } ip; } ip_info; } ip_event_got_ip_t;
typedef struct {
    esp_netif_t *esp_netif;
    struct { struct { unsigned part[8]; } ip; } ip6_info;
} ip_event_got_ip6_t;
enum { WIFI_EVENT = 1, IP_EVENT, WIFI_EVENT_STA_START, WIFI_EVENT_STA_CONNECTED,
       WIFI_EVENT_STA_DISCONNECTED, IP_EVENT_STA_GOT_IP, IP_EVENT_GOT_IP6 };
#define WIFI_CONNECTED_BIT 1u
#define WIFI_FAIL_BIT 2u
#define WIFI_DISCONNECTED_BIT 4u
#define WIFI_BACKBONE_READY_BIT 8u
#define portMUX_INITIALIZER_UNLOCKED 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (ip)->part[0], (ip)->part[1], (ip)->part[2], (ip)->part[3]
#define IPV6STR "%x:%x:%x:%x:%x:%x:%x:%x"
#define IPV62STR(ip) (ip).part[0], (ip).part[1], (ip).part[2], (ip).part[3], \
                    (ip).part[4], (ip).part[5], (ip).part[6], (ip).part[7]

static int critical_depth, critical_entries;
static void enter_critical(portMUX_TYPE *lock) {
    (void)lock; assert(critical_depth == 0); ++critical_depth; ++critical_entries;
}
static void exit_critical(portMUX_TYPE *lock) {
    (void)lock; assert(critical_depth == 1); --critical_depth;
}
#define portENTER_CRITICAL(lock) enter_critical(lock)
#define portEXIT_CRITICAL(lock) exit_critical(lock)
static int64_t now_us;
static int s_wifi_events = 1, s_retries;
static unsigned event_bits;
static bool s_wifi_hold, s_join_active, s_have_credentials;
static atomic_bool s_supervisor_on = true;
static atomic_int s_backoff_step;
static esp_netif_t netif;
static esp_netif_t *s_sta_netif = &netif;
static esp_timer_handle_t s_reconnect_timer;
static char s_ip[16] = "-", s_ip6[46] = "-";
static bool timer_pending, radio_failure;
static int radio_queries, driver_connects, config_reads;
static wifi_ap_record_t radio = { .ssid = "private-network", .rssi = -63, .primary = 6 };
static void (*during_radio_query)(void);
static struct { bool rest_api; } settings = { true };
static int64_t esp_timer_get_time(void) { assert(!critical_depth); return now_us; }
static bool esp_timer_is_active(esp_timer_handle_t timer) {
    assert(!critical_depth && timer); return timer_pending;
}
static esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) {
    assert(!critical_depth); ++radio_queries; *out = radio;
    if (during_radio_query) {
        void (*callback)(void) = during_radio_query;
        during_radio_query = NULL; callback();
    }
    return radio_failure ? ESP_FAIL : ESP_OK;
}
static esp_err_t esp_wifi_connect(void) { assert(!critical_depth); ++driver_connects; return ESP_OK; }
static esp_err_t esp_netif_create_ip6_linklocal(esp_netif_t *target) {
    assert(!critical_depth && target == s_sta_netif); return ESP_OK;
}
static void xEventGroupClearBits(int group, unsigned bits) {
    assert(!critical_depth && group == s_wifi_events); event_bits &= ~bits;
}
static void xEventGroupSetBits(int group, unsigned bits) {
    assert(!critical_depth && group == s_wifi_events); event_bits |= bits;
}
static void ui_set_wifi(const char *ssid, const char *ip) {
    assert(!critical_depth && !ssid && !ip);
}
static void refresh_wifi_ui(void) { assert(!critical_depth); }
static bool ui_defer_br_start(esp_netif_t *target) {
    assert(!critical_depth && target == s_sta_netif); return true;
}
static void thread_start_border_router(esp_netif_t *target) {
    assert(!critical_depth && target == s_sta_netif);
}
static void cancel_reconnect(void) { assert(!critical_depth); s_backoff_step = 0; timer_pending = false; }
static void schedule_reconnect(void) { assert(!critical_depth); ++s_backoff_step; timer_pending = true; }
#define settings_get() (&settings)
static bool otbr_rest_request_running(bool on) { assert(!critical_depth && on); return true; }

#include "wifi_status_production.inc"

static void disconnected(void) {
    wifi_event_sta_disconnected_t event = {201};
    on_wifi(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
}
static void connected(void) { on_wifi(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL); }
static void got_ip(void) {
    ip_event_got_ip_t event = { .ip_info = { .ip = { .part = {10, 0, 0, 7} } } };
    on_wifi(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &event);
}
static void reconnect_during_query(void) {
    disconnected(); connected(); got_ip();
    radio.rssi = -81; radio.primary = 11;
}

int main(void) {
    app_wifi_status_t status;
    memset(&status, 0xa5, sizeof(status));
    app_wifi_get_status(NULL);
    app_wifi_get_status(&status);
    assert(!status.connected && !status.signal_valid && !status.association_valid);
    assert(!status.last_disconnect_valid && status.disconnect_count == 0);
    assert(status.supervisor_on && status.retry_step == 0 && !status.reconnect_pending);
    assert(!strcmp(status.ipv4, "-") && !strcmp(status.ipv6, "-"));
    assert(radio_queries == 0);

    /* An association at uptime zero is valid; waiting for DHCP does not reset
     * its duration or invent a previous disconnect. */
    connected(); app_wifi_get_status(&status);
    assert(status.connected && status.signal_valid && status.association_valid);
    assert(status.associated_ms == 0 && status.rssi == -63 && status.channel == 6);
    assert(!status.last_disconnect_valid && !strcmp(status.ipv4, "-"));
    now_us = 5000000; got_ip(); app_wifi_get_status(&status);
    assert(status.associated_ms == 5000 && !strcmp(status.ipv4, "10.0.0.7"));
    ip_event_got_ip6_t ip6 = {
        .esp_netif = s_sta_netif, .ip6_info = { .ip = { .part = {0xfd00, 0, 0, 0, 0, 0, 0, 7} } }
    };
    on_wifi(NULL, IP_EVENT, IP_EVENT_GOT_IP6, &ip6);
    app_wifi_get_status(&status);
    assert(!strcmp(status.ipv6, "fd00:0:0:0:0:0:0:7"));
    ip6.esp_netif = NULL; ip6.ip6_info.ip.part[7] = 99;
    on_wifi(NULL, IP_EVENT, IP_EVENT_GOT_IP6, &ip6);
    app_wifi_get_status(&status);
    assert(!strcmp(status.ipv6, "fd00:0:0:0:0:0:0:7")); // Thread interface excluded

    now_us = 10000000; disconnected(); app_wifi_get_status(&status);
    assert(!status.connected && !status.signal_valid && !status.association_valid);
    assert(status.disconnect_count == 1 && status.last_disconnect_valid);
    assert(status.last_disconnect_age_ms == 0 && status.last_disconnect_reason == 201);
    assert(!strcmp(status.ipv4, "-") && !strcmp(status.ipv6, "-"));
    now_us = 15000000; s_supervisor_on = false; s_backoff_step = 5;
    s_reconnect_timer = (void *)1; timer_pending = true;
    app_wifi_get_status(&status);
    assert(!status.supervisor_on && status.retry_step == 5 && status.reconnect_pending);
    assert(status.last_disconnect_age_ms == 5000 && !status.association_valid);

    now_us = 20000000; connected(); radio_failure = true;
    app_wifi_get_status(&status);
    assert(status.connected && status.association_valid && !status.signal_valid);
    radio_failure = false;
    /* A disconnect while the SDK returns an old AP record must win. */
    now_us = 25000000; during_radio_query = disconnected;
    app_wifi_get_status(&status);
    assert(!status.connected && !status.signal_valid && !status.association_valid);
    assert(status.disconnect_count == 2 && status.last_disconnect_age_ms == 0);
    assert(!strcmp(status.ipv4, "-") && !strcmp(status.ipv6, "-"));

    /* A disconnect/reconnect inside one query cannot borrow the old RSSI.
     * The following stable query can then report the new connection. */
    connected(); during_radio_query = reconnect_during_query;
    app_wifi_get_status(&status);
    assert(status.connected && status.association_valid && !status.signal_valid);
    assert(status.disconnect_count == 3 && !strcmp(status.ipv4, "10.0.0.7"));
    now_us += 1000000; const int connect_before = driver_connects;
    app_wifi_get_status(&status);
    assert(status.signal_valid && status.rssi == -81 && status.channel == 11);
    assert(status.associated_ms == 1000 && status.last_disconnect_age_ms == 1000);
    assert(config_reads == 0 && driver_connects == connect_before && critical_depth == 0);
    assert(critical_entries > 0);
    puts("PASS production Wi-Fi telemetry: event ages, IP ownership, signal validity, racing reconnects and lock boundaries");
    return 0;
}
