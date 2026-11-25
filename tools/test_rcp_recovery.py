"""Exercise the production radio recovery path across simulated restarts."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = [args.cc] + (['cc'] if Path(args.cc).stem == 'zig' else [])
    with tempfile.TemporaryDirectory(prefix='rcp-recovery-') as work:
        work = Path(work)
        for name in ('driver/gpio.h', 'driver/uart.h', 'esp_err.h', 'esp_attr.h',
                     'esp_log.h', 'esp_openthread_spinel.h', 'esp_rcp_update.h',
                     'esp_spiffs.h', 'esp_system.h', 'esp_timer.h', 'esp_netif.h', 'nvs.h'):
            header = work / name
            header.parent.mkdir(exist_ok=True)
            header.write_text('#include "rcp_recovery_sdk.h"\n', encoding='utf-8')
        binary = work / 'rcp_recovery.exe'
        subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-I', str(work), '-I', str(root / 'tools/tests'), '-I', str(root / 'main'),
                       str(root / 'main/rcp_update.c'), str(root / 'tools/tests/rcp_recovery_harness.c'),
                       '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        print('PASS radio update/recovery lifecycle')


if __name__ == '__main__':
    main()
