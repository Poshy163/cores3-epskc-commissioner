/*
 * Standalone Thread 1.4 ePSKc commissioner for the M5Stack CoreS3.
 *
 * Console commands:
 *   wifi <ssid> <password>   connect and persist to NVS
 *   status                   Wi-Fi / IP state
 *   join <addr> <port> <passcode>   run the ePSKc exchange
 *   reveal <0|1>             show or mask Network Key / PSKc in output
 *
 * Credentials are entered at runtime and never compiled in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "discover.h"
#include "matter.h"
#include "meshcop.h"
#include "otbr_rest.h"
#include "power.h"
#include "qrscan.h"
#include "settings.h"
#include "thread.h"
#include "ui.h"
#include "wifi_ctl.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "epskc";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_DISCONNECTED_BIT BIT2
#define WIFI_BACKBONE_READY_BIT BIT3

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_wifi_op_lock; /* serialises UI, console and reset callers */
static esp_netif_t *s_sta_netif;   /* backbone for the border router */
static bool s_reveal = false;
static char s_ip[16] = "-";
static char s_ip6[46] = "-";       /* backbone link-local, for RS on the infra link */
static char s_ssid[33] = "";
static int s_retries;
static bool s_wifi_hold;   /* true after an intentional leave: no auto-reconnect */

/*
 * ---------------- reconnect supervisor ----------------
 *
 * The disconnect handler used to stop calling esp_wifi_connect() after five
 * consecutive failures, set WIFI_FAIL_BIT and leave it there. Nothing anywhere
 * re-armed it. Five attempts against an absent access point take about two
 * seconds, so any outage longer than that -- an AP rebooting, a firmware
 * upgrade, a channel change, a band steer -- stranded the border router off
 * the backbone permanently, with perfectly good credentials still in NVS. The
 * board was found in exactly that state after 93 hours of uptime, and a
 * console re-entry of the same credentials fixed it instantly.
 *
 * A mains-powered border router must keep trying for as long as it holds
 * credentials. The bounded behaviour is still what app_wifi_join() needs,
 * though: an interactive "wrong password" has to be reported within its
 * timeout rather than retried forever. So the bounded path stays, scoped to a
 * join in flight, and the unbounded backoff owns every other disconnect.
 */
static esp_timer_handle_t s_reconnect_timer;
static volatile bool s_join_active;           /* app_wifi_join() owns the retries */
static volatile bool s_supervisor_on = true;  /* `link supervisor off` for testing */
static volatile bool s_have_credentials;
static volatile int s_backoff_step;
static volatile uint32_t s_disconnect_count;
static volatile uint8_t s_last_disc_reason;
static volatile int64_t s_last_disc_us;
static volatile int64_t s_last_conn_us;

/*
 * Fast at first so a momentary blip never reaches the user, then backing off
 * to a one-minute ceiling the device can sustain indefinitely. The ceiling
 * matters more than the ramp: the failure being fixed is "gave up forever",
 * not "reconnected slowly".
 */
static const uint32_t k_backoff_ms[] = { 0, 0, 0, 1000, 2000, 5000, 15000, 30000, 60000 };

static const char *wifi_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:            return "unspecified";
    case WIFI_REASON_AUTH_EXPIRE:            return "auth expired";
    case WIFI_REASON_AUTH_LEAVE:             return "deauthenticated";
    case WIFI_REASON_ASSOC_EXPIRE:           return "assoc expired";
    case WIFI_REASON_ASSOC_TOOMANY:          return "AP full";
    case WIFI_REASON_NOT_AUTHED:             return "not authenticated";
    case WIFI_REASON_NOT_ASSOCED:            return "not associated";
    case WIFI_REASON_ASSOC_LEAVE:            return "we left";
    case WIFI_REASON_ASSOC_NOT_AUTHED:       return "assoc without auth";
    case WIFI_REASON_MIC_FAILURE:            return "MIC failure";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "group rekey timeout";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:     return "IE mismatch in 4-way";
    case WIFI_REASON_802_1X_AUTH_FAILED:     return "802.1X auth failed";
    case WIFI_REASON_MISSING_ACKS:           return "missing ACKs";
    case WIFI_REASON_STA_LEAVING:            return "station leaving";
    case WIFI_REASON_TIMEOUT:                return "timeout";
    case WIFI_REASON_PEER_INITIATED:         return "peer disassociated us";
    case WIFI_REASON_AP_INITIATED:           return "AP disassociated us";
    case WIFI_REASON_INVALID_PMKID:          return "invalid PMKID";
    case WIFI_REASON_BEACON_TIMEOUT:         return "beacon timeout (AP gone?)";
    case WIFI_REASON_NO_AP_FOUND:            return "no AP found";
    case WIFI_REASON_AUTH_FAIL:              return "auth failed (wrong password?)";
    case WIFI_REASON_ASSOC_FAIL:             return "assoc failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "handshake timeout";
    case WIFI_REASON_CONNECTION_FAIL:        return "connection failed";
    case WIFI_REASON_AP_TSF_RESET:           return "AP TSF reset";
    case WIFI_REASON_ROAMING:                return "roaming";
    case WIFI_REASON_SA_QUERY_TIMEOUT:       return "SA query timeout";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "no AP with matching security";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "AP below authmode threshold";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:     return "AP below RSSI threshold";
    default:                                 return "see esp_wifi_types.h";
    }
}

static uint32_t backoff_delay_ms(void)
{
    const int last = (int) (sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0])) - 1;
    int step = s_backoff_step;
    return k_backoff_ms[step > last ? last : step];
}

static void cancel_reconnect(void)
{
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);   /* INVALID_STATE if idle; harmless */
    }
    s_backoff_step = 0;
}

/* Arm the next attempt. Never gives up while credentials are stored. */
static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL) {
        /* No timer: fall back to the old bounded behaviour rather than
         * silently doing nothing. */
        if (s_retries < 5) {
            s_retries++;
            esp_wifi_connect();
        }
        return;
    }
    esp_timer_stop(s_reconnect_timer);
    const uint32_t delay_ms = backoff_delay_ms();
    if (s_backoff_step < 1000) {
        s_backoff_step++;
    }
    /* esp_timer rejects a zero timeout, so the immediate retries get 1 us. */
    esp_err_t err = esp_timer_start_once(s_reconnect_timer, (uint64_t) delay_ms * 1000 + 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not arm the Wi-Fi reconnect timer: %s", esp_err_to_name(err));
    }
}

static void reconnect_timer_cb(void *arg)
{
    (void) arg;
    if (!s_supervisor_on || s_wifi_hold || s_join_active) {
        return;   /* the join path or a deliberate leave owns the radio */
    }
    if (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* A rejected call raises no disconnect event, so nothing else would
         * schedule the next attempt. Re-arm here or the supervisor stalls. */
        ESP_LOGW(TAG, "reconnect attempt %d rejected (%s); retrying",
                 s_backoff_step, esp_err_to_name(err));
        schedule_reconnect();
    }
}

/*
 * Belt and braces for the ladder itself: an event lost, a timer that failed to
 * arm, esp_wifi_connect() accepted and then silently dropped. Runs on its own
 * periodic esp_timer rather than on the UI's battery timer, because app_main
 * deliberately tolerates ui_init() failing and continues headless -- hanging
 * the supervisor's only safety net off the display stack would make recovery
 * contingent on the most failure-prone part of boot.
 *
 * Being on the esp_timer task rather than the LVGL task, this can also read
 * the driver's real configuration instead of trusting a cached flag, so
 * credentials changed behind the application's back stay accounted for.
 */
