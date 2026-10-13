#!/usr/bin/env python3
"""Host regression checks for UI/QR/power/settings changes.

Runs without ESP-IDF. It compiles real settings.c and power_read logic against
NVS and PMIC stubs. QR cancellation/rendering remains a device test.
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_actual_power(cc: list[str]) -> None:
    source = (ROOT / 'main/power.c').read_text()
    start = source.index('static const char *charge_detail_str(')
    end = source.index('/* ---------------- ESP32-S3 die temperature', start)
    constants = '\n'.join(line for line in source.splitlines()
                          if line.startswith('#define AXP2101_REG_'))
    with tempfile.TemporaryDirectory(prefix='power-test-') as temp:
        temp_path = pathlib.Path(temp)
        (temp_path / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG 0x102\n')
        test = temp_path / 'test.c'
        test.write_text('''#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include "power.h"
''' + constants + '''
static int failed_reg = -1, soc = 100, adc = 4118, reads, st1 = 0x28, st2 = 4;
static esp_err_t ensure_init(void) { return ESP_OK; }
static esp_err_t reg_read(uint8_t reg, uint8_t *out) {
    reads++;
    if (reg == failed_reg) return ESP_FAIL;
    *out = reg == AXP2101_REG_STATUS1 ? st1 : reg == AXP2101_REG_STATUS2 ? st2 : soc;
    return ESP_OK;
}
static int read_adc(uint8_t reg, int bits) { (void)reg; (void)bits; return adc; }
''' + source[start:end] + '''
int main(void) {
    power_status_t out;
    assert(power_read(NULL) == ESP_ERR_INVALID_ARG && reads == 0);
    failed_reg = AXP2101_REG_STATUS2;
    assert(power_read(&out) == ESP_FAIL);
    assert(strcmp(out.charge_detail, "unknown") == 0 && out.percent == -1);
    failed_reg = AXP2101_REG_STATUS1; reads = 0;
    assert(power_read(&out) == ESP_FAIL && reads == 1);
    failed_reg = -1;
    assert(power_read(&out) == ESP_OK && out.full && out.present && out.vbus);
    assert(out.percent == 100 && !out.percent_estimated);
    soc = 0;
    assert(power_read(&out) == ESP_OK && out.percent == -1 && !out.percent_estimated);
    st2 = 0x22; // charging voltage must never become an open-circuit SOC
    assert(power_read(&out) == ESP_OK && out.percent == -1 && out.charging);
    st1 = 8; st2 = 4; // stale charge-done phase after unplugging
    assert(power_read(&out) == ESP_OK && out.percent_estimated);
    assert(out.percent > 0 && out.percent <= 100);
    assert(!out.full && !out.charging && strcmp(out.charge_detail, "discharging") == 0);
    adc = 3400;
    assert(power_read(&out) == ESP_OK && out.percent == 0 && !out.percent_estimated);
    soc = 255; adc = 0;
    assert(power_read(&out) == ESP_OK && out.percent == -1);
    adc = 5000;
    assert(power_read(&out) == ESP_OK && out.percent == -1);
    st1 = 0x20; soc = 100;
    assert(power_read(&out) == ESP_OK && out.percent == -1 && !out.full);
    st1 = 8; soc = 0;
    adc = -1;
    assert(power_read(&out) == ESP_OK && out.percent == -1);
    assert(out.pmic_temp_c == INT_MIN);
    return 0;
}
''')
        exe = temp_path / 'test.exe'
        subprocess.run([*cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(temp_path), '-I', str(ROOT / 'main'),
                        str(test), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS actual PMIC read errors, unknown telemetry and gauge fallback')


def test_discharge_history(cc: list[str]) -> None:
    source = (ROOT / 'main/power.c').read_text()
    source = source[source.index('#define HIST_N'):]
    with tempfile.TemporaryDirectory(prefix='discharge-test-') as temp:
        test = pathlib.Path(temp) / 'test.c'
        test.write_text('''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int64_t now;
static int64_t esp_timer_get_time(void) { return now; }
''' + source + '''
static void run(int start, int drop) {
    power_note_sample(-1, false);
    for (int i = 0; i <= 10; ++i) {
        now += 60000000;
        power_note_sample(start - (i * drop / 10), true);
    }
}
int main(void) {
    float rate; int mins;
    run(80, 1);
    assert(!power_discharge_rate(&rate, &mins));
    run(80, 2);
    assert(power_discharge_rate(&rate, &mins));
    assert(rate > 11.99f && rate < 12.01f && mins == 390);
    power_note_sample(78, false); // USB seen between minute samples
    assert(!power_discharge_rate(&rate, &mins));
    run(80, 2); power_note_sample(-1, true);
    assert(!power_discharge_rate(&rate, &mins));
    run(80, 2); now += 90000001;
    assert(!power_discharge_rate(&rate, &mins));
    power_note_sample(77, true);
    assert(!power_discharge_rate(&rate, &mins));
    run(80, 2); power_note_sample(79, true); // gauge correction
    assert(!power_discharge_rate(&rate, &mins));
    power_note_sample(-1, false);
    for (int i = 0; i < 80; ++i) {
        now += 1000000; power_note_sample(70, true);
    }
    assert(s_hist_n == 2); // frequent polls do not shorten the window
    run(80, 2);
    for (int i = 0; i < 20; ++i) {
        now += 60000000; power_note_sample(77 - i, true);
    }
    assert(power_discharge_rate(&rate, &mins));
    assert(rate > 59.99f && rate < 60.01f && mins == 58);
    return 0;
}
''')
        exe = pathlib.Path(temp) / 'test.exe'
        subprocess.run([*cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS actual discharge history continuity, quantisation, stale data and wraparound')


def test_power_io(cc: list[str]) -> None:
    source = (ROOT / 'main/power.c').read_text()
    source = source[source.index('static const char *TAG'):source.index('static const char *charge_detail_str')]
    with tempfile.TemporaryDirectory(prefix='power-io-test-') as temp:
        test = pathlib.Path(temp) / 'test.c'
        test.write_text('''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define I2C_ADDR_BIT_LEN_7 7
#define ESP_LOGW(tag, ...) ((void)(tag))
typedef int esp_err_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
static uint8_t registers[256];
static int adds, fail_read, fail_write, read_length;
static esp_err_t bsp_i2c_init(void) { return ESP_OK; }
static void *bsp_i2c_get_handle(void) { return (void *)1; }
static esp_err_t i2c_master_bus_add_device(void *bus, const i2c_device_config_t *cfg, void **out) {
    (void)bus; (void)cfg; ++adds; *out = (void *)2; return ESP_OK;
}
static esp_err_t i2c_master_transmit_receive(void *dev, const uint8_t *reg, size_t tx,
                                           uint8_t *out, size_t rx, int timeout) {
    (void)dev; (void)timeout; assert(tx == 1); read_length = (int)rx;
    if (fail_read) return ESP_FAIL;
    for (size_t i = 0; i < rx; ++i) out[i] = registers[*reg + i];
    return ESP_OK;
}
static esp_err_t i2c_master_transmit(void *dev, const uint8_t *data, size_t len, int timeout) {
    (void)dev; (void)timeout; assert(len == 2);
    if (fail_write) return ESP_FAIL;
    registers[data[0]] = data[1]; return ESP_OK;
}
''' + source + '''
int main(void) {
    registers[0x30] = 0x82; // preserve unrelated ADC bits
    fail_read = 1;
    assert(ensure_init() == ESP_FAIL && adds == 1 && !s_adc_ready);
    fail_read = 0; fail_write = 1;
    assert(ensure_init() == ESP_FAIL && adds == 1 && !s_adc_ready);
    fail_write = 0;
    assert(ensure_init() == ESP_OK && adds == 1 && s_adc_ready);
    assert(registers[0x30] == 0x9f);
    registers[0x34] = 0xf0; registers[0x35] = 0x15;
    assert(read_adc(0x34, 5) == 4117 && read_length == 2);
    fail_read = 1;
    assert(read_adc(0x34, 5) == -1);
    return 0;
}
''')
        exe = pathlib.Path(temp) / 'test.exe'
        subprocess.run([*cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS actual ADC burst reads and failed setup retry without duplicate device')


def test_actual_settings(cc: list[str]) -> None:
    with tempfile.TemporaryDirectory(prefix="ui-power-settings-") as temp:
        temp_path = pathlib.Path(temp)
        (temp_path / "esp_err.h").write_text("""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
