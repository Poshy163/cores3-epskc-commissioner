"""Fault-inject the production remote-admin token and atomic NVS state module.

Runs on the host without a device, network, ESP-IDF, or real tokens. The NVS
stub checks that flash calls happen under the mutation mutex and outside the
snapshot critical section; readers inspect state during pending writes.
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
    with tempfile.TemporaryDirectory(prefix='remote-admin-tests-') as tmp:
        tmp = Path(tmp)
        for name in ['esp_err.h', 'esp_random.h', 'nvs.h',
                     'freertos/FreeRTOS.h', 'freertos/semphr.h']:
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "remote_admin_sdk.h"\n', encoding='utf-8')
        binary = tmp / 'test-remote-admin.exe'
        subprocess.run(compiler + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-I', str(tmp), '-I', str(root / 'tools/tests'),
                       '-I', str(root / 'main'),
                       str(root / 'tools/tests/remote_admin_harness.c'),
                       '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
        # Token values belong only in the fixture's RAM, never test output.
        for line in result.stdout.splitlines():
            assert line.startswith('PASS '), 'unexpected test output'
            assert not any(len(word) >= 64 and all(c in '0123456789abcdef' for c in word)
                           for word in line.split()), 'token disclosure in test output'
        assert not result.stderr, 'unexpected test stderr'
        print(result.stdout, end='')


if __name__ == '__main__':
    main()
