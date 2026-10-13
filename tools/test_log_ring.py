"""Exercise the real firmware log ring, including recovery across reboots,
and the PC tail tool against a device that pages, overwrites and reboots."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from logtail import Tail, validate_page

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    'esp_err.h': '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n',
    'sdkconfig.h': '#pragma once\n#define CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY 1\n',
    'esp_attr.h': '#pragma once\n#define EXT_RAM_NOINIT_ATTR\n#define EXT_RAM_BSS_ATTR\n',
    'esp_cache.h': '''#pragma once
#include <stddef.h>
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 4
#define ESP_CACHE_MSYNC_FLAG_TYPE_DATA 16
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 2
extern int msync_calls;
static inline int esp_cache_msync(void *addr, size_t size, int flags)
{ (void)addr; (void)size; (void)flags; ++msync_calls; return 0; }
''',
    'esp_log.h': '''#pragma once
#include <stdarg.h>
typedef int (*vprintf_like_t)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
''',
    'esp_memory_utils.h': '''#pragma once
#include <stdbool.h>
static inline bool esp_ptr_external_ram(const void *p) { (void)p; return true; }
''',
    'esp_random.h': '''#pragma once
#include <stddef.h>
#include <stdint.h>
extern uint8_t next_random;
static inline void esp_fill_random(void *out, size_t n)
{ for (size_t i = 0; i < n; ++i) ((uint8_t *)out)[i] = ++next_random; }
''',
    'esp_timer.h': '''#pragma once
#include <stdint.h>
extern int64_t test_time;
static inline int64_t esp_timer_get_time(void) { return test_time; }
''',
    'freertos/FreeRTOS.h': '''#pragma once
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
''',
    'freertos/task.h': '''#pragma once
typedef void *TaskHandle_t;
extern TaskHandle_t test_task;
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return test_task; }
''',
}

TEST_C = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "log_ring.c"

int msync_calls;
uint8_t next_random;
int64_t test_time;
TaskHandle_t test_task;
static vprintf_like_t hook;
static vprintf_like_t console = vprintf;
static int custom_calls;
static FILE *json_out;

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func)
{
    vprintf_like_t old = console;
    hook = func;
    return old;
}

static int custom_console(const char *fmt, va_list args)
{
    ++custom_calls;
    char sink[512];
    return vsnprintf(sink, sizeof(sink), fmt, args);
}

static void emit(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    hook(fmt, args);
    va_end(args);
}

static void task(int n) { test_task = (TaskHandle_t)(uintptr_t)n; }

static const char *line(log_ring_boot_t boot, uint32_t seq)
{
    static log_ring_line_t out;
    assert(log_ring_get(boot, seq, &out));
    return out.text;
}

static void json(log_ring_boot_t boot, uint32_t seq, size_t cap)
{
    log_ring_line_t out;
    char buf[512];
    assert(log_ring_get(boot, seq, &out));
    size_t n = log_ring_json(&out, buf, cap);
    assert(n > 0 && n < cap && strlen(buf) == n);
    fprintf(json_out, "%s\n", buf);
}

/* Everything but the no-init ring is ordinary RAM, cleared by a reboot. */
static void reboot(void)
{
    s_cur = s_prev = NULL;
    s_latest = s_last_ms = s_prev_latest = s_prev_last_ms = 0;
    s_writeback = false;
    s_console = NULL;
    memset(s_pending, 0, sizeof(s_pending));
    s_pending_victim = 0;
    hook = NULL;
    test_time = 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    json_out = fopen(argv[1], "w");
    assert(json_out);
    log_ring_info_t info;
    log_ring_line_t out;

    /* Unused before init; power-on memory holds no previous boot. */
    assert(!log_ring_info(LOG_RING_CURRENT, &info));
    log_ring_line("ignored", 7);
    memset(&s_ring, 0xa5, sizeof(s_ring));
    assert(log_ring_init() == ESP_OK && hook == capture_vprintf);
    assert(log_ring_init() == ESP_OK);
    assert(!log_ring_info(LOG_RING_PREVIOUS, &info));
    assert(log_ring_info(LOG_RING_CURRENT, &info) && info.boot_count == 1);
    assert(info.oldest_seq == 0 && info.latest_seq == 0 && !log_ring_get(LOG_RING_CURRENT, 1, &out));

    /* An ESP_LOGx line, written back from the cache. */
    task(1);
    test_time = 5000000;   /* microseconds */
    int before = msync_calls;
    emit("W (%lu) %s: hello %d\n", 5ul, "tag", 7);
    assert(!strcmp(line(LOG_RING_CURRENT, 1), "W (5) tag: hello 7"));
    assert(msync_calls == before + 1);

    /* OpenThread's three-call line, interleaved with another task. */
    emit("W(%lu) %s:", 23591ul, "OPENTHREAD");
    task(2);
    emit("I (%d) other: whole line\n", 9);
    task(1);
    emit("[W] Mle-----------: Failed to process UDP: %s", "Duplicated");
    emit("\n");
    assert(!strcmp(line(LOG_RING_CURRENT, 2), "I (9) other: whole line"));
    assert(!strcmp(line(LOG_RING_CURRENT, 3),
                   "W(23591) OPENTHREAD:[W] Mle-----------: Failed to process UDP: Duplicated"));

    /* Blank lines are dropped; several lines in one call are split. */
    emit("\n");
    emit("one\ntwo\n");
    assert(!strcmp(line(LOG_RING_CURRENT, 4), "one") && !strcmp(line(LOG_RING_CURRENT, 5), "two"));

    /* Colour codes, CR and control bytes cleaned; invalid UTF-8 replaced. */
    emit("\x1b[0;33mcol\tour\x1b[0m \xff caf\xc3\xa9 \xed\xa0\x80\r\n");
    assert(!strcmp(line(LOG_RING_CURRENT, 6), "col our ? caf\xc3\xa9 ???"));

    /* Truncation never splits a UTF-8 character. */
    char text[LOG_RING_TEXT + 8];
    memset(text, 'a', LOG_RING_TEXT - 2);
    memcpy(text + LOG_RING_TEXT - 2, "\xc3\xa9\n", 4);
    emit("%s", text);
    assert(strlen(line(LOG_RING_CURRENT, 7)) == LOG_RING_TEXT - 2);

    /* Longer than the format buffer: one truncated line, console still whole. */
    char longer[400];
    memset(longer, 'b', sizeof(longer) - 1);
    longer[sizeof(longer) - 1] = '\0';
    emit("E (1) long: %s\n", longer);
    emit("after\n");
    assert(strncmp(line(LOG_RING_CURRENT, 8), "E (1) long: bbb", 15) == 0);
    assert(strlen(line(LOG_RING_CURRENT, 8)) == LOG_RING_TEXT - 1);
    assert(!strcmp(line(LOG_RING_CURRENT, 9), "after"));

    /* More tasks mid-line than pending slots: the oldest becomes a line. */
    for (int t = 10; t < 10 + PENDING_SLOTS + 1; ++t) {
        task(t);
        emit("part%d", t);
    }
    assert(!strcmp(line(LOG_RING_CURRENT, 10), "part10"));
    task(14);
    emit(" done\n");
    assert(!strcmp(line(LOG_RING_CURRENT, 11), "part14 done"));

    /* A complete line from outside esp_log (Matter's redirect). */
    const char *chip = "E (2) chip[DMG]: \"quoted\" \\ path";
    log_ring_line(chip, strlen(chip));
    json(LOG_RING_CURRENT, 12, 512);
    json(LOG_RING_CURRENT, 6, 512);
    json(LOG_RING_CURRENT, 8, 64);
    assert(log_ring_get(LOG_RING_CURRENT, 12, &out) && log_ring_json(&out, text, 63) == 0);

    /* Another console hook already installed is called, not bypassed. */
    reboot();
    console = custom_console;
    assert(log_ring_init() == ESP_OK);
    emit("via custom %d\n", 1);
    assert(custom_calls == 1);
    console = vprintf;

    /* Reboot: the first boot's lines are now the previous boot's. */
    assert(log_ring_info(LOG_RING_PREVIOUS, &info) && info.boot_count == 1);
    assert(info.latest_seq == 12 && info.oldest_seq == 1);
    assert(!strcmp(line(LOG_RING_PREVIOUS, 3),
                   "W(23591) OPENTHREAD:[W] Mle-----------: Failed to process UDP: Duplicated"));
    uint64_t first_boot = info.boot_id;
    assert(log_ring_info(LOG_RING_CURRENT, &info) && info.boot_count == 2);
    assert(info.boot_id != first_boot && info.latest_seq == 1);

    /* Wraparound keeps the newest LOG_RING_LINES lines. */
    for (unsigned i = 0; i < LOG_RING_LINES + 5; ++i) {
        emit("wrap %u\n", i);
    }
    assert(log_ring_info(LOG_RING_CURRENT, &info));
    assert(info.latest_seq == LOG_RING_LINES + 6 && info.oldest_seq == 7);
    assert(!log_ring_get(LOG_RING_CURRENT, 6, &out));
    assert(!strcmp(line(LOG_RING_CURRENT, 7), "wrap 5"));

    /* Third boot writes into the first boot's half; its stale slots, from a
     * different boot ID, must not extend the third boot's recovered range. */
    reboot();
    assert(log_ring_init() == ESP_OK);
    test_time = 42000000;
    emit("third %d\n", 1);
    emit("third %d\n", 2);
    reboot();
    assert(log_ring_init() == ESP_OK);
    assert(log_ring_info(LOG_RING_PREVIOUS, &info) && info.boot_count == 3);
    assert(info.latest_seq == 2 && info.oldest_seq == 1 && info.last_uptime_ms == 42000);
    assert(!strcmp(line(LOG_RING_PREVIOUS, 2), "third 2"));
    assert(!log_ring_get(LOG_RING_PREVIOUS, 3, &out));

    /* A torn slot (crash mid-write) is skipped, not served as garbage. */
    reboot();
    test_time = 1000000;
    assert(log_ring_init() == ESP_OK);
    emit("kept\n");
    emit("torn\n");
    s_cur->slots[1].text[0] = 'X';
    reboot();
    assert(log_ring_init() == ESP_OK);
    assert(log_ring_info(LOG_RING_PREVIOUS, &info) && info.latest_seq == 1);
    assert(!strcmp(line(LOG_RING_PREVIOUS, 1), "kept") && !log_ring_get(LOG_RING_PREVIOUS, 2, &out));

    /* A different firmware layout is not trusted. */
    reboot();
    s_ring.layout ^= 1;
    assert(log_ring_init() == ESP_OK && !log_ring_info(LOG_RING_PREVIOUS, &info));
    assert(log_ring_info(LOG_RING_CURRENT, &info) && info.boot_count == 1);

    /* Power-on garbage is not trusted either. */
    reboot();
    memset(&s_ring, 0x5a, sizeof(s_ring));
    assert(log_ring_init() == ESP_OK && !log_ring_info(LOG_RING_PREVIOUS, &info));

    /* Hook used before init finished storing the previous console. */
    s_console = NULL;
    emit("early %d\n", 1);

    fclose(json_out);
    return 0;
}
'''


def run(compiler):
    with tempfile.TemporaryDirectory(prefix='log-ring-test-') as tmp:
        p = Path(tmp)
        (p / 'freertos').mkdir()
        for name, text in STUBS.items():
            (p / name).write_text(text)
        (p / 'test.c').write_text(TEST_C)
        exe = p / 'test.exe'
        subprocess.run([*compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(p),
                        '-I', str(ROOT / 'main'), str(p / 'test.c'), '-o', str(exe)], check=True)
        out = p / 'json.txt'
        subprocess.run([str(exe), str(out)], check=True, stdout=subprocess.DEVNULL)
        rows = [json.loads(row) for row in out.read_text(encoding='utf-8').splitlines()]
        assert rows[0] == {'seq': 12, 'uptime_ms': 5000,
                           'text': 'E (2) chip[DMG]: "quoted" \\ path'}, rows[0]
        assert rows[1]['text'] == 'col our ? café ???', rows[1]
        assert rows[2]['seq'] == 8 and rows[2]['text'].startswith('E (1) long: b'), rows[2]
    print('PASS actual log ring assembly, cleaning, wraparound, JSON and reboot recovery')


class FakeDevice:
    """Serves /logs pages with the firmware's paging and retention rules."""

    def __init__(self):
        self.boots = {'current': self.boot('a' * 16, 1)}
        self.unreadable = set()

    @staticmethod
    def boot(boot_id, count, reason='power on'):
        return {'boot_id': boot_id, 'boot_count': count, 'reason': reason, 'latest': 0}

    def log(self, n):
        self.boots['current']['latest'] += n

    def reboot(self, reason='panic', retained=True):
        old = self.boots['current']
        self.boots = {'current': self.boot(chr(ord(old['boot_id'][0]) + 1) * 16, old['boot_count'] + 1, reason)}
        if retained:
            self.boots['previous'] = old

    def fetch(self, boot, after):
        b = self.boots.get(boot)
        if b is None:
            return None
        latest = b['latest']
        oldest = max(1, latest - 1023) if latest else 0
        start = 0 if after >= latest else max(after + 1, oldest)
        lines = []
        seq = start
        while seq and seq <= latest and len(lines) < 200:
            if (boot, seq) not in self.unreadable:
                lines.append({'seq': seq, 'uptime_ms': seq * 10, 'text': f'{b["boot_id"][0]}{seq}'})
            seq += 1
        page = {'schema': 1, 'boot': boot, 'boot_id': b['boot_id'], 'boot_count': b['boot_count'],
                'reset_reason': self.boots['current']['reason'], 'device_uptime_ms': 1,
                'capacity': 1024, 'oldest_seq': oldest, 'latest_seq': latest,
                'last_uptime_ms': latest * 10, 'lines': lines}
        return validate_page(json.loads(json.dumps(page)), after)


