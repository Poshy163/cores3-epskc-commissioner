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

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "discover.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "meshcop.h"
#include "nvs.h"
#include "power.h"
#include "qrscan.h"
#include "thread.h"
#include "wifi_ctl.h"

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

#define COL_BG      0x101410
#define COL_PANEL   0x1B2119
#define COL_TEXT    0xE4E8DF
#define COL_DIM     0x8A937F
#define COL_ACCENT  0xD9904F
#define COL_OK      0x4ECBB8
#define COL_ERROR   0xF0798F

typedef enum { JOB_SCAN, JOB_JOIN, JOB_QR, JOB_WIFI_SCAN, JOB_WIFI_JOIN } job_t;

static lv_obj_t *panel_main, *panel_list, *panel_keypad, *panel_result, *panel_qr;
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

/* Runs on the LVGL task. The battery lives on the Base DIN behind its power
 * switch, so the PMIC may legitimately see no battery at all -- hide the
 * outline entirely rather than show a bogus 0%. */
static void batt_timer_cb(lv_timer_t *t)
{
    (void) t;
    power_status_t ps;
    if (power_read(&ps) != ESP_OK || !ps.present || ps.percent < 0) {
        lv_obj_add_flag(batt_body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(batt_nub, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(batt_nub, LV_OBJ_FLAG_HIDDEN);

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
}

/* Caller must hold the LVGL lock. */
static void refresh_network_label(void)
{
    if (!lbl_network) {
        return;
    }
    char buf[96];
    if (s_have_net) {
        const char *role = thread_role();
        snprintf(buf, sizeof(buf), "%s  ch %d  pan 0x%04x\nthread: %s",
                 s_net_name, s_net_channel, s_net_panid, role);
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
    lv_obj_set_size(p, BSP_LCD_H_RES, BSP_LCD_V_RES - 44);
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
    (void) e;
    thread_forget();
    network_erase_nvs();
    refresh_network_label();
    lv_label_set_text(lbl_status, "Credentials cleared");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT), LV_PART_MAIN);
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
        lv_label_set_text(lbl_result_body,
                          "See serial log for the cause.\n"
                          "Keys are single-use and expire.");
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

static void build_main(void)
{
    panel_main = mk_panel();

    lbl_status = mk_label(panel_main, &lv_font_montserrat_20, COL_TEXT, "Starting...");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 8);

    /* Persisted Thread network, so the device is useful at a glance. */
    lbl_network = mk_label(panel_main, &lv_font_montserrat_14, COL_DIM, "no credentials yet");
    lv_obj_align(lbl_network, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *b = mk_button(panel_main, "Scan for routers", on_scan, COL_ACCENT, 240, 52);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -46);

    lv_obj_t *f = mk_button(panel_main, "Forget network", on_forget, COL_DIM, 178, 34);
    lv_obj_align(f, LV_ALIGN_BOTTOM_LEFT, 6, -6);

    lv_obj_t *w = mk_button(panel_main, "Wi-Fi", on_wifi_open, COL_DIM, 110, 34);
    lv_obj_align(w, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
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
    lv_obj_t *bk = mk_button(panel_wifi, "Back", on_back_main, COL_DIM, 96, 40);
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
    refresh_network_label();
    lv_timer_create(role_timer_cb, 2000, NULL);
    lv_timer_ready(lv_timer_create(batt_timer_cb, 10000, NULL));
    show_panel(panel_main);
    bsp_display_unlock();

    /* 24 KB. Task stacks must be internal RAM, so 48 KB was expensive -- and
     * the same handshake already runs fine on the 12 KB console task, so the
     * larger figure was never justified. */
    xTaskCreate(worker, "epskc_worker", 24576, NULL, 5, NULL);

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
