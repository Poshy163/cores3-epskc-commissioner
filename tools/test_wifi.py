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
    start = source.index('esp_err_t app_wifi_join(')
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


if __name__ == '__main__':
    main()
