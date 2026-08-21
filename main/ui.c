/*
 * Touch UI for the ePSKc commissioner.
 *
 * Flow:  MAIN --[Scan]--> LIST --[pick]--> KEYPAD --[Join]--> RESULT
 *
 * Scanning and joining run on a worker task: both block for seconds, and doing
 * them on the LVGL task would freeze the screen. LVGL buffers live in internal
 * RAM because PSRAM is disabled on this build.
 */
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "discover.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "meshcop.h"
#include "nvs.h"
#include "otbr_rest.h"
#include "power.h"
#include "qrscan.h"
#include "settings.h"
#include "thread.h"
#include "wifi_ctl.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mdns.h"

static const char *TAG = "ui";

/*
 * 10 lines = 6.4 KB, DMA-capable, internal RAM.
 *
 * A PSRAM buffer looks cheaper but is not: the SPI master then allocates an
 * internal bounce buffer per transfer, which fails under memory pressure and
 * kills the display mid-operation ("Failed to allocate priv TX buffer"). A
 * small internal buffer costs a fixed, affordable amount and never fails.
 */
#define LVGL_BUFFER_PIXELS (BSP_LCD_H_RES * 10)

/* Every panel sits below the title/wifi header and above a Back-button strip.
 * tools/layout_check.py models the same geometry -- keep the two in step. */
#define UI_HEADER_H   44
#define UI_PANEL_H    (BSP_LCD_V_RES - UI_HEADER_H)
#define UI_BACK_STRIP 56

#define COL_BG      0x101410
#define COL_PANEL   0x1B2119
#define COL_TEXT    0xE4E8DF
#define COL_DIM     0x8A937F
#define COL_ACCENT  0xD9904F
#define COL_OK      0x4ECBB8
#define COL_ERROR   0xF0798F

typedef enum {
    JOB_SCAN, JOB_JOIN, JOB_QR, JOB_WIFI_SCAN, JOB_WIFI_JOIN, JOB_CAMTEST,
    JOB_FORM, JOB_BR_START, JOB_SHARE, JOB_CALL
} job_t;

/* JOB_CALL payload; one at a time, guarded by s_call_busy. */
static ui_worker_fn s_call_fn;
static void *s_call_arg;
static SemaphoreHandle_t s_call_done;
static volatile bool s_call_busy;

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

/* Network details / share / dataset-QR screens. */
static lv_obj_t *panel_net, *panel_share, *panel_dsqr;
static lv_obj_t *lbl_net_info, *lbl_share_code, *lbl_share_state, *dsqr_widget;
static char s_share_code[10];
static int s_share_left_s;
static bool s_share_active;
/* Settings screens. */
static lv_obj_t *panel_settings, *panel_screen, *panel_power, *panel_tset, *panel_name, *panel_about;
static lv_obj_t *lbl_power_info, *lbl_about_info, *name_ta;
static lv_obj_t *s_sleep_shield;   /* full-screen tap catcher while asleep */
static bool s_asleep;
static int s_sample_tick;          /* batt timer ticks since last gauge sample */
static bool s_on_external_power;   /* latest VBUS reading, for keep-awake */
static char s_ip_str[16] = "-";

/* Backbone netif handed over for JOB_BR_START. */
static esp_netif_t *s_br_backbone;

/* Signals the console task that the worker has finished the camera selftest. */
static SemaphoreHandle_t s_camtest_done;

static lv_obj_t *panel_main, *panel_list, *panel_keypad, *panel_result, *panel_qr;
static void show_panel(lv_obj_t *p);
static bool confirm_tap(lv_obj_t *btn);
static lv_obj_t *panel_wifi, *panel_wpass;
static lv_obj_t *wifi_list_w, *lbl_wifi_hint, *lbl_wpass_ssid, *wpass_ta;
static wifi_scan_rec_t s_aps[12];
static int s_aps_n;
static char s_join_ssid[33];
static char s_join_pass[65];
static lv_obj_t *lbl_wifi, *lbl_status, *lbl_list_hint, *lbl_code, *lbl_target;
static lv_obj_t *lbl_result_title, *lbl_result_body, *ba_list;
static lv_obj_t *qr_canvas, *lbl_qr_hint, *lbl_network;
static lv_obj_t *batt_body, *batt_fill, *batt_nub, *lbl_batt;

/* Battery outline geometry. Interior width = BATT_W minus border and padding
 * on both sides; the fill bar is scaled against it. 62 wide because the worst
 * case, bolt + "100%", is ~48 px at 14 pt and must fit inside the outline. */
#define BATT_W       62
#define BATT_H       24
#define BATT_INNER_W (BATT_W - 8)
static uint16_t *s_preview;
static volatile bool s_qr_abort;

/* Last successfully retrieved network, persisted so it survives a reboot. */
#define NVS_NS "epskc"
static char s_net_name[17];
static int s_net_channel;
static uint16_t s_net_panid;
static bool s_have_net;

static void network_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "net_name", s_net_name);
    nvs_set_i32(h, "net_ch", s_net_channel);
    nvs_set_u16(h, "net_pan", s_net_panid);
    nvs_commit(h);
    nvs_close(h);
}

static void network_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_net_name);
    int32_t ch = 0;
    if (nvs_get_str(h, "net_name", s_net_name, &len) == ESP_OK) {
        nvs_get_i32(h, "net_ch", &ch);
        nvs_get_u16(h, "net_pan", &s_net_panid);
        s_net_channel = (int) ch;
        s_have_net = s_net_name[0] != '\0';
    }
    nvs_close(h);
}

static void refresh_network_label(void);
static void refresh_power_info(void);
static void refresh_about_info(void);
static void on_wake(lv_event_t *e);

/* Runs on the LVGL task. The battery lives on the Base DIN behind its power
 * switch, so the PMIC may legitimately see no battery at all -- hide the
 * outline entirely rather than show a bogus 0%. */
