"""Fault-inject the actual ePSKc client with Mbed TLS 3 and 4 API surfaces.

No sockets or device are used. The fake clock advances on FreeRTOS yields.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = [args.cc]
    if Path(args.cc).stem == 'zig':
        compiler.append('cc')
    with tempfile.TemporaryDirectory(prefix='meshcop-transport-tests-') as tmp:
        tmp = Path(tmp)
        for name in ['esp_err.h', 'esp_heap_caps.h', 'esp_log.h', 'esp_timer.h',
                     'freertos/FreeRTOS.h', 'freertos/task.h', 'lwip/netdb.h',
                     'lwip/sockets.h', 'mbedtls/version.h', 'mbedtls/ctr_drbg.h',
                     'mbedtls/entropy.h', 'mbedtls/net_sockets.h', 'mbedtls/ssl.h',
                     'mbedtls/ssl_ciphersuites.h', 'psa/crypto.h']:
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "meshcop_sdk.h"\n', encoding='utf-8')
        for major in [3, 4]:
            exe = tmp / f'test-mbedtls-{major}.exe'
            subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                           '-Wno-unused-parameter', f'-DMBEDTLS_VERSION_MAJOR={major}',
                           '-I', str(tmp), '-I', str(root / 'tools/tests'),
                           '-I', str(root / 'main'),
                           str(root / 'tools/tests/meshcop_transport_harness.c'),
                           '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
