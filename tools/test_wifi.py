"""Run the actual Wi-Fi transaction/scan functions with a fault-injected driver.

Requires a host C compiler: python tools/test_wifi.py --cc gcc
Zig is also supported: --cc path/to/zig.exe (automatically adds `cc`).
No serial port or device is used. Source is extracted, not reimplemented.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (args.source or root / 'main/main.c').read_text(encoding='utf-8')
    start = source.index('static esp_err_t wifi_config_from_credentials(')
    end = source.index('static int cmd_wifi(', start)
    compiler = [args.cc]
    if Path(args.cc).stem == 'zig':
        compiler.append('cc')
    with tempfile.TemporaryDirectory(prefix='epskc-wifi-test-') as tmp:
        tmp = Path(tmp)
        port_helper = ''
        if 'static bool parse_udp_port(' in source:
            port_start = source.index('static bool parse_udp_port(')
            port_end = source.index('static int cmd_join(', port_start)
            port_helper = '#define TEST_UDP_PORT 1\n' + source[port_start:port_end]
        (tmp / 'wifi_production.inc').write_text(source[start:end] + port_helper, encoding='utf-8')
        shutil.copyfile(root / 'tools/tests/wifi_harness.c', tmp / 'test.c')
        exe = tmp / 'test.exe'
        subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                                   '-Wno-unused-parameter', '-Wno-unused-function', str(tmp / 'test.c'),
                                   '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        # Exercise the actual status publisher plus event handler, including
        # a driver query that races a disconnect/reconnect on the other task.
        status_start = source.index('static portMUX_TYPE s_wifi_status_lock')
        status_end = source.index('/*\n * Fast at first', status_start)
        event_start = source.index('static void on_wifi(')
        event_end = source.index('/* Stop the current station attempt', event_start)
        (tmp / 'wifi_status_production.inc').write_text(
            source[status_start:status_end] + source[event_start:event_end], encoding='utf-8')
        (tmp / 'esp_err.h').write_text(
            '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n', encoding='utf-8')
        shutil.copyfile(root / 'tools/tests/wifi_status_harness.c', tmp / 'status.c')
        status_exe = tmp / 'status.exe'
        subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                                   '-Wno-unused-parameter', '-Wno-unused-function',
                                   '-I', str(tmp), '-I', str(root / 'main'),
                                   str(tmp / 'status.c'), '-o', str(status_exe)], check=True)
        subprocess.run([str(status_exe)], check=True)


if __name__ == '__main__':
    main()