static void batt_timer_cb(lv_timer_t *t)
{
    (void) t;
    power_status_t ps;
    esp_err_t perr = power_read(&ps);
    if (perr == ESP_OK) {
        s_on_external_power = ps.vbus;
    }
    if (perr != ESP_OK || !ps.present || ps.percent < 0) {
        lv_obj_add_flag(batt_body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(batt_nub, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(batt_nub, LV_OBJ_FLAG_HIDDEN);

    /* One gauge sample a minute for the discharge estimate (timer is 10 s). */
    if (++s_sample_tick >= 6) {
        s_sample_tick = 0;
        power_note_sample(ps.percent, !ps.vbus);
    }

    /* Bolt whenever external power is present, not only while the charger is
     * actively topping up: a full battery on USB reported "no bolt", which
     * read as unplugged. */
    uint32_t col = ps.vbus ? COL_OK
                   : ps.percent < 15 ? COL_ERROR
                   : ps.percent < 40 ? COL_ACCENT
                                     : COL_DIM;
    lv_label_set_text_fmt(lbl_batt, "%s%d%%",
                          ps.vbus ? LV_SYMBOL_CHARGE : "", ps.percent);
    lv_obj_set_style_border_color(batt_body, lv_color_hex(col), LV_PART_MAIN);
    lv_obj_set_style_bg_color(batt_nub, lv_color_hex(col), LV_PART_MAIN);
    lv_obj_set_style_bg_color(batt_fill, lv_color_hex(col), LV_PART_MAIN);
    int w = (BATT_INNER_W * ps.percent) / 100;
    lv_obj_set_width(batt_fill, w < 2 ? 2 : w);
}

/* Runs on the LVGL task (lock already held), so the role on screen tracks
 * detached -> child even when attachment completes long after the join. */
static void role_timer_cb(lv_timer_t *t)
{
    (void) t;
    if (s_have_net) {
        refresh_network_label();
    }

    thread_apply_router_preference();

    /* Live pages refresh while visible. */
    if (panel_power && !lv_obj_has_flag(panel_power, LV_OBJ_FLAG_HIDDEN)) {
        refresh_power_info();
    }
    if (panel_about && !lv_obj_has_flag(panel_about, LV_OBJ_FLAG_HIDDEN)) {
        refresh_about_info();
    }

    /*
     * Screen sleep: backlight off after the configured idle time, and a
     * transparent full-screen catcher so the waking touch does not also land
     * on whatever is underneath. Never while a share code is showing.
     */
    uint32_t timeout = settings_sleep_ms[settings_get()->sleep_idx];
    bool sharing = panel_share && !lv_obj_has_flag(panel_share, LV_OBJ_FLAG_HIDDEN);
    bool held_awake = settings_get()->keep_awake_powered && s_on_external_power;
    if (timeout && !s_asleep && !sharing && !held_awake &&
        lv_display_get_inactive_time(NULL) > timeout) {
        s_sleep_shield = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_sleep_shield, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_opa(s_sleep_shield, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_sleep_shield, 0, LV_PART_MAIN);
        lv_obj_clear_flag(s_sleep_shield, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_sleep_shield, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_sleep_shield, on_wake, LV_EVENT_PRESSED, NULL);
        bsp_display_brightness_set(0);
        s_asleep = true;
    }
}

/* Caller must hold the LVGL lock. */
static void refresh_network_label(void)
{
    if (!lbl_network) {
        return;
    }
    char buf[128];
    if (s_have_net) {
        const char *role = thread_role();
        /* Parent link strength, when there is a parent to measure. */
        char sig[20] = "";
        int8_t rssi;
        if (thread_link_rssi(&rssi)) {
            snprintf(sig, sizeof(sig), "  %d dBm", rssi);
        }
        /* As router/leader, how many devices hang off us -- the thing you
         * actually want to know when validating joins against this network. */
        char kids[20] = "";
        if (strcmp(role, "leader") == 0 || strcmp(role, "router") == 0) {
            int n = thread_child_count();
            snprintf(kids, sizeof(kids), "  %d device%s", n, n == 1 ? "" : "s");
        }
        snprintf(buf, sizeof(buf), "%s  ch %d  pan 0x%04x\nthread: %s%s%s",
                 s_net_name, s_net_channel, s_net_panid, role, sig, kids);
        lv_obj_set_style_text_color(
            lbl_network,
            lv_color_hex(thread_attached() ? COL_OK : COL_ACCENT), LV_PART_MAIN);
    } else {
        snprintf(buf, sizeof(buf), "no credentials yet");
        lv_obj_set_style_text_color(lbl_network, lv_color_hex(COL_DIM), LV_PART_MAIN);
    }
    lv_label_set_text(lbl_network, buf);
}

static ba_entry_t s_found[BA_MAX];
static int s_found_n;
static ba_entry_t s_target;
static char s_code[10];
static QueueHandle_t s_jobs;
static bool s_ready;
static bool s_have_wifi;

/* ---------------- helpers ---------------- */

static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, uint32_t col, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(col), LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, BSP_LCD_H_RES - 20);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *mk_panel(void)
{
    lv_obj_t *p = lv_obj_create(lv_screen_active());
    lv_obj_set_size(p, BSP_LCD_H_RES, UI_PANEL_H);
    lv_obj_align(p, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(p, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 6, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static lv_obj_t *mk_button(lv_obj_t *p, const char *txt, lv_event_cb_t cb,
                           uint32_t col, int w, int h)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), LV_PART_MAIN);
    lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0x101410), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(l);
    return b;
}

/* Caller must already hold the LVGL lock. */
static void show_panel(lv_obj_t *p)
{
    lv_obj_add_flag(panel_main, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_keypad, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_result, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_wifi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_wpass, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_net, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_share, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_dsqr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_power, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_tset, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_about, LV_OBJ_FLAG_HIDDEN);
    if (panel_qr) {
        lv_obj_add_flag(panel_qr, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);
}

/* Caller must hold the LVGL lock. Refreshes the on-screen passcode field. */
static void redraw_code(void)
{
    char shown[16];
    size_t n = strlen(s_code);
    for (size_t i = 0; i < 9; i++) {
        shown[i] = i < n ? s_code[i] : '_';
    }
    shown[9] = '\0';
    lv_label_set_text(lbl_code, shown);
}

/* ---------------- event handlers ---------------- */

static void network_erase_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_net_name[0] = '\0';
    s_net_channel = 0;
    s_net_panid = 0;
    s_have_net = false;
}

void ui_forget_network(void)
{
    network_erase_nvs();
    if (s_ready) {
        bsp_display_lock(0);
        refresh_network_label();
        bsp_display_unlock();
    }
}

static void on_forget(lv_event_t *e)
{
    if (!confirm_tap(lv_event_get_target(e))) {
        return;
    }
    thread_forget();
    network_erase_nvs();
    refresh_network_label();
    lv_label_set_text(lbl_status, "Credentials cleared");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    show_panel(panel_main);
}

/* ---------------- network details / share / dataset QR ---------------- */

static void refresh_net_info(void)
{
    char buf[160];
    if (!s_have_net) {
        snprintf(buf, sizeof(buf), "No Thread network yet.\n\nScan for a router, or create one.");
    } else {
        const char *role = thread_role();
        int n = thread_child_count();
        int8_t rssi;
        char link[24] = "";
        if (thread_link_rssi(&rssi)) {
            snprintf(link, sizeof(link), "\nparent link %d dBm", rssi);
        }
        snprintf(buf, sizeof(buf), "%s\nchannel %d   pan 0x%04x\nrole: %s   %d device%s%s",
                 s_net_name, s_net_channel, s_net_panid, role, n, n == 1 ? "" : "s", link);
    }
    lv_label_set_text(lbl_net_info, buf);
}

static void on_net_open(lv_event_t *e)
{
    (void) e;
    refresh_net_info();
    show_panel(panel_net);
}

static void on_share_open(lv_event_t *e)
{
    (void) e;
    if (!thread_attached()) {
        lv_label_set_text(lbl_net_info, "Join or create a network first.");
        return;
    }
    lv_label_set_text(lbl_share_code, "...");
    lv_label_set_text(lbl_share_state, "Opening...");
    show_panel(panel_share);
    job_t j = JOB_SHARE;
    xQueueSend(s_jobs, &j, 0);
}

static void on_share_close(lv_event_t *e)
{
    (void) e;
    s_share_active = false;
    thread_share_stop();
    show_panel(panel_net);
}

/* 1 s tick while the share screen is up: state word and countdown. */
static void share_timer_cb(lv_timer_t *t)
{
    (void) t;
    if (!s_share_active) {
        return;
    }
    const char *st = thread_share_state();
    if (s_share_left_s > 0) {
        s_share_left_s--;
    }
    char buf[64];
    if (strcmp(st, "off") == 0) {
        snprintf(buf, sizeof(buf), "Used or expired - close and share again");
        s_share_active = false;
    } else if (strcmp(st, "accepted") == 0) {
        snprintf(buf, sizeof(buf), "Commissioner accepted - credentials sent");
    } else if (strcmp(st, "connected") == 0) {
        snprintf(buf, sizeof(buf), "Commissioner connecting...");
    } else {
        snprintf(buf, sizeof(buf), "Waiting for a commissioner   %d:%02d",
                 s_share_left_s / 60, s_share_left_s % 60);
    }
    lv_label_set_text(lbl_share_state, buf);
}

static void on_dsqr_open(lv_event_t *e)
{
    (void) e;
    char hex[520];
    if (!thread_dataset_hex(hex, sizeof(hex))) {
        lv_label_set_text(lbl_net_info, "No dataset stored.");
        return;
    }
    /* Built on demand and torn down on close: the QR widget and its encoder
     * scratch come out of internal RAM, which this board cannot spare idle. */
    if (dsqr_widget == NULL) {
        dsqr_widget = lv_qrcode_create(panel_dsqr);
        lv_qrcode_set_size(dsqr_widget, 140);
        lv_qrcode_set_dark_color(dsqr_widget, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(dsqr_widget, lv_color_hex(0xFFFFFF));
        lv_obj_align(dsqr_widget, LV_ALIGN_TOP_MID, 0, 2);
    }
    lv_qrcode_update(dsqr_widget, hex, strlen(hex));
    show_panel(panel_dsqr);
}

static void on_dsqr_close(lv_event_t *e)
{
    (void) e;
    if (dsqr_widget) {
        lv_obj_delete(dsqr_widget);
        dsqr_widget = NULL;
    }
    show_panel(panel_net);
}

static void on_scan(lv_event_t *e)
{
    (void) e;
    if (!s_have_wifi) {
        lv_label_set_text(lbl_status, "No Wi-Fi yet");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_ERROR), LV_PART_MAIN);
        return;
    }
    lv_label_set_text(lbl_status, "Scanning...");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    job_t j = JOB_SCAN;
    xQueueSend(s_jobs, &j, 0);
}

