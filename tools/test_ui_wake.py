"""Compile the production battery poll and wake handler against a fake display."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = (ROOT / 'main/ui.c').read_text()
    poll = source[source.index('static void batt_timer_cb('):source.index('/* Runs on the LVGL task (lock already held)')]
    start = source.index('static void on_wake(lv_event_t *e)\n{')
    wake = source[start:source.index('/* -- screen -- */', start)]
    with tempfile.TemporaryDirectory(prefix='ui-wake-') as directory:
        temp = Path(directory)
        (temp / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n')
        test = temp / 'test.c'
        test.write_text(r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "power.h"
typedef int lv_timer_t;
typedef int lv_event_t;
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_PART_MAIN 0
#define LV_SYMBOL_CHARGE "bolt "
#define COL_OK 1
#define COL_ERROR 2
#define COL_ACCENT 3
#define COL_DIM 4
#define BATT_INNER_W 20
static bool s_asleep, s_dimmed, s_on_external_power;
static int body, nub, fill, label, panel, shield;
static int *batt_body=&body, *batt_nub=&nub, *batt_fill=&fill, *lbl_batt=&label;
static int *panel_power=&panel, *s_sleep_shield=&shield;
static struct { int brightness; } settings={95};
#define settings_get() (&settings)
static power_status_t pmic;
static int read_error, reads, logs, reports, flushed, lit, refreshed, deleted;
static char label_text[40], rendered[40];
static const char *expected;
static esp_err_t power_read_test(power_status_t *out) { ++reads; *out=pmic; return read_error; }
#define power_read power_read_test
static void power_log_note(const power_status_t *s,int e,unsigned screen) {
    (void)s;(void)e; ++logs; assert(screen == (unsigned)(s_asleep ? 0 : settings.brightness));
}
static void matter_note_power(const power_status_t *s,int e) {
    ++reports; if(!e) assert(s->percent==pmic.percent);
}
static void app_wifi_set_power_profile(bool usb) { assert(usb==pmic.vbus); }
void power_note_sample(int percent,bool on_battery) {
    if(read_error) assert(percent==-1 && !on_battery);
}
static void lv_obj_add_flag(int *o,int flag) { (void)flag; *o=1; }
static void lv_obj_clear_flag(int *o,int flag) { (void)flag; *o=0; }
static bool lv_obj_has_flag(int *o,int flag) { (void)flag; return *o!=0; }
static void lv_label_set_text(int *o,const char *text) { (void)o; snprintf(label_text,sizeof(label_text),"%s",text); }
static void lv_label_set_text_fmt(int *o,const char *fmt,...) {
    (void)o; va_list a; va_start(a,fmt); vsnprintf(label_text,sizeof(label_text),fmt,a); va_end(a);
}
static int lv_color_hex(int c) { return c; }
static void lv_obj_set_style_border_color(int *o,int c,int p) { (void)o;(void)c;(void)p; }
static void lv_obj_set_style_bg_color(int *o,int c,int p) { (void)o;(void)c;(void)p; }
static void lv_obj_set_width(int *o,int w) { (void)o;(void)w; }
static void refresh_power_info(void) { assert(!lit); ++refreshed; }
static void lv_refr_now(void *display) {
    (void)display; assert(!lit); ++flushed; strcpy(rendered,label_text);
}
static void bsp_display_brightness_set(int v) {
    assert(v==95 && flushed && !s_asleep && !s_dimmed);
    assert(!strcmp(rendered,expected)); // fresh frame before backlight
    if(!panel) assert(refreshed);
    ++lit;
}
static void lv_obj_delete_async(int *obj) { assert(obj==&shield); ++deleted; }
''' + poll + wake + r'''
int main(void) {
    pmic.present=true; pmic.percent=100; pmic.vbus=true;
    batt_timer_cb(NULL); assert(!strcmp(label_text,"bolt 100%"));
    // Screen sleeps on USB, then unplugged samples continue for hours.
    s_asleep=true; pmic.percent=80; pmic.vbus=false;
    for(int i=0;i<10;++i) batt_timer_cb(NULL);
    assert(logs==11 && reports==11 && !s_on_external_power);
    assert(!strcmp(label_text,"bolt 100%")); // intentionally not drawing asleep
    expected="80%"; s_dimmed=true; on_wake(NULL);
    assert(lit==1 && refreshed==1 && deleted==1 && !s_sleep_shield);
    assert(reads==12 && logs==12 && reports==12);
    int before=reads; on_wake(NULL); assert(reads==before && lit==1);
    // Failure on wake must not expose the last good (but stale) percentage.
    s_asleep=true; s_dimmed=true; s_sleep_shield=&shield;
    lit=flushed=refreshed=0; read_error=-1; expected="--";
    on_wake(NULL); assert(body==1 && nub==1 && lit==1);
    // The power page can be hidden; header freshness still applies.
    s_asleep=true; s_sleep_shield=&shield; panel=1;
    lit=flushed=refreshed=0; read_error=0; pmic.percent=79; expected="79%";
    on_wake(NULL); assert(lit==1 && !refreshed);
    // Unknown/absent readings also clear text in the ordinary periodic poll.
    pmic.present=false; batt_timer_cb(NULL); assert(!strcmp(label_text,"--"));
    puts("PASS real UI background polling, wake freshness, render-before-light and failed reads");
}
''')
        command = [args.cc, 'cc'] if Path(args.cc).stem == 'zig' else [args.cc]
        binary = temp / 'test.exe'
        subprocess.run(command + ['-std=c11','-Wall','-Wextra','-Werror',
                                 '-I',str(temp),'-I',str(ROOT/'main'),str(test),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