static inline const char *esp_err_to_name(esp_err_t e) { return e ? "ERR" : "OK"; }
""")
        (temp_path / "esp_log.h").write_text("#pragma once\n#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)\n")
        (temp_path / "nvs.h").write_text("""#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
esp_err_t nvs_erase_all(nvs_handle_t);
""")
        test = temp_path / "test.c"
        test.write_text(f'''#include <string.h>
#include "{(ROOT / "main/settings.c").as_posix()}"
static const char *key; static uint8_t value; static int fail_set, commits;
esp_err_t nvs_open(const char *n,int m,nvs_handle_t *h){{(void)n;(void)m;*h=1;return ESP_OK;}}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v){{(void)h;if(key&&strcmp(key,k)==0){{*v=value;return ESP_OK;}}return ESP_ERR_NVS_NOT_FOUND;}}
esp_err_t nvs_set_u8(nvs_handle_t h,const char *k,uint8_t v){{(void)h;(void)k;(void)v;return fail_set?ESP_FAIL:ESP_OK;}}
esp_err_t nvs_get_str(nvs_handle_t h,const char*k,char*v,size_t*n){{(void)h;(void)k;(void)v;(void)n;return ESP_ERR_NVS_NOT_FOUND;}}
esp_err_t nvs_set_str(nvs_handle_t h,const char*k,const char*v){{(void)h;(void)k;(void)v;return ESP_OK;}}
esp_err_t nvs_commit(nvs_handle_t h){{(void)h;commits++;return ESP_OK;}} void nvs_close(nvs_handle_t h){{(void)h;}}
esp_err_t nvs_erase_all(nvs_handle_t h){{(void)h;return ESP_OK;}}
int main(void){{key="restapi";value=2;s_cfg.rest_api=1;settings_load();if(!s_cfg.rest_api)return 1;value=0;settings_load();if(s_cfg.rest_api)return 2;fail_set=1;settings_save();return commits?3:0;}}
''')
        exe = temp_path / "test.exe"
        subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(temp_path), "-I", str(ROOT / "main"), str(test), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS actual settings invalid boolean and failed-save behavior")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", help="host C compiler (defaults to zig cc)")
    args = parser.parse_args()
    cc = args.cc or shutil.which("zig")
    if not cc:
        raise SystemExit("need a host compiler; pass --cc PATH")
    command = [cc, "cc"] if pathlib.Path(cc).name.lower().startswith("zig") else [cc]
    test_actual_settings(command)
    test_actual_power(command)
    test_discharge_history(command)
    test_power_io(command)


if __name__ == "__main__":
    main()