/*
 * Two-tap confirmation for anything that throws credentials away. First tap
 * relabels the button "Tap again" for 3 s; a second tap inside that window
 * returns true. Any other button, or the timeout, disarms it.
 */
static lv_obj_t *s_armed;
static lv_timer_t *s_arm_timer;
static char s_arm_label[24];

static void disarm(void)
{
    if (s_armed) {
        lv_label_set_text(lv_obj_get_child(s_armed, 0), s_arm_label);
        s_armed = NULL;
    }
    if (s_arm_timer) {
        lv_timer_delete(s_arm_timer);
        s_arm_timer = NULL;
    }
}

static void disarm_cb(lv_timer_t *t)
{
    (void) t;
    s_arm_timer = NULL;   /* one-shot: LVGL frees it after this returns */
    if (s_armed) {
        lv_label_set_text(lv_obj_get_child(s_armed, 0), s_arm_label);
        s_armed = NULL;
    }
}

static bool confirm_tap(lv_obj_t *btn)
{
    if (s_armed == btn) {
        disarm();
        return true;
    }
    disarm();
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    snprintf(s_arm_label, sizeof(s_arm_label), "%s", lv_label_get_text(l));
    lv_label_set_text(l, "Tap again");
    s_armed = btn;
    s_arm_timer = lv_timer_create(disarm_cb, 3000, NULL);
    lv_timer_set_repeat_count(s_arm_timer, 1);
    return false;
}

static void on_generate(lv_event_t *e)
{
    if (!confirm_tap(lv_event_get_target(e))) {
        return;
    }
    lv_label_set_text(lbl_status, "Creating network...");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    job_t j = JOB_FORM;
    xQueueSend(s_jobs, &j, 0);
}

static void on_pick(lv_event_t *e)
{
    int idx = (int) (intptr_t) lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_found_n) {
        return;
    }
    s_target = s_found[idx];
    s_code[0] = '\0';

    char buf[80];
    snprintf(buf, sizeof(buf), "%s:%u", s_target.ip, s_target.port);
    lv_label_set_text(lbl_target, buf);
    lv_label_set_text(lbl_code, "_________");
    show_panel(panel_keypad);
}

static void on_back_main(lv_event_t *e)
{
    (void) e;
    lv_label_set_text(lbl_status, "Ready");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    show_panel(panel_main);
}

static void on_keypad(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    const char *txt = lv_buttonmatrix_get_button_text(bm, lv_buttonmatrix_get_selected_button(bm));
    if (txt == NULL) {
        return;
    }
    size_t len = strlen(s_code);

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        if (len) {
            s_code[len - 1] = '\0';
        }
    } else if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        if (len == 9) {
            job_t j = JOB_JOIN;
            lv_label_set_text(lbl_result_title, "Connecting...");
            lv_obj_set_style_text_color(lbl_result_title, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
            lv_label_set_text(lbl_result_body, s_target.name);
            show_panel(panel_result);
            xQueueSend(s_jobs, &j, 0);
        }
        return;
    } else if (len < 9 && txt[0] >= '0' && txt[0] <= '9') {
        s_code[len] = txt[0];
        s_code[len + 1] = '\0';
    }

    redraw_code();
}

/* ---------------- Wi-Fi provisioning ---------------- */

static void on_wifi_open(lv_event_t *e)
{
    (void) e;
    lv_obj_clean(wifi_list_w);
    lv_label_set_text(lbl_wifi_hint, "Scanning...");
    lv_obj_clear_flag(lbl_wifi_hint, LV_OBJ_FLAG_HIDDEN);
    show_panel(panel_wifi);
    job_t j = JOB_WIFI_SCAN;
    xQueueSend(s_jobs, &j, 0);
}

static void on_wifi_leave(lv_event_t *e)
{
    (void) e;
    app_wifi_leave();   /* quick: config write + disconnect, safe from here */
    lv_label_set_text(lbl_wifi_hint, "Left the network.\nCredentials erased.");
    lv_obj_clear_flag(lbl_wifi_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_list_w, LV_OBJ_FLAG_HIDDEN);
}

static void on_wifi_pick(lv_event_t *e)
{
    int idx = (int) (intptr_t) lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_aps_n) {
        return;
    }
    snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", s_aps[idx].ssid);
    lv_label_set_text(lbl_wpass_ssid, s_join_ssid);
    lv_textarea_set_text(wpass_ta, "");
    show_panel(panel_wpass);
}

static void on_wpass_kb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) {
        show_panel(panel_wifi);
        return;
    }
    if (code != LV_EVENT_READY) {
        return;
    }
    snprintf(s_join_pass, sizeof(s_join_pass), "%s", lv_textarea_get_text(wpass_ta));
    lv_label_set_text(lbl_status, "Connecting to Wi-Fi...");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    show_panel(panel_main);
    job_t j = JOB_WIFI_JOIN;
    xQueueSend(s_jobs, &j, 0);
}

static void on_qr_open(lv_event_t *e)
{
    (void) e;
    s_qr_abort = false;
    lv_label_set_text(lbl_qr_hint, "Point at the QR code");
    show_panel(panel_qr);
    job_t j = JOB_QR;
    xQueueSend(s_jobs, &j, 0);
}

static void on_qr_cancel(lv_event_t *e)
{
    (void) e;
    s_qr_abort = true;
    show_panel(panel_keypad);
}

/* ---------------- worker ---------------- */

static void finish_scan(void)
{
    bsp_display_lock(0);
    lv_obj_clean(ba_list);
    if (s_found_n == 0) {
        lv_label_set_text(lbl_list_hint,
                          "No border agents found.\n\n"
                          "A router only advertises while its\n"
                          "ephemeral key is active - start one,\n"
                          "then scan again.");
        lv_obj_clear_flag(lbl_list_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ba_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_list_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ba_list, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < s_found_n; i++) {
            char row[80];
            snprintf(row, sizeof(row), "%s  (%s)", s_found[i].name, s_found[i].ip);
            lv_obj_t *b = lv_list_add_button(ba_list, LV_SYMBOL_WIFI, row);
            lv_obj_set_style_text_color(b, lv_color_hex(COL_TEXT), LV_PART_MAIN);
            lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL), LV_PART_MAIN);
            lv_obj_add_event_cb(b, on_pick, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        }
    }
    show_panel(panel_list);
    bsp_display_unlock();
}

static void finish_wifi_scan(void)
{
    bsp_display_lock(0);
    lv_obj_clean(wifi_list_w);
    if (s_aps_n == 0) {
        lv_label_set_text(lbl_wifi_hint, "No networks found");
        lv_obj_clear_flag(lbl_wifi_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_list_w, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_wifi_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_list_w, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < s_aps_n; i++) {
            char row[48];
            snprintf(row, sizeof(row), "%s  (%d)", s_aps[i].ssid, s_aps[i].rssi);
            lv_obj_t *b = lv_list_add_button(
                wifi_list_w, s_aps[i].secured ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_WIFI, row);
            lv_obj_set_style_text_color(b, lv_color_hex(COL_TEXT), LV_PART_MAIN);
            lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL), LV_PART_MAIN);
            lv_obj_add_event_cb(b, on_wifi_pick, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        }
    }
    bsp_display_unlock();
}