static esp_timer_handle_t s_watchdog_timer;
#define WIFI_WATCHDOG_PERIOD_US (15 * 1000 * 1000)
/* A hold that outlives this much downtime with credentials still in flash is
 * a stuck hold, not a deliberate one: app_wifi_leave() erases the credentials
 * it holds down for, so the two cases cannot be confused. */
#define WIFI_HOLD_RECOVERY_US   (120 * 1000 * 1000)

static void wifi_watchdog_cb(void *arg)
{
    (void) arg;
    if (!s_supervisor_on || s_join_active || s_wifi_events == NULL) {
        return;
    }
    if (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) {
        return;
    }

    wifi_config_t cfg = { 0 };
    const bool stored = esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK &&
                        cfg.sta.ssid[0] != '\0';
    s_have_credentials = stored;
    if (!stored) {
        return;   /* nothing to reconnect to */
    }

    if (s_wifi_hold) {
        /* The only path that leaves a hold set with credentials still in
         * flash is a rollback whose restore write failed. Recovering beats
         * staying dark forever on a border router. */
        const int64_t down_us = esp_timer_get_time() - s_last_disc_us;
        if (s_last_disc_us == 0 || down_us < WIFI_HOLD_RECOVERY_US) {
            return;
        }
        ESP_LOGW(TAG, "clearing a Wi-Fi hold stuck for %llds with credentials stored",
                 (long long) (down_us / 1000000));
        s_wifi_hold = false;
    }

    if (s_reconnect_timer != NULL && esp_timer_is_active(s_reconnect_timer)) {
        return;
    }
    ESP_LOGW(TAG, "Wi-Fi down with no attempt pending; re-arming");
    schedule_reconnect();
}

/*
 * Console log policy. A person types on this console, and at INFO the
 * OpenThread port, the CHIP exchange layer and the Wi-Fi driver bury every
 * prompt under router advertisements and subscription reports. Boot with
 * every tag at WARN and this application's own tags at INFO; `log info`
 * brings the full firehose back for a debugging session.
 */
static const char *const k_app_log_tags[] = {
    "epskc", "thread", "matter", "otbr_rest", "meshcop", "discover", "qrscan",
    "ui", "power", "settings",
};
static const char *s_log_profile = "quiet";

static void apply_quiet_log_profile(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);   /* also drops per-tag overrides */
    for (size_t i = 0; i < sizeof(k_app_log_tags) / sizeof(k_app_log_tags[0]); i++) {
        esp_log_level_set(k_app_log_tags[i], ESP_LOG_INFO);
    }
    matter_set_log_verbosity(ESP_LOG_WARN);
    s_log_profile = "quiet";
}

static const struct {
    const char *name;
    esp_log_level_t level;
} k_log_levels[] = {
    { "none", ESP_LOG_NONE }, { "error", ESP_LOG_ERROR }, { "warn", ESP_LOG_WARN },
    { "info", ESP_LOG_INFO }, { "debug", ESP_LOG_DEBUG }, { "verbose", ESP_LOG_VERBOSE },
};

static int cmd_log(int argc, char **argv)
{
    if (argc == 1) {
        printf("profile : %s\n", s_log_profile);
        printf("usage   : log quiet | log <none|error|warn|info|debug|verbose> [tag]\n");
        printf("          quiet = warnings and errors only, this app's tags at info\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "quiet") == 0) {
        apply_quiet_log_profile();
        printf("log profile: quiet\n");
        return 0;
    }
    const char *tag = argc == 3 ? argv[2] : "*";
    for (size_t i = 0; argc <= 3 && i < sizeof(k_log_levels) / sizeof(k_log_levels[0]); i++) {
        if (strcmp(argv[1], k_log_levels[i].name) != 0) {
            continue;
        }
        if ((int) k_log_levels[i].level > CONFIG_LOG_MAXIMUM_LEVEL) {
            printf("note: this build compiles logging out above level %d;"
                   " rebuild with a higher CONFIG_LOG_MAXIMUM_LEVEL to see %s\n",
                   CONFIG_LOG_MAXIMUM_LEVEL, k_log_levels[i].name);
        }
        esp_log_level_set(tag, k_log_levels[i].level);
        /* CHIP gates its categories itself; "chip" or any chip[XX] tag
         * adjusts that gate as well as the ESP tag level. */
        if (strcmp(tag, "*") == 0 || strncmp(tag, "chip", 4) == 0) {
            matter_set_log_verbosity(k_log_levels[i].level);
        }
        if (strcmp(tag, "*") == 0) {
            s_log_profile = k_log_levels[i].name;
        }
        printf("%s -> %s\n", tag, k_log_levels[i].name);
        return 0;
    }
    printf("usage: log quiet | log <none|error|warn|info|debug|verbose> [tag]\n");
    return 1;
}

void app_wifi_set_power_profile(bool external_power)
{
    static int s_applied = -1;
    const int wanted = external_power ? 1 : 0;
    if (wanted == s_applied) {
        return;
    }
    esp_err_t err = esp_wifi_set_ps(external_power ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not change Wi-Fi power save: %s", esp_err_to_name(err));
        return;
    }
    s_applied = wanted;
    ESP_LOGI(TAG, "Wi-Fi power save %s", external_power ? "off (external power)" : "on (battery)");
}

void app_apply_device_name(const char *name)
{
    esp_err_t err = thread_set_border_router_name(name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MeshCoP rename waits for the next boot: %s", esp_err_to_name(err));
    }
    err = matter_set_border_router_name(name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Matter rename waits for the next boot: %s", esp_err_to_name(err));
    }
}

static void refresh_wifi_ui(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", (char *) ap.ssid);
    }
    ui_set_wifi(s_ssid, s_ip);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        /*
         * The border router sends Router Solicitations on the backbone to learn
         * the LAN's on-link prefixes, and that needs a link-local address on
         * this netif. ESP-IDF only creates one on request, so without this
         * OpenThread logs "Failed to send RS" every few seconds and never sees
         * the infra prefixes. Link-local needs no DHCP, so claim it as soon as
         * the link is up rather than waiting for GOT_IP.
         */
        esp_err_t err = esp_netif_create_ip6_linklocal(s_sta_netif);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "no backbone IPv6 link-local: %s", esp_err_to_name(err));
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *) data;
        if (d != NULL) {
            s_last_disc_reason = d->reason;
        }
        s_disconnect_count++;
        s_last_disc_us = esp_timer_get_time();
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_BACKBONE_READY_BIT);
        strcpy(s_ip, "-");
        strcpy(s_ip6, "-");
        ui_set_wifi(NULL, NULL);
        if (s_wifi_hold) {
            /* deliberate leave: stay down */
            cancel_reconnect();
        } else if (s_join_active || !s_supervisor_on) {
            /* Bounded, on purpose. app_wifi_join() must be able to report a
             * bad password inside its timeout, and `link supervisor off`
             * reproduces the old give-up behaviour so the fix can be shown to
             * do something. */
            if (s_retries < 5) {
                s_retries++;
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            }
        } else {
            schedule_reconnect();
        }
        /* Acknowledge only after the handler has consumed s_wifi_hold. The
         * config-switching task can then release the hold without racing this
         * event into an unwanted reconnect. */
        xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *) data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_last_conn_us = esp_timer_get_time();
        s_have_credentials = true;
        cancel_reconnect();
        refresh_wifi_ui();
        xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_BACKBONE_READY_BIT);
        /* Backbone is up, so the border router can start and (if credentials
         * are stored) re-attach to its network. Idempotent on reconnects. */
        if (!ui_defer_br_start(s_sta_netif)) {
            thread_start_border_router(s_sta_netif);   /* headless fallback */
        }
        /* Idempotent, so reconnects are harmless. */
        if (settings_get()->rest_api) {
            if (!otbr_rest_request_running(true)) {
                ESP_LOGE(TAG, "could not queue REST start after Wi-Fi connect");
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        /* The Thread netif raises this too, so only record the backbone's. */
        ip_event_got_ip6_t *e = (ip_event_got_ip6_t *) data;
        if (e->esp_netif == s_sta_netif) {
            snprintf(s_ip6, sizeof(s_ip6), IPV6STR, IPV62STR(e->ip6_info.ip));
            ESP_LOGI(TAG, "backbone IPv6 %s", s_ip6);
            xEventGroupSetBits(s_wifi_events, WIFI_BACKBONE_READY_BIT);
            /* IPv4 is not a prerequisite for Thread border routing.  Start on
             * a usable backbone IPv6 address too; the operation is idempotent
             * if the IPv4 event already queued it. */
            if (!ui_defer_br_start(s_sta_netif)) {
                thread_start_border_router(s_sta_netif);
            }
        }
    }
}