class TailTests(unittest.TestCase):
    def setUp(self):
        self.device = FakeDevice()
        self.out, self.notes = [], []
        self.tail = Tail(self.device.fetch, self.out.append, self.notes.append)

    def test_drain_pages_until_caught_up(self):
        self.device.log(450)
        page = self.tail.drain('current')
        self.assertEqual(self.out, [f'a{i}' for i in range(1, 451)])
        self.assertEqual(page['latest_seq'], 450)
        self.assertEqual(self.notes, [])

    def test_follow_prints_only_new_lines(self):
        self.device.log(3)
        self.tail.poll()
        self.device.log(2)
        self.tail.poll()
        self.tail.poll()
        self.assertEqual(self.out, ['a1', 'a2', 'a3', 'a4', 'a5'])

    def test_reboot_recovers_the_crashed_boots_last_lines(self):
        self.device.log(10)
        self.tail.poll()
        self.device.log(3)           # written after the last poll...
        self.device.reboot('panic')  # ...then the device crashed
        self.device.log(2)
        self.tail.poll()
        self.assertEqual(self.out[10:], ['a11', 'a12', 'a13', 'b1', 'b2'])
        self.assertIn('device rebooted; this boot started by: panic', self.notes)

    def test_power_cut_reports_lost_lines(self):
        self.device.log(4)
        self.tail.poll()
        self.device.log(1)
        self.device.reboot('power on', retained=False)
        self.device.log(1)
        self.tail.poll()
        self.assertEqual(self.out[4:], ['b1'])
        self.assertTrue(any('lost' in n for n in self.notes))

    def test_overwritten_lines_are_reported(self):
        self.device.log(5)
        self.tail.poll()
        self.device.log(2000)
        self.tail.poll()
        self.assertEqual(self.out[5], f'a{2005 - 1023}')
        self.assertEqual(self.out[-1], 'a2005')
        self.assertIn(f'{2005 - 1023 - 6} line(s) overwritten before they were read', self.notes)

    def test_torn_tail_does_not_loop(self):
        self.device.log(5)
        self.device.unreadable |= {('current', 4), ('current', 5)}
        self.tail.drain('current')
        self.assertEqual(self.out, ['a1', 'a2', 'a3'])
        self.assertEqual(self.tail.cursor, 5)

    def test_rejects_malformed_pages(self):
        page = self.device.fetch('current', 0)
        with self.assertRaises(ValueError):
            validate_page(dict(page, schema=2))
        bad = dict(page, latest_seq=2, lines=[{'seq': 2, 'uptime_ms': 0, 'text': 'x'},
                                              {'seq': 1, 'uptime_ms': 0, 'text': 'y'}])
        with self.assertRaises(ValueError):
            validate_page(bad)
        with self.assertRaises(ValueError):
            validate_page(dict(page, latest_seq=0, lines=[{'seq': 1, 'uptime_ms': 0, 'text': 'x'}]))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', required=True)
    args = parser.parse_args()
    run([args.cc, 'cc'] if Path(args.cc).stem == 'zig' else [args.cc])
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(TailTests))
    raise SystemExit(0 if result.wasSuccessful() else 1)