static void finish_join(bool ok, const char *name, int channel, uint16_t panid)
{
    bsp_display_lock(0);
    if (ok) {
        lv_label_set_text(lbl_result_title, "Credentials received");
        lv_obj_set_style_text_color(lbl_result_title, lv_color_hex(COL_OK), LV_PART_MAIN);
        char body[128];
        snprintf(body, sizeof(body), "%s\nchannel %d   pan 0x%04x",
                 name && name[0] ? name : "(unnamed)", channel, panid);
        lv_label_set_text(lbl_result_body, body);
    } else {
        lv_label_set_text(lbl_result_title, "Failed");
        lv_obj_set_style_text_color(lbl_result_title, lv_color_hex(COL_ERROR), LV_PART_MAIN);
        const char *why = meshcop_last_error();
        char body[160];
        snprintf(body, sizeof(body), "%s\n\nKeys are single-use and expire.",
                 why && why[0] ? why : "Unknown error - see serial log.");
        lv_label_set_text(lbl_result_body, body);
    }
    bsp_display_unlock();
}

/*
 * Take the passcode out of a scanned payload. Home Assistant encodes the bare
 * 9-digit code, but other vendors may wrap it, so keep only digits.
 */
static bool digits_from_payload(const char *payload, char *out9)
{
    int n = 0;
    for (const char *p = payload; *p && n < 9; p++) {
        if (*p >= '0' && *p <= '9') {
            out9[n++] = *p;
        }
    }
    out9[n] = '\0';
    return n == 9;
}

static void run_qr_job(void)
{
    if (qrscan_start() != ESP_OK) {
        bsp_display_lock(0);
        lv_label_set_text(lbl_qr_hint, "Camera unavailable");
        bsp_display_unlock();
        return;
    }

    char payload[256];
    char digits[10];
    /*
     * quirc on a 320x240 frame is expensive, so this loop is CPU-bound. Without
     * yielding it starves the LVGL and Thread tasks and the UI appears to hang
     * after a while. Cap the run and sleep briefly between frames.
     */
    const int max_frames = 400;
    bool got = false;

    for (int i = 0; i < max_frames && !s_qr_abort && !got; i++) {
        if (!qrscan_poll(payload, sizeof(payload), s_preview)) {
            if ((i % 2) == 0 && s_preview) {
                bsp_display_lock(0);
                lv_obj_invalidate(qr_canvas);
                bsp_display_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        if (digits_from_payload(payload, digits)) {
            strcpy(s_code, digits);
            got = true;
        } else {
            bsp_display_lock(0);
            lv_label_set_text(lbl_qr_hint, "Scanned, but no 9-digit code");
            bsp_display_unlock();
        }
    }

    qrscan_stop();

    bsp_display_lock(0);
    if (got) {
        redraw_code();
        show_panel(panel_keypad);
    } else if (!s_qr_abort) {
        lv_label_set_text(lbl_qr_hint, "No code found - try again");
    }
    bsp_display_unlock();
}

static void worker(void *arg)
{
    (void) arg;
    static uint8_t dataset[512];
    job_t job;

    for (;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job == JOB_QR) {
            run_qr_job();
        } else if (job == JOB_CAMTEST) {
            qrscan_selftest();
            xSemaphoreGive(s_camtest_done);
        } else if (job == JOB_SHARE) {
            const uint32_t lifetime_ms = settings_get()->share_minutes * 60000u;
            esp_err_t err = thread_share_start(s_share_code, sizeof(s_share_code), lifetime_ms);
            bsp_display_lock(0);
            if (err == ESP_OK) {
                char spaced[16];
                snprintf(spaced, sizeof(spaced), "%.3s %.3s %.3s",
                         s_share_code, s_share_code + 3, s_share_code + 6);
                lv_label_set_text(lbl_share_code, spaced);
                s_share_left_s = lifetime_ms / 1000;
                s_share_active = true;
                lv_label_set_text_fmt(lbl_share_state, "Waiting for a commissioner   %d:00",
                                      (int) settings_get()->share_minutes);
            } else {
                lv_label_set_text(lbl_share_code, "--- --- ---");
                lv_label_set_text(lbl_share_state,
                                  err == ESP_ERR_INVALID_STATE ? "Border router not running"
                                                               : "Could not start ephemeral key");
            }
            bsp_display_unlock();
        } else if (job == JOB_CALL) {
            if (s_call_fn) {
                s_call_fn(s_call_arg);
            }
            xSemaphoreGive(s_call_done);
        } else if (job == JOB_BR_START) {
            thread_run_border_router_start(s_br_backbone);
            bsp_display_lock(0);
            refresh_network_label();
            bsp_display_unlock();
        } else if (job == JOB_FORM) {
            char name[17];
            int ch = 0;
            uint16_t pan = 0;
            esp_err_t err = thread_form_network(settings_get()->new_net_channel, name, sizeof(name), &ch, &pan);
            if (err == ESP_OK) {
                snprintf(s_net_name, sizeof(s_net_name), "%s", name);
                s_net_channel = ch;
                s_net_panid = pan;
                s_have_net = true;
                network_save();
                /* Sole member of a fresh PAN: leader comes quickly. */
                for (int i = 0; i < 30 && !thread_attached(); i++) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }
            bsp_display_lock(0);
            if (err == ESP_OK) {
                lv_label_set_text(lbl_status, "Network created");
                lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_OK), LV_PART_MAIN);
                refresh_network_label();
            } else {
                lv_label_set_text(lbl_status, "Failed to create network");
                lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_ERROR), LV_PART_MAIN);
            }
            bsp_display_unlock();
        } else if (job == JOB_WIFI_SCAN) {
            s_aps_n = app_wifi_scan(s_aps, sizeof(s_aps) / sizeof(s_aps[0]));
            finish_wifi_scan();
        } else if (job == JOB_WIFI_JOIN) {
            bool ok = app_wifi_join(s_join_ssid, s_join_pass, 20000) == ESP_OK;
            bsp_display_lock(0);
            if (ok) {
                lv_label_set_text(lbl_status, "Wi-Fi connected");
                lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_OK), LV_PART_MAIN);
            } else {
                lv_label_set_text(lbl_status, "Wi-Fi failed - check password");
                lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_ERROR), LV_PART_MAIN);
            }
            bsp_display_unlock();
        } else if (job == JOB_SCAN) {
            s_found_n = discover_border_agents(s_found, BA_MAX, 3000);
            finish_scan();
        } else {
            size_t dlen = 0;
            esp_err_t err = meshcop_fetch_dataset(s_target.ip, s_target.port, s_code,
                                                  dataset, sizeof(dataset), &dlen);
            /* Watermark right after the handshake: this is the deepest the
             * worker stack ever gets, and a reset here looks like a UI bug. */
            ESP_LOGI(TAG, "worker stack headroom: %u bytes",
                     (unsigned) uxTaskGetStackHighWaterMark(NULL));
            if (err == ESP_OK) {
                char name[17];
                int ch = 0;
                uint16_t pan = 0;
                meshcop_summarize(dataset, dlen, name, sizeof(name), &ch, &pan);
                finish_join(true, name, ch, pan);
                printf("\n--- ACTIVE OPERATIONAL DATASET (%u bytes) ---\n", (unsigned) dlen);
                meshcop_print_tlvs(dataset, dlen, false);

                /* Remember it, then actually attach to that network. */
                snprintf(s_net_name, sizeof(s_net_name), "%s", name);
                s_net_channel = ch;
                s_net_panid = pan;
                s_have_net = true;
                network_save();

                if (thread_join(dataset, dlen) == ESP_OK) {
                    /* Attaching to an established mesh can take the better part
                     * of a minute after a fresh erase. The periodic UI timer
                     * keeps updating after this loop gives up, so a slow attach
                     * still shows up rather than being reported as a failure. */
                    for (int i = 0; i < 120 && !thread_attached(); i++) {
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                    ESP_LOGI(TAG, "thread role: %s", thread_role());
                }
                bsp_display_lock(0);
                refresh_network_label();
                bsp_display_unlock();
            } else {
                finish_join(false, NULL, 0, 0);
            }
        }
    }
}