/* Stop the current station attempt without letting the disconnect handler
 * immediately reconnect it. Waiting for the event closes the race where the
 * handler could otherwise observe s_wifi_hold=false after a config switch. */
static void wifi_disconnect_for_config_switch(void)
{
    s_wifi_hold = true;
    xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_DISCONNECTED_BIT,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
        if (!(bits & WIFI_DISCONNECTED_BIT)) {
            ESP_LOGW(TAG, "timed out waiting for Wi-Fi disconnect event");
        }
    } else {
        /* Common when a failed candidate has already exhausted its retries. */
        ESP_LOGD(TAG, "Wi-Fi already disconnected (%s)", esp_err_to_name(err));
    }
}

esp_err_t app_wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (ssid == NULL || pass == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wifi_op_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_wifi_op_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "another Wi-Fi operation is still running");
        return ESP_ERR_TIMEOUT;
    }
    /* Take the retries back off the supervisor for the duration: this caller
     * is waiting on a verdict and must get one inside timeout_ms. */
    s_join_active = true;
    cancel_reconnect();

    /*
     * Test the candidate in RAM only. With WIFI_STORAGE_FLASH every
     * esp_wifi_set_config() lands in NVS immediately, so the old code
     * overwrote known-good credentials with an untested guess and left them
     * overwritten for the whole 20-second trial. A power cut, a crash or a
     * reset anywhere in that window destroyed the working credentials for
     * good -- which is exactly what "the Wi-Fi credentials do not survive a
     * reboot" looks like from the outside. Switching storage to RAM keeps
     * flash holding the last configuration that actually worked until this
     * one has proved itself.
     */
    esp_err_t storage_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "could not switch Wi-Fi storage to RAM (%s); the candidate "
                 "will be written to flash before it is tested",
                 esp_err_to_name(storage_err));
    }

    /* Snapshot the known-good config so a typo does not strand a device that
     * was already provisioned, even within this call. */
    wifi_config_t previous = { 0 };
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &previous);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot snapshot current Wi-Fi config: %s", esp_err_to_name(err));
        goto done;
    }
    const bool had_previous = previous.sta.ssid[0] != '\0';

    wifi_config_t cfg = { 0 };
    strncpy((char *) cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *) cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);

    /* Stop the old station attempt before replacing its config. IDF can reject
     * esp_wifi_set_config() with ESP_ERR_WIFI_STATE while STA is connecting.
     * Clear result bits after the disconnect acknowledgement so a late GOT_IP
     * from the old AP cannot make the candidate look successful. */
    wifi_disconnect_for_config_switch();
    xEventGroupClearBits(s_wifi_events,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not stage Wi-Fi config for \"%s\": %s",
                 ssid, esp_err_to_name(err));
        goto rollback;
    }

    s_retries = 0;
    s_wifi_hold = false;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not start Wi-Fi candidate \"%s\": %s",
                 ssid, esp_err_to_name(err));
        goto rollback;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        /* Proved itself: only now commit it to flash. */
        if (esp_wifi_set_storage(WIFI_STORAGE_FLASH) == ESP_OK) {
            esp_err_t commit_err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
            if (commit_err != ESP_OK) {
                ESP_LOGE(TAG, "\"%s\" is connected but could not be saved: %s",
                         ssid, esp_err_to_name(commit_err));
                err = commit_err;
                goto done;
            }
        }
        ESP_LOGI(TAG, "Wi-Fi candidate \"%s\" connected and saved", ssid);
        s_have_credentials = true;
        err = ESP_OK;
        goto done;
    }

    err = ESP_FAIL;
    ESP_LOGW(TAG, "Wi-Fi candidate \"%s\" failed%s; rolling back",
             ssid, (bits & WIFI_FAIL_BIT) ? " after retries" : " by timeout");

rollback:
    {
        wifi_disconnect_for_config_switch();
        wifi_config_t empty = { 0 };
        wifi_config_t *restore = had_previous ? &previous : &empty;
        esp_err_t restore_err = esp_wifi_set_config(WIFI_IF_STA, restore);
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi rollback failed: %s", esp_err_to_name(restore_err));
            /* The driver still holds the rejected candidate, so do not
             * reconnect it. Flash was never touched, so a reboot recovers --
             * and so does the watchdog, which clears a hold that outlives two
             * minutes with credentials still stored. */
            s_wifi_hold = true;
            goto done;
        }

        s_retries = 0;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        if (had_previous) {
            s_wifi_hold = false;
            s_have_credentials = true;
            esp_err_t reconnect_err = esp_wifi_connect();
            if (reconnect_err == ESP_OK) {
                ESP_LOGI(TAG, "restored previous Wi-Fi config for \"%.*s\"",
                         (int) sizeof(previous.sta.ssid),
                         (const char *) previous.sta.ssid);
            } else {
                ESP_LOGW(TAG, "previous Wi-Fi config restored but reconnect failed: %s",
                         esp_err_to_name(reconnect_err));
            }
        } else {
            /* First-time provisioning failed. The candidate never reached
             * flash, so there is nothing to erase; leave the station
             * intentionally idle. */
            s_wifi_hold = true;
            s_have_credentials = false;
            ESP_LOGI(TAG, "failed first Wi-Fi candidate discarded; none was stored");
        }
    }
done:
    /* Every exit restores flash-backed storage, so the next successful
     * association and the boot-time load behave normally. Idempotent. */
    if (esp_wifi_set_storage(WIFI_STORAGE_FLASH) != ESP_OK) {
        ESP_LOGE(TAG, "could not restore flash-backed Wi-Fi storage");
    }
    /* Hand the radio back to the supervisor before releasing the lock. */
    s_join_active = false;
    xSemaphoreGive(s_wifi_op_lock);
    return err;
}

