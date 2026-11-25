"""Run the production Thread dataset join and REST hex entry point with a fake radio."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'main/thread.c').read_text(encoding='utf-8')
    join = source[source.index('static void restore_border_agent_id('):
                  source.index('esp_err_t thread_form_network(')]
    hex_entry = source[source.index('static int hex_nibble('):
                       source.index('esp_err_t thread_set_enabled(')]
    compiler = [args.cc] + (['cc'] if Path(args.cc).stem == 'zig' else [])
    with tempfile.TemporaryDirectory(prefix='thread-join-tests-') as directory:
        directory = Path(directory)
        (directory / 'thread_join_production.inc').write_text(join + hex_entry, encoding='utf-8')
        exe = directory / 'thread-join.exe'
        subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-I', str(directory), str(root / 'tools/tests/thread_join_harness.c'),
                       '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
