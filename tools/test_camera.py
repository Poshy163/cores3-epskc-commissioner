"""Check production camera ownership and normal/mirrored QR decoding.

The default checks need only a C compiler. --quirc-dir additionally exercises
the production decode helper with real quirc and its bundled PGM fixture.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def compile_run(compiler, source, directory, name, extra=()):
    test = directory / (name + '.c')
    test.write_text(source)
    binary = directory / (name + '.exe')
    subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                              str(test)] + list(extra) + ['-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--quirc-dir', type=Path)
    args = parser.parse_args()
    compiler = [args.cc, 'cc'] if Path(args.cc).stem == 'zig' else [args.cc]
    source = (ROOT / 'main/qrscan.c').read_text()
    start = source.index('static quirc_decode_error_t decode_candidate(')
    decode = source[start:source.index('bool qrscan_poll(', start)]
    boot = (ROOT / 'main/main.c').read_text()
    start = boot.index('static void do_camera_probe(')
    probe = boot[start:boot.index('void app_main(', start)]
    start = boot.index('    esp_err_t camera_probe_result = ESP_FAIL;')
    request = boot[start:boot.index('    /* Start the REST lifecycle task', start)]

    with tempfile.TemporaryDirectory(prefix='epskc-camera-') as temporary:
        directory = Path(temporary)
        compile_run(compiler, r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int quirc_decode_error_t;
enum { QUIRC_SUCCESS, QUIRC_ERROR_DATA_ECC, QUIRC_ERROR_FORMAT_ECC };
struct quirc_code { bool mirrored; };
struct quirc_data { int payload; };
static int calls, flips, initial_error;
static quirc_decode_error_t quirc_decode(const struct quirc_code *code, struct quirc_data *data) {
    ++calls;
    if (initial_error) return initial_error;
    if (code->mirrored) return QUIRC_ERROR_DATA_ECC;
    data->payload = 123456789;
    return QUIRC_SUCCESS;
}
static void quirc_flip(struct quirc_code *code) { ++flips; code->mirrored = !code->mirrored; }
''' + decode + r'''
int main(void) {
    struct quirc_code code = {false};
    struct quirc_data data = {0};
    assert(decode_candidate(&code, &data) == QUIRC_SUCCESS);
    assert(calls == 1 && flips == 0 && data.payload == 123456789);
    calls = flips = 0; code.mirrored = true; data.payload = 0;
    assert(decode_candidate(&code, &data) == QUIRC_SUCCESS);
    assert(calls == 2 && flips == 1 && data.payload == 123456789);
    calls = flips = 0; initial_error = QUIRC_ERROR_FORMAT_ECC;
    assert(decode_candidate(&code, &data) == QUIRC_ERROR_FORMAT_ECC);
    assert(calls == 1 && flips == 0);
    calls = flips = 0; initial_error = QUIRC_ERROR_DATA_ECC;
    assert(decode_candidate(&code, &data) == QUIRC_ERROR_DATA_ECC);
    assert(calls == 2 && flips == 1); // corrupt candidates get one retry only
    puts("PASS production QR decode: normal, mirrored ECC retry, unrelated errors");
}
''', directory, 'decode')
        compile_run(compiler, r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL };
#define ESP_LOGW(...) ((void)0)
static bool worker_task, active_user_preview, reject_request;
static int starts, stops, probe_error;
static esp_err_t qrscan_start(void) {
    assert(worker_task && !active_user_preview); ++starts; return probe_error;
}
static void qrscan_stop(void) { assert(worker_task); ++stops; }
static bool ui_run_on_worker(void (*fn)(void *), void *arg, uint32_t timeout_ms) {
    assert(!worker_task && timeout_ms == 30000);
    if (reject_request) return false;
    // The existing preview finishes before the queued probe owns acquisition.
    active_user_preview = false;
    worker_task = true; fn(arg); worker_task = false;
    return true;
}
''' + probe + '\nstatic void boot_probe_request(void) {\n' + request + r'''
}
int main(void) {
    active_user_preview = true;
    boot_probe_request();
    assert(starts == 1 && stops == 1 && !active_user_preview);
    probe_error = ESP_FAIL;
    boot_probe_request();
    assert(starts == 2 && stops == 1);
    reject_request = true; active_user_preview = true;
    boot_probe_request();
    assert(starts == 2 && stops == 1 && active_user_preview);
    puts("PASS production boot probe: worker ownership, failures, rejected queue preserves preview");
}
''', directory, 'ownership')
        if args.quirc_dir:
            quirc = args.quirc_dir.resolve()
            library = quirc / 'quirc/lib'
            pgm = (quirc / 'test/test_qrcode.pgm').read_bytes()
            magic, dimensions, maximum, image = pgm.split(b'\n', 3)
            assert magic == b'P5' and maximum == b'255'
            width, height = map(int, dimensions.split())
            assert len(image) == width * height
            fixture = 'static const unsigned char image[] = {' + ','.join(map(str, image)) + '};\n'
            compile_run(compiler, r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "quirc.h"
''' + decode + fixture + f'\nenum {{ WIDTH = {width}, HEIGHT = {height} }};\n' + r'''
int main(void) {
    struct quirc *q = quirc_new();
    assert(q && quirc_resize(q, WIDTH, HEIGHT) == 0);
    // Decode actual upright and horizontally mirrored camera pixels.
    for (int mirrored = 0; mirrored < 2; ++mirrored) {
        unsigned char *pixels = quirc_begin(q, NULL, NULL);
        for (int y = 0; y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                pixels[y * WIDTH + x] = image[y * WIDTH + (mirrored ? WIDTH - 1 - x : x)];
            }
        }
        quirc_end(q);
        assert(quirc_count(q) == 1);
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(q, 0, &code);
        assert(decode_candidate(&code, &data) == QUIRC_SUCCESS);
        assert(data.payload_len == 13 && !memcmp(data.payload, "test of quirc", 13));
    }
    quirc_destroy(q);
    puts("PASS real quirc fixture: upright and horizontally mirrored image payloads match");
}
''', directory, 'real_quirc', ['-Wno-unused-parameter', '-Wno-sign-compare',
                '-I', str(library)] +
                [str(library / name) for name in ('quirc.c', 'identify.c', 'decode.c', 'version_db.c')] + ['-lm'])


if __name__ == '__main__':
    main()