esp_err_t app_wifi_leave(void)
{
    if (s_wifi_op_lock == NULL || xSemaphoreTake(s_wifi_op_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Wi-Fi operation lock unavailable; credentials not changed");
        return ESP_ERR_INVALID_STATE;
    }
    /* As with replacement, the driver can reject set_config while STA is
     * connecting. Stop and acknowledge the old attempt before erasing it. */
    wifi_disconnect_for_config_switch();
    wifi_config_t cfg = { 0 };
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg); /* zeroed = erased */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not erase Wi-Fi credentials: %s", esp_err_to_name(err));
    } else {
        s_have_credentials = false;
    }
    cancel_reconnect();
    xSemaphoreGive(s_wifi_op_lock);
    return err;
}

int app_wifi_scan(wifi_scan_rec_t *out, int max)
{
    if (out == NULL || max <= 0 || s_wifi_op_lock == NULL ||
        xSemaphoreTake(s_wifi_op_lock, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    int m = 0;
    wifi_ap_record_t *recs = NULL;
    wifi_scan_config_t sc = { 0 };   /* all channels, active scan */
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        goto done;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        goto done;
    }
    recs = calloc(n, sizeof(*recs));
    if (recs == NULL) {
        goto done;
    }
    esp_wifi_scan_get_ap_records(&n, recs);   /* already strongest-first */

    for (int i = 0; i < n && m < max; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < m && !dup; j++) {
            dup = strcmp(out[j].ssid, (const char *) recs[i].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        snprintf(out[m].ssid, sizeof(out[m].ssid), "%s", (const char *) recs[i].ssid);
        out[m].rssi = recs[i].rssi;
        out[m].secured = recs[i].authmode != WIFI_AUTH_OPEN;
        m++;
    }
done:
    free(recs);
    xSemaphoreGive(s_wifi_op_lock);
    return m;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: wifi <ssid> <password>\n");
        return 1;
    }
    printf("connecting to \"%s\"...\n", argv[1]);
    if (app_wifi_join(argv[1], argv[2], 20000) == ESP_OK) {
        printf("connected, ip=%s (saved to NVS)\n", s_ip);
        return 0;
    }
    printf("failed to connect\n");
    return 1;
}

static int cmd_status(int argc, char **argv)
{
    (void) argc; (void) argv;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("wifi   : \"%s\" rssi %d", (char *) ap.ssid, ap.rssi);
        if (s_last_conn_us != 0) {
            printf(", associated %llds",
                   (long long) ((esp_timer_get_time() - s_last_conn_us) / 1000000));
        }
        printf("\n");
    } else {
        printf("wifi   : not connected\n");
    }
    /* The stored SSID is what separates "credentials were lost" from
     * "credentials are fine but we are not using them", which is the whole
     * question when a border router turns up offline. */
    wifi_config_t stored_cfg = { 0 };
    if (esp_wifi_get_config(WIFI_IF_STA, &stored_cfg) == ESP_OK &&
        stored_cfg.sta.ssid[0] != '\0') {
        printf("stored : \"%.*s\" in NVS\n", (int) sizeof(stored_cfg.sta.ssid),
               (const char *) stored_cfg.sta.ssid);
    } else {
        printf("stored : no credentials\n");
    }
    printf("link   : %lu disconnect(s)", (unsigned long) s_disconnect_count);
    if (s_disconnect_count != 0) {
        printf(", last %u (%s) %llds ago", (unsigned) s_last_disc_reason,
               wifi_reason_name(s_last_disc_reason),
               (long long) ((esp_timer_get_time() - s_last_disc_us) / 1000000));
    }
    printf("\n");
    printf("retry  : supervisor %s, step %d", s_supervisor_on ? "on" : "OFF",
           s_backoff_step);
    if (s_reconnect_timer != NULL && esp_timer_is_active(s_reconnect_timer)) {
        printf(", attempt pending");
    } else if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
        printf(", idle");
    }
    printf("\n");
    printf("ip     : %s\n", s_ip);
    printf("ipv6   : %s\n", s_ip6);
    printf("matter : %s (endpoint %u, window %s)\n",
           matter_status_string(matter_get_status()),
           (unsigned) matter_get_thread_border_router_endpoint_id(),
           matter_is_commissioning_window_open() ? "open" : "closed");
    printf("lvgl st: %u bytes free (min)\n", (unsigned) ui_lvgl_stack_free());
    printf("heap   : internal %u free, largest %u, min ever %u; PSRAM %u free\n",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    int64_t up_s = esp_timer_get_time() / 1000000;
    printf("uptime : %lldd %02lldh %02lldm %02llds\n",
           (long long) (up_s / 86400), (long long) ((up_s / 3600) % 24),
           (long long) ((up_s / 60) % 60), (long long) (up_s % 60));
    printf("reveal : %s\n", s_reveal ? "on" : "off");
    printf("fw     : %s\n", FW_VERSION);
    return 0;
}

/*
 * Wi-Fi link diagnostics. Deliberately a separate command rather than
 * subcommands of `wifi`, which would shadow any network actually called
 * "drop" or "supervisor".
 */
static int cmd_link(int argc, char **argv)
{
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "drop") == 0) {
        /*
         * Simulates an access-point outage. A single disconnect proves
         * nothing while the AP is in range, because the reconnect succeeds and
         * resets the retry budget. Issuing several faster than the station can
         * re-associate is what reproduces a real outage: each one lands as a
         * failed attempt. Six is enough to exhaust the old five-attempt
         * budget, which is the exact state the board was found stranded in.
         */
        long count = 1;
        if (argc == 3) {
            char *end = NULL;
            count = strtol(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' || count < 1 || count > 20) {
                printf("usage: link drop [1..20]\n");
                return 1;
            }
        }
        for (long i = 0; i < count; i++) {
            esp_err_t err = esp_wifi_disconnect();
            printf("drop %ld/%ld: %s\n", i + 1, count, esp_err_to_name(err));
            if (i + 1 < count) {
                vTaskDelay(pdMS_TO_TICKS(150));
            }
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "supervisor") == 0) {
        if (strcmp(argv[2], "on") == 0) {
            s_supervisor_on = true;
        } else if (strcmp(argv[2], "off") == 0) {
            s_supervisor_on = false;
            cancel_reconnect();
        } else {
            printf("usage: link supervisor <on|off>\n");
            return 1;
        }
        printf("reconnect supervisor %s (not persisted; on at every boot)\n",
               s_supervisor_on ? "on" : "off");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "retry") == 0) {
        s_backoff_step = 0;
        schedule_reconnect();
        printf("immediate reconnect attempt scheduled\n");
        return 0;
    }
    if (argc != 1) {
        printf("usage: link [drop [n]|retry|supervisor <on|off>]\n");
        return 1;
    }
    printf("state     : %s\n",
           (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) ? "connected"
           : s_wifi_hold ? "held down (deliberate leave)" : "disconnected");
    printf("credential: %s\n", s_have_credentials ? "stored" : "none");
    printf("supervisor: %s, backoff step %d, next delay %lu ms\n",
           s_supervisor_on ? "on" : "off", s_backoff_step,
           (unsigned long) backoff_delay_ms());
    printf("pending   : %s\n",
           (s_reconnect_timer != NULL && esp_timer_is_active(s_reconnect_timer))
           ? "yes" : "no");
    printf("disconnects: %lu\n", (unsigned long) s_disconnect_count);
    if (s_disconnect_count != 0) {
        printf("last reason: %u (%s), %llds ago\n", (unsigned) s_last_disc_reason,
               wifi_reason_name(s_last_disc_reason),
               (long long) ((esp_timer_get_time() - s_last_disc_us) / 1000000));
    }
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void) argc; (void) argv;
    printf("rebooting\n");
    app_reboot();
    return 0;
}