/* ---------------- construction ---------------- */

/* ---------------- settings ---------------- */

static void on_settings_open(lv_event_t *e)
{
    (void) e;
    show_panel(panel_settings);
}

static void on_wake(lv_event_t *e)
{
    (void) e;
    if (!s_asleep) {
        return;
    }
    s_asleep = false;
    bsp_display_brightness_set(settings_get()->brightness);
    /* Deleting the object from inside its own event is unsafe; defer it. */
    lv_obj_delete_async(s_sleep_shield);
    s_sleep_shield = NULL;
}

/* -- screen -- */

static void on_brightness(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    settings_get()->brightness = (uint8_t) v;
    bsp_display_brightness_set(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        settings_save();
    }
}

static void on_sleep_choice(lv_event_t *e)
{
    settings_get()->sleep_idx = (uint8_t) lv_roller_get_selected(lv_event_get_target(e));
    settings_save();
}

static void on_keep_awake(lv_event_t *e)
{
    settings_get()->keep_awake_powered =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
}

/* -- power -- */

/*
 * Appends to a fixed buffer, clamping at the end.
 *
 * `n += snprintf(buf + n, sizeof(buf) - n, ...)` is the obvious idiom and it
 * is wrong: snprintf returns the length it *would* have written, so once the
 * text is truncated `n` exceeds the buffer and `sizeof(buf) - n` underflows to
 * a huge size_t -- the next call then writes off the end.
 */
static void appendf(char *buf, size_t cap, size_t *used, const char *fmt, ...)
{
    if (*used >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (w < 0) {
        return;
    }
    *used = ((size_t) w >= cap - *used) ? cap - 1 : *used + (size_t) w;
}

static void refresh_power_info(void)
{
    power_status_t ps;
    char buf[320];
    size_t n = 0;
    if (power_read(&ps) != ESP_OK) {
        lv_label_set_text(lbl_power_info, "PMIC not reachable");
        return;
    }
    if (ps.present) {
        appendf(buf, sizeof(buf), &n, "Battery  %d%%   %d.%02d V   %s\n",
                ps.percent, ps.batt_mv / 1000, (ps.batt_mv % 1000) / 10, ps.charge_detail);
    } else {
        appendf(buf, sizeof(buf), &n, "Battery  not connected (switch on Base DIN)\n");
    }
    appendf(buf, sizeof(buf), &n, "USB      %s   %d.%02d V\n",
            ps.vbus ? "present" : "absent", ps.vbus_mv / 1000, (ps.vbus_mv % 1000) / 10);
    appendf(buf, sizeof(buf), &n, "System   %d.%02d V\n",
            ps.vsys_mv / 1000, (ps.vsys_mv % 1000) / 10);
    float t;
    if (power_esp_temp(&t)) {
        appendf(buf, sizeof(buf), &n, "Temp     PMIC %d C   ESP32 %.0f C\n", ps.pmic_temp_c, t);
    } else {
        appendf(buf, sizeof(buf), &n, "Temp     PMIC %d C\n", ps.pmic_temp_c);
    }
    float rate;
    int mins;
    if (ps.vbus) {
        appendf(buf, sizeof(buf), &n, "\nOn external power; discharge\nrate measured on battery only.");
    } else if (power_discharge_rate(&rate, &mins)) {
        appendf(buf, sizeof(buf), &n, "\nDischarge  %.1f %%/h   about %dh %02dm left",
                rate, mins / 60, mins % 60);
    } else {
        appendf(buf, sizeof(buf), &n, "\nDischarge  measuring (10 min on battery)");
    }
    appendf(buf, sizeof(buf), &n, "\n\nThe AXP2101 has no current sensor,\nso there is no live mW figure.");
    lv_label_set_text(lbl_power_info, buf);
}

static void on_power_open(lv_event_t *e)
{
    (void) e;
    refresh_power_info();
    show_panel(panel_power);
}

/* -- thread -- */

static void on_prefer_router(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_get()->prefer_router = on;
    settings_save();
    thread_set_prefer_router(on);
}

static void on_rest_api(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_get()->rest_api = on;
    settings_save();
    if (on) {
        otbr_rest_start();
    } else {
        otbr_rest_stop();
    }
}

static void on_rest_epskc(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_get()->rest_epskc = on;
    settings_save();
    /* Turning it on also enables the stack feature, so a client can activate
     * a key without anyone pressing Share first. */
    thread_epskc_set_feature_enabled(on);
    otbr_rest_set_epskc(on);
}

static void on_channel_choice(lv_event_t *e)
{
    uint32_t idx = lv_roller_get_selected(lv_event_get_target(e));
    settings_get()->new_net_channel = idx == 0 ? 0 : (uint8_t) (10 + idx);   /* 1 -> 11 */
    settings_save();
}

static void on_share_choice(lv_event_t *e)
{
    static const uint8_t mins[] = { 2, 5, 10 };
    uint32_t idx = lv_roller_get_selected(lv_event_get_target(e));
    settings_get()->share_minutes = mins[idx < 3 ? idx : 1];
    settings_save();
}

/* -- device name -- */

static void on_name_open(lv_event_t *e)
{
    (void) e;
    char host[33] = "cores3-thread-br";
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(host);
        nvs_get_str(h, "host", host, &len);
        nvs_close(h);
    }
    lv_textarea_set_text(name_ta, host);
    show_panel(panel_name);
}

static void on_name_kb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) {
        show_panel(panel_settings);
        return;
    }
    if (code != LV_EVENT_READY) {
        return;
    }
    const char *name = lv_textarea_get_text(name_ta);
    if (name[0] == '\0') {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "host", name);
        nvs_commit(h);
        nvs_close(h);
    }
    /* Takes effect for anything published from now on; the meshcop record
     * re-publishes on the next attach or reboot. */
    mdns_hostname_set(name);
    show_panel(panel_settings);
}

/* -- about -- */

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "unknown";
    }
}

static void refresh_about_info(void)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    int64_t up = esp_timer_get_time() / 1000000;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "Firmware   %s\n"
             "ESP-IDF    %s\n"
             "IP         %s\n"
             "MAC        %02x:%02x:%02x:%02x:%02x:%02x\n"
             "Uptime     %lldh %02lldm %02llds\n"
             "Last reset %s\n"
             "Free RAM   %u KB internal, %u KB PSRAM",
             FW_VERSION, esp_get_idf_version(), s_ip_str,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (long long) (up / 3600), (long long) ((up / 60) % 60), (long long) (up % 60),
             reset_reason_str(),
             (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    lv_label_set_text(lbl_about_info, buf);
}

static void on_about_open(lv_event_t *e)
{
    (void) e;
    refresh_about_info();
    show_panel(panel_about);
}

static void on_screen_open(lv_event_t *e)   { (void) e; show_panel(panel_screen); }
static void on_tset_open(lv_event_t *e)     { (void) e; show_panel(panel_tset); }

static void on_reboot(lv_event_t *e)
{
    if (confirm_tap(lv_event_get_target(e))) {
        app_reboot();
    }
}

static void on_factory_reset(lv_event_t *e)
{
    if (confirm_tap(lv_event_get_target(e))) {
        app_factory_reset();
    }
}

/* ---------------- construction ---------------- */

