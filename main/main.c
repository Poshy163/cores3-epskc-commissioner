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
#include <string.h>

#include "esp_console.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "discover.h"
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

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_sta_netif;   /* backbone for the border router */
static bool s_reveal = false;
static char s_ip[16] = "-";
static char s_ssid[33] = "";
static int s_retries;
static bool s_wifi_hold;   /* true after an intentional leave: no auto-reconnect */

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
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        strcpy(s_ip, "-");
        ui_set_wifi(NULL, NULL);
        if (s_wifi_hold) {
            /* deliberate leave: stay down */
        } else if (s_retries < 5) {
            s_retries++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *) data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        refresh_wifi_ui();
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        /* Backbone is up, so the border router can start and (if credentials
         * are stored) re-attach to its network. Idempotent on reconnects. */
        if (!ui_defer_br_start(s_sta_netif)) {
            thread_start_border_router(s_sta_netif);   /* headless fallback */
        }
        /* Idempotent, so reconnects are harmless. */
        if (settings_get()->rest_api) {
            otbr_rest_start();
        }
    }
}

esp_err_t app_wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    wifi_config_t cfg = { 0 };
    strncpy((char *) cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *) cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);

    s_wifi_hold = false;
    s_retries = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    esp_wifi_disconnect();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

void app_wifi_leave(void)
{
    s_wifi_hold = true;
    wifi_config_t cfg = { 0 };
    esp_wifi_set_config(WIFI_IF_STA, &cfg);   /* zeroed = credentials erased */
    esp_wifi_disconnect();
}

int app_wifi_scan(wifi_scan_rec_t *out, int max)
{
    wifi_scan_config_t sc = { 0 };   /* all channels, active scan */
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return 0;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        return 0;
    }
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (recs == NULL) {
        return 0;
    }
    esp_wifi_scan_get_ap_records(&n, recs);   /* already strongest-first */

    int m = 0;
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
    free(recs);
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
        printf("wifi   : \"%s\" rssi %d\n", (char *) ap.ssid, ap.rssi);
    } else {
        printf("wifi   : not connected\n");
    }
    printf("ip     : %s\n", s_ip);
    printf("reveal : %s\n", s_reveal ? "on" : "off");
    printf("fw     : %s\n", FW_VERSION);
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

static void do_thread_forget(void *arg)
{
    *(esp_err_t *) arg = thread_forget();
}

/* Falls back to running inline when the worker is unavailable (headless
 * boot); better a deep call than no call, and the UI is absent anyway. */
static bool run_off_console(ui_worker_fn fn, void *arg, uint32_t timeout_ms)
{
    if (ui_run_on_worker(fn, arg, timeout_ms)) {
        return true;
    }
    ESP_LOGW(TAG, "worker unavailable; running inline on the console stack");
    fn(arg);
    return true;
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
    run_off_console(do_fetch_dataset, &fa, 60000);
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
    run_off_console(do_thread_join, &ja, 30000);
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

static int cmd_thread(int argc, char **argv)
{
    (void) argc; (void) argv;
    printf("role       : %s\n", thread_role());
    printf("credentials: %s\n", thread_has_dataset() ? "stored" : "none");
    extern const char *g_br_boot_status;
    printf("br boot    : %s\n", g_br_boot_status);
    int8_t rssi;
    if (thread_link_rssi(&rssi)) {
        printf("parent rssi: %d dBm\n", rssi);
    }
    return 0;
}

static int cmd_forget(int argc, char **argv)
{
    (void) argc; (void) argv;
    esp_err_t ferr = ESP_FAIL;
    run_off_console(do_thread_forget, &ferr, 30000);
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

static int cmd_name(int argc, char **argv)
{
    if (argc < 2 || strlen(argv[1]) > 32) {
        printf("usage: name <mdns-hostname>   (max 32 chars)\n");
        return 1;
    }
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) {
        printf("nvs open failed\n");
        return 1;
    }
    nvs_set_str(h, "host", argv[1]);
    nvs_commit(h);
    nvs_close(h);
    printf("mDNS name set to \"%s\" - reboot to re-publish\n", argv[1]);
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
    thread_forget();
    app_wifi_leave();
    settings_erase();
    nvs_handle_t h;
    if (nvs_open("epskc", NVS_READWRITE, &h) == ESP_OK) {   /* remembered network */
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    app_reboot();
}

static int cmd_rest(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        otbr_rest_stop();
        settings_get()->rest_api = false;
        settings_save();
        printf("REST API stopped\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "epskc") == 0) {
        bool on = strcmp(argv[2], "on") == 0;
        settings_get()->rest_epskc = on;
        settings_save();
        thread_epskc_set_feature_enabled(on);
        otbr_rest_set_epskc(on);
        printf("ba-epskc endpoints %s\n", on ? "registered" : "removed");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "start") == 0) {
        settings_get()->rest_api = true;
        settings_save();
        if (otbr_rest_start() != ESP_OK) {
            printf("could not start (see log)\n");
            return 1;
        }
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
    printf("battery  : %d%%  %d mV  %s%s\n", ps.percent, ps.batt_mv,
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
        thread_share_stop();
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
    run_off_console(do_share_start, &sa, 30000);
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
    run_off_console(do_form_network, &na, 40000);
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
        { .command = "name", .help = "name <hostname> - set the mDNS/border-router name", .func = cmd_name },
        { .command = "camtest", .help = "start the camera and try 30 frames", .func = cmd_camtest },
        { .command = "thread", .help = "show Thread role and stored credentials", .func = cmd_thread },
        { .command = "forget", .help = "erase stored Thread credentials", .func = cmd_forget },
        { .command = "wifi", .help = "wifi <ssid> <password>", .func = cmd_wifi },
        { .command = "status", .help = "show wifi/ip state", .func = cmd_status },
        { .command = "reveal", .help = "reveal <0|1> - unmask Network Key / PSKc", .func = cmd_reveal },
        { .command = "join", .help = "join <addr> <port> <passcode>", .func = cmd_join },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void app_main(void)
{
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
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH)); /* reuse saved creds */
    ESP_ERROR_CHECK(esp_wifi_start());

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

    /* OpenThread must come up AFTER esp_netif_init() and the default event
     * loop: its netif attach registers event handlers and fails with
     * ESP_ERR_INVALID_STATE otherwise. Non-fatal -- without the radio this is
     * still a working commissioner, it just cannot attach to the network. */
    if (thread_init() != ESP_OK) {
        ESP_LOGW(TAG, "OpenThread unavailable - credential retrieval still works");
    }
    thread_set_prefer_router(settings_get()->prefer_router);
    thread_epskc_set_feature_enabled(settings_get()->rest_epskc);

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

    /*
     * Camera pipeline: after the console (its 12 KB REPL stack must win the
     * contiguous-memory race -- claimed first, the console silently fails to
     * start), before Wi-Fi/Thread bring-up finishes fragmenting the heap. The
     * pipeline then stays alive for the life of the process (see qrscan.c),
     * so this is a one-time claim of the 8 KB DVP DMA buffer.
     */
    if (qrscan_start() == ESP_OK) {
        qrscan_stop();
    } else {
        ESP_LOGW(TAG, "camera unavailable - QR scanning disabled");
    }

    ui_set_status(UI_STATE_IDLE, "Ready");
    ESP_LOGI(TAG, "Thread 1.4 ePSKc commissioner ready");
}