static int cmd_reveal(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: reveal <0|1>\n");
        return 1;
    }
    s_reveal = (argv[1][0] == '1');
    printf("secrets will be %s\n", s_reveal ? "SHOWN" : "masked");
    return 0;
}

static int cmd_matter(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        matter_runtime_status_t status = matter_get_status();
        printf("state   : %s\n", matter_status_string(status));
        printf("endpoint: %u (Thread Border Router 0x0091)\n",
               (unsigned) matter_get_thread_border_router_endpoint_id());
        printf("fabric  : %s\n", matter_is_commissioned() ? "commissioned" : "not commissioned");
        printf("window  : %s\n", matter_is_commissioning_window_open() ? "open" : "closed");
        if (status == MATTER_RUNTIME_ERROR) {
            printf("error   : %s\n", esp_err_to_name(matter_get_last_error()));
        }
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "code") == 0) {
        char qr[MATTER_QR_CODE_BUFFER_SIZE];
        char manual[MATTER_MANUAL_CODE_BUFFER_SIZE];
        esp_err_t err = matter_get_onboarding_codes(qr, sizeof(qr), manual, sizeof(manual));
        if (err != ESP_OK) {
            printf("pairing code unavailable: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("manual  : %s\n", manual);
        printf("QR data : %s\n", qr);
        return 0;
    }

    if ((argc == 2 || argc == 3) && strcmp(argv[1], "open") == 0) {
        unsigned long seconds = 900;
        if (argc == 3) {
            char *end = NULL;
            seconds = strtoul(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' || seconds > UINT16_MAX) {
                printf("usage: matter open [180..900]\n");
                return 1;
            }
        }
        esp_err_t err = matter_open_commissioning_window((uint16_t) seconds);
        if (err != ESP_OK) {
            printf("could not open Matter window: %s\n", esp_err_to_name(err));
            return 1;
        }
        for (int i = 0; i < 20 && !matter_is_commissioning_window_open(); i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        printf("Matter commissioning window %s for %lu seconds\n",
               matter_is_commissioning_window_open() ? "open" : "requested", seconds);
        return 0;
    }

    printf("usage: matter [status|code|open [180..900]]\n");
    return 1;
}

/*
 * Console commands that reach OpenThread's spinel layer, or run the DTLS
 * handshake, need far more stack than the REPL task has. The join path alone
 * peaks around 23 KB against a 12 KB console stack, which is a silent overflow
 * rather than an error. They all run on the 28 KB UI worker instead, the same
 * way the touch UI and the REST handlers do.
 */
struct fetch_args {
    const char *addr;
    uint16_t port;
    const char *passcode;
    uint8_t *dataset;
    size_t cap;
    size_t len;
    esp_err_t result;
};

static void do_fetch_dataset(void *arg)
{
    struct fetch_args *a = arg;
    a->result = meshcop_fetch_dataset(a->addr, a->port, a->passcode,
                                      a->dataset, a->cap, &a->len);
}

struct tlv_args {
    const uint8_t *tlvs;
    size_t len;
    esp_err_t result;
};

static void do_thread_join(void *arg)
{
    struct tlv_args *a = arg;
    a->result = thread_join(a->tlvs, a->len);
}

struct form_args {
    uint8_t channel;
    char name[17];
    int ch;
    uint16_t pan;
    esp_err_t result;
};

static void do_form_network(void *arg)
{
    struct form_args *a = arg;
    a->result = thread_form_network(a->channel, a->name, sizeof(a->name),
                                    &a->ch, &a->pan);
}

struct share_args {
    char code[10];
    uint32_t lifetime_ms;
    esp_err_t result;
};

static void do_share_start(void *arg)
{
    struct share_args *a = arg;
    a->result = thread_share_start(a->code, sizeof(a->code), a->lifetime_ms);
}

struct share_stop_args {
    bool done;
};

static void do_share_stop(void *arg)
{
    struct share_stop_args *a = arg;
    thread_share_stop();
    a->done = true;
}

static void do_thread_forget(void *arg)
{
    *(esp_err_t *) arg = thread_forget();
}

/* Never fall back to the 6 KB console stack: these calls are known to need
 * much more, and an unavailable/busy worker is safer than a delayed panic. */
static bool run_off_console(ui_worker_fn fn, void *arg, uint32_t timeout_ms)
{
    if (ui_run_on_worker(fn, arg, timeout_ms)) {
        return true;
    }
    ESP_LOGE(TAG, "operation worker unavailable; command was not run");
    return false;
}

static int cmd_join(int argc, char **argv)
{
    if (argc != 4) {
        printf("usage: join <addr> <port> <passcode>\n");
        printf("  e.g. join 192.168.1.59 49156 874213905\n");
        return 1;
    }
    if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
        printf("not on wifi yet - run 'wifi <ssid> <pass>' first\n");
        return 1;
    }

    /* static: keeps the console task's stack for the handshake itself */
    static uint8_t dataset[512];
    size_t dlen = 0;

    ui_clear_dataset();
    ui_set_status(UI_STATE_BUSY, "Connecting...");

    struct fetch_args fa = {
        .addr = argv[1], .port = (uint16_t) atoi(argv[2]), .passcode = argv[3],
        .dataset = dataset, .cap = sizeof(dataset), .result = ESP_FAIL,
    };
    if (!run_off_console(do_fetch_dataset, &fa, 60000)) {
        ui_set_status(UI_STATE_ERROR, "Device busy");
        printf("operation worker unavailable - try again\n");
        return 1;
    }
    esp_err_t err = fa.result;
    dlen = fa.len;
    if (err != ESP_OK) {
        ui_set_status(UI_STATE_ERROR, "Failed");
        printf("\n>>> FAILED <<<\n");
        return 1;
    }

    char name[17];
    int channel = 0;
    uint16_t panid = 0;
    meshcop_summarize(dataset, dlen, name, sizeof(name), &channel, &panid);
    ui_set_status(UI_STATE_OK, "Credentials received");
    ui_show_dataset(name, channel, panid);

    printf("\n--- ACTIVE OPERATIONAL DATASET (%u bytes) ---\n", (unsigned) dlen);
    meshcop_print_tlvs(dataset, dlen, s_reveal);
    printf(">>> DATASET RETRIEVED <<<\n");

    /* Same as the touch flow: apply it and actually attach. */
    struct tlv_args ja = { .tlvs = dataset, .len = dlen, .result = ESP_FAIL };
    if (!run_off_console(do_thread_join, &ja, 30000)) {
        printf(">>> dataset retrieved, but worker could not apply it <<<\n");
        return 1;
    }
    if (ja.result == ESP_OK) {
        for (int i = 0; i < 40 && !thread_attached(); i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        printf(">>> THREAD ROLE: %s <<<\n", thread_role());
    } else {
        printf(">>> thread attach not attempted (radio unavailable) <<<\n");
    }
    return 0;
}

static const char *topology_status_name(thread_topology_status_t status)
{
    switch (status) {
    case THREAD_TOPOLOGY_NEVER:    return "not scanned";
    case THREAD_TOPOLOGY_SCANNING: return "scanning";
    case THREAD_TOPOLOGY_COMPLETE: return "complete";
    case THREAD_TOPOLOGY_PARTIAL:  return "partial";
    case THREAD_TOPOLOGY_FAILED:   return "failed";
    default:                       return "unknown";
    }
}

static int cmd_thread(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "scan") != 0)) {
        printf("usage: thread [scan]\n");
        return 1;
    }

    thread_topology_t topology;
    thread_topology_get(&topology);
    if (argc == 2 && topology.status != THREAD_TOPOLOGY_SCANNING) {
        esp_err_t err = thread_topology_refresh();
        if (err != ESP_OK) {
            printf("topology scan could not start: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("topology scan started; waiting for mesh replies...\n");
    }
    if (argc == 2) {
        for (int i = 0; i < 70; i++) {
            thread_topology_get(&topology);
            if (topology.status != THREAD_TOPOLOGY_SCANNING) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    thread_activity_t activity;
    bool have_activity = thread_activity_get(&activity);
    printf("role       : %s\n", have_activity ? activity.role : thread_role());
    printf("credentials: %s\n", thread_has_dataset() ? "stored" : "none");
    printf("br boot    : %s\n", thread_border_router_status());
    char rcp[128];
    if (thread_rcp_version(rcp, sizeof(rcp))) {
        printf("rcp        : %s\n", rcp);
    }
    if (have_activity) {
        printf("attached   : %s", activity.attached ? "yes" : "no");
        if (activity.attached) {
            printf(" (%lu seconds)", (unsigned long) activity.attach_duration_s);
        }
        printf("\n");
        if (activity.parent_valid) {
            printf("parent     : 0x%04x, ", activity.parent_rloc16);
            if (activity.parent_rssi_valid) {
                printf("%d dBm, ", activity.parent_rssi);
            } else {
                printf("signal pending, ");
            }
            printf("LQI %u\n", activity.parent_lqi);
        }
        printf("routers    : %u known locally\n", activity.known_routers);
        if (activity.direct_children_valid) {
            printf("children   : %u directly attached here\n", activity.direct_children);
        }
        printf("radio      : TX %lu, RX %lu frames since start\n",
               (unsigned long) activity.mac_tx_total,
               (unsigned long) activity.mac_rx_total);
        printf("IPv6       : TX %lu ok/%lu fail, RX %lu ok/%lu fail\n",
               (unsigned long) activity.ip_tx_success,
               (unsigned long) activity.ip_tx_failure,
               (unsigned long) activity.ip_rx_success,
               (unsigned long) activity.ip_rx_failure);
        printf("parent moves: %u\n", activity.parent_changes);
    }
    thread_topology_get(&topology);
    printf("topology   : %s\n", topology_status_name(topology.status));
    if (topology.status == THREAD_TOPOLOGY_COMPLETE ||
        topology.status == THREAD_TOPOLOGY_PARTIAL) {
        printf("mesh seen  : %u routers, %u children, %u border routers"
               " (%lu devices%s)\n",
               topology.routers_seen, topology.children_seen,
               topology.border_routers_seen,
               (unsigned long) topology.routers_seen + topology.children_seen,
               topology.includes_self ? ", including this one" : "");
    }
    return 0;
}

static int cmd_forget(int argc, char **argv)
{
    (void) argc; (void) argv;
    esp_err_t ferr = ESP_FAIL;
    if (!run_off_console(do_thread_forget, &ferr, 30000)) {
        printf("operation worker unavailable - try again\n");
        return 1;
    }
    if (ferr != ESP_OK) {
        printf("failed to erase\n");
        return 1;
    }
    ui_forget_network();
    printf("Thread credentials erased\n");
    return 0;
}

static int cmd_camtest(int argc, char **argv)
{
    (void) argc; (void) argv;
    printf("internal heap free : %u (largest block %u)\n",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("DMA-capable free   : %u (largest block %u)\n",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    printf("PSRAM free         : %u\n",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* The UI's QR job may own the camera. Driving it from here as well tears
     * the decoder down under an in-flight poll, so report its stats instead. */
    if (qrscan_active()) {
        printf("UI scan in progress - observing, not driving\n");
        for (int i = 0; i < 10; i++) {
            uint8_t mn, mx, mean;
            int cand;
            qrscan_last_frame_stats(&mn, &mx, &mean, &cand);
            printf("  luma min=%3u max=%3u mean=%3u  qr_candidates=%d\n",
                   mn, mx, mean, cand);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        return 0;
    }

    /*
     * Run it on the UI worker, which already has the stack quirc needs. A
     * dedicated task is not an option: internal RAM is exhausted enough that
     * the largest free block is ~11 KB, well under what the decoder wants.
     */
    if (!ui_run_camtest()) {
        printf("camtest unavailable (UI worker not running)\n");
        return 1;
    }
    return 0;
}

static void do_apply_name(void *arg)
{
    app_apply_device_name((const char *) arg);
}

static int cmd_name(int argc, char **argv)
{
    if (argc < 2 || strlen(argv[1]) > SETTINGS_DEVICE_NAME_MAX_LEN) {
        printf("usage: name <border-router-name>   (max 32 chars)\n");
        return 1;
    }
    esp_err_t err = settings_set_device_name(argv[1]);
    if (err != ESP_OK) {
        printf("could not save name: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (run_off_console(do_apply_name, argv[1], 20000)) {
        printf("Matter/MeshCoP name set to \"%s\"\n", argv[1]);
    } else {
        printf("name saved; it is published after the next reboot\n");
    }
    return 0;
}

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

void app_reboot(void)
{
    ESP_LOGW(TAG, "reboot requested");
    vTaskDelay(pdMS_TO_TICKS(200));   /* let the console/UI flush */
    esp_restart();
}

void app_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");
    /* Fail closed: never reboot and claim a reset while a credential store
     * reports failure. A retry can finish any stores already cleared. */
    if (thread_forget() != ESP_OK) {
        ESP_LOGE(TAG, "factory reset aborted: Thread credentials remain");
        ui_set_status(UI_STATE_ERROR, "Factory reset failed - Thread data remains");
        return;
    }
    if (app_wifi_leave() != ESP_OK) {
        ESP_LOGE(TAG, "factory reset aborted: Wi-Fi credentials remain");
        ui_set_status(UI_STATE_ERROR, "Factory reset failed - try again");
        return;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open("epskc", NVS_READWRITE, &h);   /* remembered network */
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    esp_err_t settings_err = err == ESP_OK ? settings_erase() : ESP_FAIL;
    if (err != ESP_OK || settings_err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset aborted: NVS erase failed (network=%s settings=%s)",
                 esp_err_to_name(err), esp_err_to_name(settings_err));
        ui_set_status(UI_STATE_ERROR, "Factory reset incomplete - try again");
        return;
    }
    /* Matter owns a separate NVS partition.  Its factory-reset path removes
     * every fabric and CHIP key, then schedules the reboot. */
    esp_err_t matter_err = matter_factory_reset();
    if (matter_err == ESP_ERR_INVALID_STATE) {
        /* Matter never started, so there are no live CHIP users of the
         * partition. Erase it directly and use the normal reboot path. */
        matter_err = nvs_flash_erase_partition("matter_nvs");
        if (matter_err == ESP_OK) {
            app_reboot();
            return;
        }
    }
    if (matter_err != ESP_OK) {
        ESP_LOGE(TAG, "Matter factory reset incomplete: %s", esp_err_to_name(matter_err));
        ui_set_status(UI_STATE_ERROR, "Factory reset incomplete - Matter data remains");
    }
    /* On success esp_matter::factory_reset() reboots asynchronously. */
}

static int cmd_rest(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        if (!otbr_rest_request_running(false)) {
            printf("could not queue REST stop\n");
            return 1;
        }
        settings_get()->rest_api = false;
        settings_save();
        printf("REST API stop requested\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "epskc") == 0) {
        if (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0) {
            printf("usage: rest epskc <on|off>\n");
            return 1;
        }
        bool on = strcmp(argv[2], "on") == 0;
        if (!otbr_rest_request_epskc(on)) {
            printf("could not queue ba-epskc change\n");
            return 1;
        }
        settings_get()->rest_epskc = on;
        settings_save();
        printf("ba-epskc endpoint change requested: %s\n", on ? "on" : "off");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "start") == 0) {
        if (!otbr_rest_request_running(true)) {
            printf("could not queue REST start\n");
            return 1;
        }
        settings_get()->rest_api = true;
        settings_save();
        printf("REST API start requested\n");
        return 0;
    }
    printf("REST API : %s\n", otbr_rest_running() ? "running" : "stopped");
    printf("URL      : http://%s:%d\n", s_ip, OTBR_REST_PORT);
    printf("ba-epskc : %s\n", settings_get()->rest_epskc ? "registered" : "removed");
    return 0;
}

static int cmd_power(int argc, char **argv)
{
    (void) argc; (void) argv;
    power_status_t ps;
    if (power_read(&ps) != ESP_OK) {
        printf("PMIC unreachable\n");
        return 1;
    }
    printf("battery  : %d%%%s  %d mV  %s%s\n",
           ps.percent, ps.percent_estimated ? " est" : "", ps.batt_mv,
           ps.present ? "" : "(not detected) ", ps.charge_detail);
    printf("vbus     : %s  %d mV\n", ps.vbus ? "present" : "absent", ps.vbus_mv);
    printf("vsys     : %d mV\n", ps.vsys_mv);
    printf("pmic temp: %d C\n", ps.pmic_temp_c);
    float t;
    if (power_esp_temp(&t)) {
        printf("esp temp : %.1f C\n", t);
    }
    float rate;
    int mins;
    if (power_discharge_rate(&rate, &mins)) {
        printf("discharge: %.1f %%/h  ~%dh%02dm left\n", rate, mins / 60, mins % 60);
    } else {
        printf("discharge: measuring (needs 10 min on battery)\n");
    }
    return 0;
}

static int cmd_settings(int argc, char **argv)
{
    (void) argc; (void) argv;
    const settings_t *c = settings_get();
    printf("brightness   : %u%%\n", c->brightness);
    printf("sleep        : %s\n", c->sleep_idx == 0 ? "30 s" : c->sleep_idx == 1 ? "1 min"
                                   : c->sleep_idx == 2 ? "5 min" : "never");
    printf("prefer router: %s\n", c->prefer_router ? "yes" : "no");
    if (c->new_net_channel) {
        printf("new-net chan : %u\n", c->new_net_channel);
    } else {
        printf("new-net chan : auto\n");
    }
    printf("share minutes: %u\n", c->share_minutes);
    printf("rest api     : %s\n", c->rest_api ? "on" : "off");
    printf("rest epskc   : %s\n", c->rest_epskc ? "on" : "off");
    return 0;
}

static int cmd_share(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        struct share_stop_args a = { 0 };
        if (!run_off_console(do_share_stop, &a, 20000) || !a.done) {
            printf("operation worker unavailable - key state unchanged\n");
            return 1;
        }
        printf("ephemeral key stopped\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "state") == 0) {
        printf("share: %s\n", thread_share_state());
        return 0;
    }
    char code[10];
    struct share_args sa = {
        .lifetime_ms = settings_get()->share_minutes * 60000u, .result = ESP_FAIL,
    };
    if (!run_off_console(do_share_start, &sa, 30000)) {
        printf("operation worker unavailable - try again\n");
        return 1;
    }
    esp_err_t err = sa.result;
    snprintf(code, sizeof(code), "%s", sa.code);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("border router not running yet\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("could not start ephemeral key\n");
        return 1;
    }
    printf("ephemeral key: %s  (%u min, single use)\n", code, settings_get()->share_minutes);
    printf("state: %s\n", thread_share_state());
    return 0;
}

static int cmd_newnet(int argc, char **argv)
{
    (void) argc; (void) argv;
    char name[17];
    int ch = 0;
    uint16_t pan = 0;
    struct form_args na = { .channel = settings_get()->new_net_channel, .result = ESP_FAIL };
    if (!run_off_console(do_form_network, &na, 40000)) {
        printf("operation worker unavailable - try again\n");
        return 1;
    }
    if (na.result != ESP_OK) {
        printf("failed to create network\n");
        return 1;
    }
    snprintf(name, sizeof(name), "%s", na.name);
    ch = na.ch;
    pan = na.pan;
    printf("forming \"%s\"  ch %d  pan 0x%04x\n", name, ch, pan);
    for (int i = 0; i < 30 && !thread_attached(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    printf("role: %s\n", thread_role());
    return 0;
}

static int cmd_batt(int argc, char **argv)
{
    (void) argc; (void) argv;
    power_status_t ps;
    if (power_read(&ps) != ESP_OK) {
        printf("PMIC unreachable\n");
        return 1;
    }
    if (!ps.present) {
        printf("no battery detected (vbus: %s, vbat %d mV)\n",
               ps.vbus ? "present" : "absent", ps.batt_mv);
    } else {
        printf("battery : %d%%  (%d mV)\n", ps.percent, ps.batt_mv);
        printf("state   : %s  vbus: %s\n",
               ps.charging ? "charging" : ps.full ? "full" : "discharging",
               ps.vbus ? "present" : "absent");
    }
    printf("raw     : st1=0x%02x st2=0x%02x\n", ps.raw_st1, ps.raw_st2);
    return 0;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "newnet", .help = "form a new Thread network with fresh credentials", .func = cmd_newnet },
        { .command = "rest", .help = "rest [start|stop|epskc on|off] - OTBR REST API", .func = cmd_rest },
        { .command = "power", .help = "voltages, temperatures, discharge rate", .func = cmd_power },
        { .command = "settings", .help = "show persisted settings", .func = cmd_settings },
        { .command = "share", .help = "share [stop|state] - ePSKc code so a commissioner can pull our credentials", .func = cmd_share },
        { .command = "batt", .help = "battery %, voltage and charge state", .func = cmd_batt },
        { .command = "name", .help = "name <name> - set the Matter/MeshCoP border-router name", .func = cmd_name },
        { .command = "camtest", .help = "start the camera and try 30 frames", .func = cmd_camtest },
        { .command = "thread", .help = "thread [scan] - activity and best-effort mesh topology", .func = cmd_thread },
        { .command = "matter", .help = "matter [status|code|open [seconds]]", .func = cmd_matter },
        { .command = "forget", .help = "erase stored Thread credentials", .func = cmd_forget },
        { .command = "wifi", .help = "wifi <ssid> <password>", .func = cmd_wifi },
        { .command = "status", .help = "show wifi/ip state", .func = cmd_status },
        { .command = "reveal", .help = "reveal <0|1> - unmask Network Key / PSKc", .func = cmd_reveal },
        { .command = "join", .help = "join <addr> <port> <passcode>", .func = cmd_join },
        { .command = "log", .help = "log [quiet|<level> [tag]] - console log verbosity", .func = cmd_log },
        { .command = "link", .help = "link [drop|retry|supervisor on|off] - Wi-Fi link diagnostics", .func = cmd_link },
        { .command = "reboot", .help = "restart the device", .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void app_main(void)
{
    apply_quiet_log_profile();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings_load();

    /* Display first, so boot progress is visible on the LCD rather than
     * leaving the screen dark until something happens. */
    if (ui_init() != ESP_OK) {
        ESP_LOGW(TAG, "continuing headless - console still works");
    }
    ui_set_status(UI_STATE_BUSY, "Starting...");


    s_wifi_events = xEventGroupCreate();
    s_wifi_op_lock = xSemaphoreCreateMutex();
    if (s_wifi_events == NULL || s_wifi_op_lock == NULL) {
        ESP_LOGE(TAG, "could not allocate Wi-Fi synchronisation primitives");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    if (otbr_rest_control_init() != ESP_OK) {
        ESP_LOGE(TAG, "REST controls unavailable");
    }
    /* Created before the Wi-Fi driver starts, so the very first disconnect --
     * an AP that is not up yet at boot -- already has a supervisor to fall
     * back on rather than burning the five retries and stopping. */
    const esp_timer_create_args_t reconnect_args = {
        .callback = reconnect_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconnect",
    };
    if (esp_timer_create(&reconnect_args, &s_reconnect_timer) != ESP_OK) {
        ESP_LOGE(TAG, "no Wi-Fi reconnect timer; falling back to bounded retries");
        s_reconnect_timer = NULL;
    }
    const esp_timer_create_args_t watchdog_args = {
        .callback = wifi_watchdog_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_watchdog",
    };
    if (esp_timer_create(&watchdog_args, &s_watchdog_timer) == ESP_OK) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_watchdog_timer, WIFI_WATCHDOG_PERIOD_US));
    } else {
        ESP_LOGE(TAG, "no Wi-Fi watchdog timer; the reconnect ladder has no backstop");
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_GOT_IP6, &on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH)); /* reuse saved creds */
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Modem sleep adds a DTIM interval of latency to every packet, which is
     * the wrong trade for a border router other devices route through. Keep
     * the radio awake on USB power; the UI's battery timer switches profiles
     * as the power source changes. Without a PMIC reading, assume mains. */
    power_status_t boot_power;
    app_wifi_set_power_profile(power_read(&boot_power) != ESP_OK || boot_power.vbus);

    /* Cached so the watchdog, which runs on the LVGL task, never has to call
     * into the Wi-Fi driver while the display lock is held. */
    wifi_config_t boot_cfg = { 0 };
    s_have_credentials = esp_wifi_get_config(WIFI_IF_STA, &boot_cfg) == ESP_OK &&
                         boot_cfg.sta.ssid[0] != '\0';
    ESP_LOGI(TAG, "stored Wi-Fi credentials at boot: %s",
             s_have_credentials ? "yes" : "no");

    /* Compile-time credentials (menuconfig -> "ePSKc Commissioner"), applied
     * only when nothing is stored: a console-provisioned device keeps its
     * runtime credentials across reflashes. */
    if (CONFIG_EPSKC_WIFI_SSID[0] != '\0') {
        wifi_config_t stored = { 0 };
        if (esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK &&
            stored.sta.ssid[0] == '\0') {
            wifi_config_t cfg = { 0 };
            strncpy((char *) cfg.sta.ssid, CONFIG_EPSKC_WIFI_SSID,
                    sizeof(cfg.sta.ssid) - 1);
            strncpy((char *) cfg.sta.password, CONFIG_EPSKC_WIFI_PASSWORD,
                    sizeof(cfg.sta.password) - 1);
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
            esp_wifi_disconnect();
            esp_wifi_connect();
            ESP_LOGI(TAG, "using compile-time Wi-Fi credentials for \"%s\"",
                     CONFIG_EPSKC_WIFI_SSID);
        }
    }

    /* mDNS must be up before the border agent attaches, otherwise publishing
     * _meshcop._udp fails and the device never appears as a border router.
     * Starting it lazily on the first scan was too late. */
    if (discover_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed - discovery and meshcop publishing will not work");
    }

    /* Matter owns the one OpenThread host task, exposes this appliance as the
     * standard Matter Thread Border Router device type, and uses the existing
     * application-controlled Wi-Fi connection as its infrastructure link. */
    if (matter_init() != ESP_OK) {
        ESP_LOGE(TAG, "Matter startup failed - Matter and Thread unavailable");
    }

    /* Adopt Matter's OpenThread instance; never launch a second radio task. */
    if (thread_init() != ESP_OK) {
        ESP_LOGW(TAG, "OpenThread unavailable - credential retrieval still works");
    }
    thread_set_prefer_router(settings_get()->prefer_router);
    thread_epskc_set_feature_enabled(settings_get()->rest_epskc);
    /* A saved AP can deliver GOT_IP before the RCP finishes initializing. The
     * event's early BR job then correctly skips, so retry once the Thread stack
     * is authoritative; both paths are idempotent if the event was slower. */
    if (xEventGroupGetBits(s_wifi_events) & WIFI_BACKBONE_READY_BIT) {
        if (!ui_defer_br_start(s_sta_netif)) {
            thread_start_border_router(s_sta_netif);
        }
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rcfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rcfg.prompt = "epskc>";
    rcfg.max_cmdline_length = 256;
    /*
     * 6 KB. Every deep command -- join, newnet, share, forget -- now runs on
     * the UI worker, so nothing on this task goes anywhere near the DTLS or
     * spinel paths. The reclaimed internal RAM is what lets the camera find a
     * contiguous 8 KB DMA buffer.
     */
    rcfg.task_stack_size = 6144;

    esp_console_dev_usb_serial_jtag_config_t ucfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    /* Largest block, not just total: the REPL stack has to be contiguous, and
     * this allocation is the first thing to fail when internal RAM tightens. */
    ESP_LOGI(TAG, "internal heap before console: %u free, largest block %u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    /* The console is a convenience; the touch UI is the product. If it cannot
     * start, log it and carry on rather than aborting the boot. */
    esp_err_t cerr = esp_console_new_repl_usb_serial_jtag(&ucfg, &rcfg, &repl);
    if (cerr == ESP_OK) {
        ESP_ERROR_CHECK(esp_console_register_help_command());
        register_commands();
        cerr = esp_console_start_repl(repl);
    }
    if (cerr != ESP_OK) {
        ESP_LOGE(TAG, "console unavailable (%s) - touch UI still active",
                 esp_err_to_name(cerr));
    }

    /* Probe the camera only after the console and network stacks have settled.
     * STREAMOFF retains the V4L2 buffers and decoder but intentionally releases
     * the DVP controller, including its 7680-byte DMA staging block. The compact
     * Wi-Fi buffer profile keeps enough contiguous internal RAM available when
     * a later QR scan recreates the controller. */
    if (qrscan_start() == ESP_OK) {
        qrscan_stop();
    } else {
        ESP_LOGW(TAG, "camera unavailable - QR scanning disabled");
    }
    /* Start the REST lifecycle task after the camera probe; its queue already
     * captured any early GOT_IP. Its stack and HTTPD's stay in internal RAM so
     * flash-backed operations remain cache-safe. */
    if (otbr_rest_control_start() != ESP_OK) {
        ESP_LOGE(TAG, "REST lifecycle task unavailable");
    }

    ui_set_status(UI_STATE_IDLE, "Ready");
    ESP_LOGI(TAG, "Thread 1.4 ePSKc commissioner ready");
}