static void build_main(void)
{
    panel_main = mk_panel();

    lbl_status = mk_label(panel_main, &lv_font_montserrat_20, COL_TEXT, "Starting...");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 8);

    /* Persisted Thread network, so the device is useful at a glance. */
    lbl_network = mk_label(panel_main, &lv_font_montserrat_14, COL_DIM, "no credentials yet");
    lv_obj_align(lbl_network, LV_ALIGN_TOP_MID, 0, 40);

    /*
     * 2x2 grid of identical 148x46 buttons, centred: columns sit at +-78 from
     * the panel's middle (half of 148 + an 8 px gutter). Top row is the two
     * ways to get credentials -- from an existing router, or minted fresh.
     */
    const int bw = 148, bh = 46, col = 78;
    lv_obj_t *b = mk_button(panel_main, "Scan", on_scan, COL_ACCENT, bw, bh);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, -col, -58);
    lv_obj_t *g = mk_button(panel_main, "New network", on_generate, COL_OK, bw, bh);
    lv_obj_align(g, LV_ALIGN_BOTTOM_MID, col, -58);

    /* Details, share, dataset QR and forget all live behind "Network": the
     * grid has no room for four more buttons, and the destructive one belongs
     * one tap further away than Scan. */
    lv_obj_t *f = mk_button(panel_main, "Network", on_net_open, COL_DIM, bw, bh);
    lv_obj_align(f, LV_ALIGN_BOTTOM_MID, -col, -6);
    lv_obj_t *w = mk_button(panel_main, "Settings", on_settings_open, COL_DIM, bw, bh);
    lv_obj_align(w, LV_ALIGN_BOTTOM_MID, col, -6);
}

static lv_obj_t *settings_row(lv_obj_t *list, const char *txt, lv_event_cb_t cb)
{
    /* No icon: confirm_tap() relabels child 0, which must be the text. */
    lv_obj_t *b = lv_list_add_button(list, NULL, txt);
    lv_obj_set_style_text_color(b, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/*
 * Scrolling content region: fills the panel except the strip the Back button
 * occupies, so text can never run underneath it.
 *
 * `flex` lays children out as a column, which is what the settings pages want.
 * They are built from flex rows rather than hand-placed at pixel offsets
 * because widgets draw larger than the size they are given -- a slider's knob
 * overhangs its track, switches and rollers carry theme padding -- so absolute
 * placement kept producing overlaps that only showed up on the hardware. Text
 * panels pass false and position their own label.
 */
static lv_obj_t *mk_content(lv_obj_t *panel, bool flex)
{
    lv_obj_t *box = lv_obj_create(panel);
    lv_obj_set_size(box, BSP_LCD_H_RES - 12, UI_PANEL_H - UI_BACK_STRIP);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    if (flex) {
        lv_obj_set_style_pad_row(box, 6, LV_PART_MAIN);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    }
    return box;
}

/* Plain left-aligned caption, for panels that are not flex pages. */
static lv_obj_t *mk_caption(lv_obj_t *p, const char *txt, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 8, y);
    return l;
}

/* One setting: caption on the left, control added by the caller on the right. */
static lv_obj_t *mk_row(lv_obj_t *page, const char *caption, int h)
{
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_set_size(row, LV_PCT(100), h);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_text(l, caption);
    return row;
}

/*
 * Roller, not dropdown. A dropdown's list opens downward over whatever follows
 * -- on a 196 px panel the channel list (17 entries) covered the next setting
 * and the Back button, and ran off the bottom. A roller keeps its size.
 */
static lv_obj_t *mk_roller(lv_obj_t *row, const char *opts, lv_event_cb_t cb)
{
    lv_obj_t *r = lv_roller_create(row);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 2);
    lv_obj_set_width(r, 118);
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_text_color(r, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_ACCENT), LV_PART_SELECTED);
    lv_obj_add_event_cb(r, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return r;
}

