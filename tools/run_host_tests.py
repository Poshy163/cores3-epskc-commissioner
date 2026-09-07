"""Run firmware host regressions without a device or ESP-IDF.

python tools/run_host_tests.py --cc gcc
python tools/run_host_tests.py --cc path/to/zig.exe
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc', help='host C compiler or Zig executable')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    for script in ['test_wifi.py', 'test_ui_power_settings.py']:
        subprocess.run([sys.executable, str(root / 'tools' / script), '--cc', args.cc], check=True)
    compiler = [args.cc]
    if Path(args.cc).stem == 'zig':
        compiler.append('cc')
    with tempfile.TemporaryDirectory(prefix='epskc-codec-tests-') as tmp:
        for name in ['test_otbr_rest_wire', 'test_meshcop_codec']:
            binary = Path(tmp) / (name + '.exe')
            subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                           str(root / 'tools' / (name + '.c')), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            print(f'PASS {name}', flush=True)


if __name__ == '__main__':
    main()
