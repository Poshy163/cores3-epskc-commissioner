"""Exercise production authenticated HTTP handlers and queued reboot behavior.

Only unrelated server setup is omitted. Actual body parsing, auth gate,
handlers, queue helper and reboot worker functions compile unchanged.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def extract_function(source, name):
    # Firmware function closing braces start in column zero. This preserves
    # whole bodies (including all branches), rather than copying their logic.
    import re
    match = re.search(r'^static [^\n]+\b' + re.escape(name) + r'\([^;]*?\)\s*\{',
                      source, re.MULTILINE)
    assert match, name
    end = source.index('\n}\n', match.start()) + 3
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'main/otbr_rest.c').read_text(encoding='utf-8')
    start = source.index('typedef enum {\n    REST_CONTROL_RUNNING,')
    end = source.index('static bool request_control(rest_control_type_t type, bool on);', start)
    declarations = source[start:end] + 'static bool request_control(rest_control_type_t type, bool on);\n'
    functions = ['send_json', 'recv_body', 'device_authorized', 'h_device_status',
                 'h_device_reboot', 'perform_remote_reboot', 'request_control']
    production = ('#include <stdatomic.h>\n#include "remote_http_sdk.h"\n'
                  '#include "remote_admin.h"\n#include "device_status.h"\n'
                  '#include "otbr_rest_wire.h"\nstatic const char *TAG = "otbr_rest";\n' +
                  declarations + '\n'.join(extract_function(source, name) for name in functions))
    compiler = [args.cc] + (['cc'] if Path(args.cc).stem == 'zig' else [])
    with tempfile.TemporaryDirectory(prefix='remote-http-tests-') as tmp:
        tmp = Path(tmp)
        (tmp / 'esp_err.h').write_text('#include "remote_http_sdk.h"\n', encoding='utf-8')
        (tmp / 'remote_http_production.inc').write_text(production, encoding='utf-8')
        binary = tmp / 'test-remote-http.exe'
        subprocess.run(compiler + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-I', str(tmp), '-I', str(root / 'tools/tests'), '-I', str(root / 'main'),
                       str(root / 'tools/tests/remote_http_harness.c'), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
        assert not result.stderr
        for line in result.stdout.splitlines():
            assert line.startswith('PASS '), 'unexpected output'
            assert not any(len(word) >= 64 and all(c in '0123456789abcdef' for c in word)
                           for word in line.split()), 'token disclosure'
        print(result.stdout, end='')


if __name__ == '__main__':
    main()