static lv_obj_t *mk_switch(lv_obj_t *row, bool on, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 50, 26);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_OK), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void build_settings(void)
{
    panel_settings = mk_panel();

    lv_obj_t *list = lv_list_create(panel_settings);
    lv_obj_set_size(list, BSP_LCD_H_RES - 20, 140);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);

    settings_row(list, "Wi-Fi", on_wifi_open);
    settings_row(list, "Screen", on_screen_open);
    settings_row(list, "Power", on_power_open);
    settings_row(list, "Thread", on_tset_open);
    settings_row(list, "Device name", on_name_open);
    settings_row(list, "About", on_about_open);
    lv_obj_t *rb = settings_row(list, "Reboot", on_reboot);
    lv_obj_set_style_text_color(rb, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_t *fr = settings_row(list, "Factory reset", on_factory_reset);
    lv_obj_set_style_text_color(fr, lv_color_hex(COL_ERROR), LV_PART_MAIN);

    lv_obj_t *bk = mk_button(panel_settings, "Back", on_back_main, COL_DIM, 110, 40);
    lv_obj_align(bk, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void build_screen(void)
{
    panel_screen = mk_panel();

    lv_obj_t *page = mk_content(panel_screen, true);

    /* 30 px row: the knob overhangs the track, so the row must be taller than
     * the slider itself or the knob clips into the neighbouring row. */
    lv_obj_t *r1 = mk_row(page, "Brightness", 30);
    lv_obj_t *sl = lv_slider_create(r1);
    lv_slider_set_range(sl, 10, 100);
    lv_slider_set_value(sl, settings_get()->brightness, LV_ANIM_OFF);
    lv_obj_set_size(sl, 150, 8);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, on_brightness, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, on_brightness, LV_EVENT_RELEASED, NULL);

    lv_obj_t *r2 = mk_row(page, "Sleep after", 62);
    lv_obj_t *rl = mk_roller(r2, SETTINGS_SLEEP_OPTIONS, on_sleep_choice);
    lv_roller_set_selected(rl, settings_get()->sleep_idx, LV_ANIM_OFF);

    lv_obj_t *r3 = mk_row(page, "Stay on when plugged in", 34);
    mk_switch(r3, settings_get()->keep_awake_powered, on_keep_awake);

    lv_obj_t *bk = mk_button(panel_screen, "Back", on_settings_open, COL_DIM, 110, 40);
    lv_obj_align(bk, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void build_power(void)
{
    panel_power = mk_panel();
    /* Scrollable region that ends above the Back button: the text is taller
     * than the panel, and a plain label just ran underneath the button. */
    lv_obj_t *box = mk_content(panel_power, false);
    lbl_power_info = lv_label_create(box);
    lv_obj_set_style_text_font(lbl_power_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_power_info, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_width(lbl_power_info, BSP_LCD_H_RES - 28);
    lv_label_set_long_mode(lbl_power_info, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_power_info, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bk = mk_button(panel_power, "Back", on_settings_open, COL_DIM, 110, 40);
    lv_obj_align(bk, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void build_tset(void)
{
    panel_tset = mk_panel();

    lv_obj_t *page = mk_content(panel_tset, true);

    lv_obj_t *r1 = mk_row(page, "Prefer router role", 34);
    mk_switch(r1, settings_get()->prefer_router, on_prefer_router);

    lv_obj_t *r_rest = mk_row(page, "REST API (port 8081)", 34);
    mk_switch(r_rest, settings_get()->rest_api, on_rest_api);

    lv_obj_t *r_eps = mk_row(page, "ePSKc over REST", 34);
    mk_switch(r_eps, settings_get()->rest_epskc, on_rest_epskc);

    lv_obj_t *r2 = mk_row(page, "New network channel", 62);
    lv_obj_t *ch = mk_roller(r2,
                             "Auto\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26",
                             on_channel_choice);
    uint8_t c = settings_get()->new_net_channel;
    lv_roller_set_selected(ch, c ? c - 10 : 0, LV_ANIM_OFF);

    lv_obj_t *r3 = mk_row(page, "Share code lifetime", 62);
    lv_obj_t *sh = mk_roller(r3, "2 min\n5 min\n10 min", on_share_choice);
    uint8_t m = settings_get()->share_minutes;
    lv_roller_set_selected(sh, m == 2 ? 0 : m == 10 ? 2 : 1, LV_ANIM_OFF);

    lv_obj_t *bk = mk_button(panel_tset, "Back", on_settings_open, COL_DIM, 110, 40);
    lv_obj_align(bk, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void build_name(void)
{
    panel_name = mk_panel();

    mk_caption(panel_name, "Device name (mDNS)", 0);

    name_ta = lv_textarea_create(panel_name);
    lv_textarea_set_one_line(name_ta, true);
    lv_textarea_set_max_length(name_ta, 32);
    lv_textarea_set_accepted_chars(name_ta,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-");
    lv_obj_set_size(name_ta, BSP_LCD_H_RES - 24, 36);
    lv_obj_align(name_ta, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(name_ta, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_text_color(name_ta, lv_color_hex(COL_TEXT), LV_PART_MAIN);

    lv_obj_t *kb = lv_keyboard_create(panel_name);
    lv_keyboard_set_textarea(kb, name_ta);
    lv_obj_set_size(kb, BSP_LCD_H_RES - 12, 130);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_add_event_cb(kb, on_name_kb, LV_EVENT_ALL, NULL);
}

static void build_about(void)
{
    panel_about = mk_panel();
    lv_obj_t *box = mk_content(panel_about, false);
    lbl_about_info = lv_label_create(box);
    lv_obj_set_style_text_font(lbl_about_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_about_info, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_width(lbl_about_info, BSP_LCD_H_RES - 28);
    lv_label_set_long_mode(lbl_about_info, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_about_info, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bk = mk_button(panel_about, "Back", on_settings_open, COL_DIM, 110, 40);
    lv_obj_align(bk, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void build_net(void)
{
    panel_net = mk_panel();

    lbl_net_info = mk_label(panel_net, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_align(lbl_net_info, LV_ALIGN_TOP_MID, 0, 6);

    const int w = 72, h = 40, y = -6;
    lv_obj_t *sh = mk_button(panel_net, "Share", on_share_open, COL_OK, w, h);
    lv_obj_align(sh, LV_ALIGN_BOTTOM_MID, -117, y);
    lv_obj_t *qr = mk_button(panel_net, "QR", on_dsqr_open, COL_ACCENT, w, h);
    lv_obj_align(qr, LV_ALIGN_BOTTOM_MID, -39, y);
    lv_obj_t *fg = mk_button(panel_net, "Forget", on_forget, COL_ERROR, w, h);
    lv_obj_align(fg, LV_ALIGN_BOTTOM_MID, 39, y);
    lv_obj_t *bk = mk_button(panel_net, "Back", on_back_main, COL_DIM, w, h);
    lv_obj_align(bk, LV_ALIGN_BOTTOM_MID, 117, y);
}

static void build_share(void)
{
    panel_share = mk_panel();

    lv_obj_t *hint = mk_label(panel_share, &lv_font_montserrat_14, COL_DIM,
                              "Enter this code on the commissioner");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 8);

    lbl_share_code = mk_label(panel_share, &lv_font_montserrat_20, COL_ACCENT, "");
    lv_obj_align(lbl_share_code, LV_ALIGN_TOP_MID, 0, 44);

    lbl_share_state = mk_label(panel_share, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_align(lbl_share_state, LV_ALIGN_TOP_MID, 0, 90);

    lv_obj_t *b = mk_button(panel_share, "Close", on_share_close, COL_DIM, 110, 40);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_timer_create(share_timer_cb, 1000, NULL);
}

static void build_dsqr(void)
{
    panel_dsqr = mk_panel();

    lv_obj_t *hint = mk_label(panel_dsqr, &lv_font_montserrat_14, COL_DIM,
                              "Dataset TLVs - contains the network key");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -50);

    lv_obj_t *b = mk_button(panel_dsqr, "Back", on_dsqr_close, COL_DIM, 110, 40);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void build_wifi(void)
{
    panel_wifi = mk_panel();

    wifi_list_w = lv_list_create(panel_wifi);
    lv_obj_set_size(wifi_list_w, BSP_LCD_H_RES - 20, 138);
    lv_obj_align(wifi_list_w, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_bg_color(wifi_list_w, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_list_w, 0, LV_PART_MAIN);

    lbl_wifi_hint = mk_label(panel_wifi, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_align(lbl_wifi_hint, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_add_flag(lbl_wifi_hint, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *r = mk_button(panel_wifi, "Rescan", on_wifi_open, COL_DIM, 96, 40);
    lv_obj_align(r, LV_ALIGN_BOTTOM_LEFT, 4, -6);
    lv_obj_t *l = mk_button(panel_wifi, "Leave", on_wifi_leave, COL_ERROR, 96, 40);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_t *bk = mk_button(panel_wifi, "Back", on_settings_open, COL_DIM, 96, 40);
    lv_obj_align(bk, LV_ALIGN_BOTTOM_RIGHT, -4, -6);
}

static void build_wpass(void)
{
    panel_wpass = mk_panel();

    lbl_wpass_ssid = mk_label(panel_wpass, &lv_font_montserrat_14, COL_ACCENT, "");
    lv_obj_align(lbl_wpass_ssid, LV_ALIGN_TOP_LEFT, 2, 0);

    wpass_ta = lv_textarea_create(panel_wpass);
    lv_textarea_set_one_line(wpass_ta, true);
    lv_textarea_set_password_mode(wpass_ta, true);
    lv_textarea_set_max_length(wpass_ta, 64);
    lv_obj_set_size(wpass_ta, BSP_LCD_H_RES - 24, 36);
    lv_obj_align(wpass_ta, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(wpass_ta, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_text_color(wpass_ta, lv_color_hex(COL_TEXT), LV_PART_MAIN);

    /* Checkmark joins, close button goes back to the list. */
    lv_obj_t *kb = lv_keyboard_create(panel_wpass);
    lv_keyboard_set_textarea(kb, wpass_ta);
    lv_obj_set_size(kb, BSP_LCD_H_RES - 12, 130);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_add_event_cb(kb, on_wpass_kb, LV_EVENT_ALL, NULL);
}

static void build_list(void)
{
    panel_list = mk_panel();

    ba_list = lv_list_create(panel_list);
    lv_obj_set_size(ba_list, BSP_LCD_H_RES - 20, 118);
    lv_obj_align(ba_list, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_bg_color(ba_list, lv_color_hex(COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_width(ba_list, 0, LV_PART_MAIN);

    lbl_list_hint = mk_label(panel_list, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_align(lbl_list_hint, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_add_flag(lbl_list_hint, LV_OBJ_FLAG_HIDDEN);

    mk_button(panel_list, "Back", on_back_main, COL_DIM, 110, 40);
    lv_obj_align(lv_obj_get_child(panel_list, 2), LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void build_keypad(void)
{
    panel_keypad = mk_panel();

    /* Single line, ellipsised: a wrapping two-line target label overlapped the
     * passcode field below it. */
    lbl_target = mk_label(panel_keypad, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_width(lbl_target, BSP_LCD_H_RES - 80);
    lv_label_set_long_mode(lbl_target, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl_target, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(lbl_target, LV_ALIGN_TOP_LEFT, 2, 4);

    lbl_code = mk_label(panel_keypad, &lv_font_montserrat_20, COL_ACCENT, "_________");
    lv_obj_align(lbl_code, LV_ALIGN_TOP_MID, 0, 28);

    static const char *map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, "",
    };
    lv_obj_t *bm = lv_buttonmatrix_create(panel_keypad);
    lv_buttonmatrix_set_map(bm, map);
    lv_obj_set_size(bm, BSP_LCD_H_RES - 24, 118);
    lv_obj_align(bm, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(bm, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(bm, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bm, lv_color_hex(COL_PANEL), LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, lv_color_hex(COL_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_add_event_cb(bm, on_keypad, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *qr = mk_button(panel_keypad, "QR", on_qr_open, COL_OK, 56, 30);
    lv_obj_align(qr, LV_ALIGN_TOP_RIGHT, -2, 26);
}

static void build_qr(void)
{
    panel_qr = mk_panel();

    /* Preview buffer in PSRAM: 320x240x2 is far too big for internal RAM. */
    s_preview = heap_caps_malloc((size_t) BSP_LCD_H_RES * BSP_LCD_V_RES * 2,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_preview) {
        memset(s_preview, 0, (size_t) BSP_LCD_H_RES * BSP_LCD_V_RES * 2);
        qr_canvas = lv_canvas_create(panel_qr);
        lv_canvas_set_buffer(qr_canvas, s_preview, QRSCAN_PREVIEW_W, QRSCAN_PREVIEW_H,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_align(qr_canvas, LV_ALIGN_TOP_MID, 0, 0);
    }

    lbl_qr_hint = mk_label(panel_qr, &lv_font_montserrat_14, COL_DIM,
                           "Point at the QR code");
    lv_obj_align(lbl_qr_hint, LV_ALIGN_BOTTOM_MID, 0, -50);

    lv_obj_t *b = mk_button(panel_qr, "Cancel", on_qr_cancel, COL_DIM, 110, 40);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_result(void)
{
    panel_result = mk_panel();
    lbl_result_title = mk_label(panel_result, &lv_font_montserrat_20, COL_TEXT, "");
    lv_obj_align(lbl_result_title, LV_ALIGN_TOP_MID, 0, 16);
    lbl_result_body = mk_label(panel_result, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_align(lbl_result_body, LV_ALIGN_TOP_MID, 0, 54);
    mk_button(panel_result, "Done", on_back_main, COL_DIM, 110, 40);
    lv_obj_align(lv_obj_get_child(panel_result, 2), LV_ALIGN_BOTTOM_MID, 0, -6);
}

esp_err_t ui_init(void)
{
    ESP_ERROR_CHECK(bsp_i2c_init());

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = LVGL_BUFFER_PIXELS,
        .double_buffer = false,
        .flags = { .buff_dma = true, .buff_spiram = false, .sw_rotate = false },
    };
    if (bsp_display_start_with_config(&cfg) == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }
    bsp_display_backlight_on();
    bsp_display_brightness_set(settings_get()->brightness);

    s_jobs = xQueueCreate(4, sizeof(job_t));

    bsp_display_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* 16 pt, not 20: at 20 pt the centred title runs to ~x270 and anything in
     * the top-right corner lands on the "...er". At 16 pt it ends near x240,
     * leaving room for the battery. */
    lv_obj_t *title = mk_label(scr, &lv_font_montserrat_16, COL_ACCENT, "ePSKc Commissioner");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lbl_wifi = mk_label(scr, &lv_font_montserrat_14, COL_DIM, "wifi: not connected");
    lv_obj_align(lbl_wifi, LV_ALIGN_TOP_MID, 0, 30);

    /* Battery drawn as an outline with the percent inside, top-right on the
     * top layer so it rides above every panel. Non-clickable, so touches fall
     * through to whatever is underneath. -8 leaves room for the nub. */
    batt_body = lv_obj_create(lv_layer_top());
    lv_obj_set_size(batt_body, BATT_W, BATT_H);
    lv_obj_align(batt_body, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_bg_color(batt_body, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(batt_body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(batt_body, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(batt_body, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_set_style_radius(batt_body, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(batt_body, 2, LV_PART_MAIN);
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(batt_body, LV_OBJ_FLAG_HIDDEN);

    batt_fill = lv_obj_create(batt_body);
    lv_obj_set_size(batt_fill, 2, BATT_H - 8);
    lv_obj_align(batt_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(batt_fill, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_border_width(batt_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(batt_fill, 1, LV_PART_MAIN);
    lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lbl_batt = lv_label_create(batt_body);
    lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_label_set_text(lbl_batt, "");
    lv_obj_center(lbl_batt);

    /* The positive-terminal nub. */
    batt_nub = lv_obj_create(lv_layer_top());
    lv_obj_set_size(batt_nub, 3, 10);
    lv_obj_set_style_bg_color(batt_nub, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(batt_nub, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(batt_nub, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(batt_nub, 1, LV_PART_MAIN);
    lv_obj_clear_flag(batt_nub, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(batt_nub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align_to(batt_nub, batt_body, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    network_load();
    build_main();
    build_list();
    build_keypad();
    build_result();
    build_qr();
    build_wifi();
    build_wpass();
    build_net();
    build_share();
    build_dsqr();
    build_settings();
    build_screen();
    build_power();
    build_tset();
    build_name();
    build_about();
    refresh_network_label();
    lv_timer_create(role_timer_cb, 2000, NULL);
    lv_timer_ready(lv_timer_create(batt_timer_cb, 10000, NULL));
    show_panel(panel_main);
    bsp_display_unlock();

    /*
     * 28 KB. Measured: a real ePSKc join peaks at ~23.3 KB of stack, leaving
     * only 1240 bytes free at 24 KB. Nearly all of that is mbedTLS's EC-JPAKE
     * bignum arithmetic, not this code -- the CoAP buffers are already static
     * -- so it cannot be trimmed from our side. Task stacks must come from
     * internal RAM, which is scarce here, so this buys a ~5 KB margin rather
     * than a generous one. Do not lower it: an overflow here corrupts silently
     * and faults minutes later somewhere unrelated.
     */
    xTaskCreate(worker, "epskc_worker", 28672, NULL, 5, NULL);

    s_ready = true;
    ESP_LOGI(TAG, "display up");
    return ESP_OK;
}

void ui_set_wifi(const char *ssid, const char *ip)
{
    if (!s_ready) {
        return;
    }
    char buf[96];
    s_have_wifi = ip && ip[0] && ip[0] != '-';
    snprintf(s_ip_str, sizeof(s_ip_str), "%s", s_have_wifi ? ip : "-");
    if (s_have_wifi) {
        snprintf(buf, sizeof(buf), "%s  |  %s", ssid ? ssid : "?", ip);
    } else {
        snprintf(buf, sizeof(buf), "wifi: not connected");
    }
    bsp_display_lock(0);
    lv_label_set_text(lbl_wifi, buf);
    bsp_display_unlock();
}

void ui_set_status(ui_state_t state, const char *text)
{
    if (!s_ready) {
        return;
    }
    uint32_t colour = state == UI_STATE_BUSY    ? COL_ACCENT
                      : state == UI_STATE_OK    ? COL_OK
                      : state == UI_STATE_ERROR ? COL_ERROR
                                                : COL_TEXT;
    bsp_display_lock(0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(colour), LV_PART_MAIN);
    lv_label_set_text(lbl_status, text);
    bsp_display_unlock();
}

void ui_show_dataset(const char *net_name, int channel, uint16_t panid)
{
    finish_join(true, net_name, channel, panid);
    bsp_display_lock(0);
    show_panel(panel_result);
    bsp_display_unlock();
}

void ui_clear_dataset(void) { }

bool ui_defer_br_start(esp_netif_t *backbone)
{
    if (!s_ready || s_jobs == NULL || backbone == NULL) {
        return false;
    }
    s_br_backbone = backbone;
    job_t j = JOB_BR_START;
    return xQueueSend(s_jobs, &j, 0) == pdTRUE;
}

bool ui_run_on_worker(ui_worker_fn fn, void *arg, uint32_t timeout_ms)
{
    if (!s_ready || s_jobs == NULL || fn == NULL) {
        return false;
    }
    if (s_call_done == NULL) {
        s_call_done = xSemaphoreCreateBinary();
        if (s_call_done == NULL) {
            return false;
        }
    }
    /* Serialise: the slot below is shared, and the worker runs one job at a
     * time anyway. */
    if (s_call_busy) {
        return false;
    }
    s_call_busy = true;
    s_call_fn = fn;
    s_call_arg = arg;

    job_t j = JOB_CALL;
    bool ok = xQueueSend(s_jobs, &j, 0) == pdTRUE &&
              xSemaphoreTake(s_call_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    s_call_busy = false;
    return ok;
}

bool ui_run_camtest(void)
{
    if (!s_ready || s_jobs == NULL) {
        return false;
    }
    if (s_camtest_done == NULL) {
        s_camtest_done = xSemaphoreCreateBinary();
        if (s_camtest_done == NULL) {
            return false;
        }
    }
    job_t j = JOB_CAMTEST;
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
        return false;
    }
    /* The selftest captures 40 frames with a decode attempt on each, so it can
     * legitimately take tens of seconds. */
    return xSemaphoreTake(s_camtest_done, pdMS_TO_TICKS(120000)) == pdTRUE;
}
