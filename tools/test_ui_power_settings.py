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
static int failed_reg = -1, soc = 100, adc = 4118, reads;
static esp_err_t ensure_init(void) { return ESP_OK; }
static esp_err_t reg_read(uint8_t reg, uint8_t *out) {
    reads++;
    if (reg == failed_reg) return ESP_FAIL;
    *out = reg == AXP2101_REG_STATUS1 ? 0x28 : reg == AXP2101_REG_STATUS2 ? 4 : soc;
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
    assert(power_read(&out) == ESP_OK && out.percent_estimated);
    assert(out.percent > 0 && out.percent <= 100);
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


if __name__ == "__main__":
    main()
