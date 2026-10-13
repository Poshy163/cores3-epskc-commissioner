"""Exercise the real firmware ring and the durable PC recorder."""
import argparse
import csv
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from battery_record import Recorder, validate_page, lock_output

ROOT = Path(__file__).resolve().parents[1]


def firmware_test(compiler):
    with tempfile.TemporaryDirectory(prefix='power-log-test-') as tmp:
        p = Path(tmp)
        (p / 'freertos').mkdir()
        (p / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n')
        (p / 'freertos/FreeRTOS.h').write_text('''#pragma once
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
''')
        (p / 'esp_heap_caps.h').write_text('''#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
extern int fail_alloc;
static void *heap_caps_calloc(size_t n, size_t size, int cap) {
    (void)cap; return fail_alloc ? NULL : calloc(n, size);
}
''')
        (p / 'esp_random.h').write_text('''#pragma once
#include <string.h>
static void esp_fill_random(void *out, size_t n) { memset(out, 0x42, n); }
''')
        (p / 'esp_timer.h').write_text('''#pragma once
#include <stdint.h>
extern int64_t test_time;
static int64_t esp_timer_get_time(void) { return test_time; }
''')
        (p / 'test.c').write_text('''#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "power_log.h"
int fail_alloc;
int64_t test_time;
int main(void) {
    power_log_info_t info;
    power_log_sample_t row;
    assert(!power_log_info(&info));
    fail_alloc = 1; assert(power_log_init() == ESP_ERR_NO_MEM);
    fail_alloc = 0; assert(power_log_init() == ESP_OK);
    assert(power_log_info(&info) && info.count == 0 && info.oldest_seq == 0);
    power_status_t ps = {.percent=73,.batt_mv=3900,.vbus_mv=0,.vsys_mv=3870,
                         .pmic_temp_c=40,.present=true,.raw_st1=8};
    for (unsigned i=1; i<=POWER_LOG_CAPACITY+3; ++i) {
        test_time = (int64_t)i * 10000000;
        power_log_note(&ps, ESP_OK, 20);
    }
    assert(power_log_info(&info) && info.count == POWER_LOG_CAPACITY);
    assert(info.oldest_seq == 4 && info.latest_seq == POWER_LOG_CAPACITY+3);
    assert(!power_log_get(3, &row) && !power_log_get(0, &row));
    assert(power_log_get(4, &row) && row.uptime_ms == 40000);
    assert(!row.usb && row.percent == 73 && row.screen_percent == 20);
    ps.vbus=true; ps.charging=true;
    power_log_note(&ps, ESP_OK, 95);
    assert(power_log_get(4+POWER_LOG_CAPACITY, &row));
    char json[512]; assert(power_log_json(&row,json,sizeof(json))); puts(json);
    assert(!power_log_json(&row,json,10));
    assert(power_log_get(5, &row) && !row.usb); // USB retained older battery data
    assert(power_log_init() == ESP_OK); // idempotent, no clearing
    assert(power_log_get(5, &row));
    ps.pmic_temp_c=INT_MIN;
    power_log_note(&ps, ESP_FAIL, 500);
    assert(power_log_info(&info) && power_log_get(info.latest_seq,&row));
    assert(!row.valid && row.screen_percent == 100);
    assert(power_log_json(&row,json,sizeof(json))); puts(json);
    uint32_t number;
    assert(power_log_parse_uint("4294967295",UINT32_MAX,&number) && number==UINT32_MAX);
    assert(!power_log_parse_uint("4294967296",UINT32_MAX,&number));
    assert(!power_log_parse_uint("-1",UINT32_MAX,&number));
    assert(!power_log_parse_uint("+1",UINT32_MAX,&number));
    assert(!power_log_parse_uint("1x",UINT32_MAX,&number));
    assert(!power_log_parse_uint("",UINT32_MAX,&number));
    return 0;
}
''')
        exe = p / 'test.exe'
        subprocess.run([*compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(p),
                        '-I', str(ROOT/'main'), str(p/'test.c'), str(ROOT/'main/power_log.c'),
                        '-o', str(exe)], check=True)
        valid, invalid = map(json.loads, subprocess.check_output([str(exe)], text=True).splitlines())
        assert valid['usb'] and valid['charging'] and valid['percent'] == 73
        assert invalid['percent'] is None and invalid['batt_mv'] is None and invalid['pmic_temp_c'] is None
        assert invalid['valid'] is False
    print('PASS actual history ring retention/wraparound, ADC errors, JSON and cursor bounds')


def page(seqs, boot='a'*16, usb=False, oldest=1, latest=None):
    seqs = list(seqs)
    latest = latest if latest is not None else max(seqs, default=0)
    return {'schema': 1, 'boot_id': boot, 'capacity': 8640,
            'device_uptime_ms': (latest+1)*10000,
            'oldest_seq': oldest if latest else 0, 'latest_seq': latest,
            'samples': [dict(seq=i,uptime_ms=i*10000,valid=True,percent=90,
                             batt_mv=4000,vbus_mv=4800 if usb else 0,vsys_mv=3980,
                             pmic_temp_c=40,present=True,usb=usb,charging=usb,
                             full=False,estimated=False,screen_percent=20,
                             raw_st1=8,raw_st2=0) for i in seqs]}


class RecorderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.output = Path(self.tmp.name)/'battery.csv'
        self.r = Recorder(self.output)

    def tearDown(self):
        self.tmp.cleanup()

    def rows(self):
        return list(csv.DictReader(io.StringIO(self.output.read_text())))

    def events(self):
        return [json.loads(line) for line in self.r.events_path.read_text().splitlines()]

    def test_usb_reconnect_keeps_battery_rows(self):
        self.r.ingest(page([1,2]))
        self.r.ingest(page([3],usb=True))
        self.assertEqual(len(self.rows()),3)
        self.assertEqual(self.r.battery_count,2)
        self.assertEqual(self.rows()[0]['usb'],'False')
        self.assertEqual(self.rows()[2]['usb'],'True')

    def test_restart_and_overlap_deduplicates(self):
        self.r.ingest(page([1,2]))
        restarted=Recorder(self.output)
        restarted.ingest(page([1,2,3]))
        self.assertEqual(len(self.rows()),3)
        self.assertEqual(restarted.last_seq,3)

    def test_reboot_resets_old_cursor_before_backfill(self):
        self.r.ingest(page([1,2,3]))
        self.assertTrue(self.r.ingest(page([4,5],boot='b'*16)))
        self.assertEqual(self.r.last_seq,0)
        self.r.ingest(page([1,2,3,4,5],boot='b'*16))
        self.assertEqual(len(self.rows()),8)
        self.assertEqual([e['event'] for e in self.events()],['boot','device_reboot'])

    def test_overwrite_gap_is_recorded(self):
        self.r.ingest(page([50,51],oldest=50))
        self.assertEqual(self.events()[-1]['event'],'missing_samples')
        self.assertEqual(self.events()[-1]['expected_seq'],1)

    def test_truncated_last_record_is_refetched(self):
        self.r.ingest(page([1]))
        with self.output.open('ab') as f:
            f.write(b'incomplete,record')
        restarted=Recorder(self.output)
        restarted.ingest(page([2]))
        self.assertEqual(len(self.rows()),2)
        self.assertEqual(self.rows()[1]['seq'],'2')

    def test_pagination_and_stale_detection(self):
        self.assertTrue(self.r.ingest(page([1,2],latest=3)))
        self.assertFalse(self.r.ingest(page([3])))
        latest=page([],latest=3)
        latest['device_uptime_ms']=100000
        self.r.ingest(latest)
        self.assertTrue(json.loads(self.r.status_path.read_text())['stale'])

    def test_rejects_malformed_data_before_writing(self):
        invalid=page([1]); invalid['samples'][0]['usb']='false'
        with self.assertRaises(ValueError): self.r.ingest(invalid)
        self.assertFalse(self.output.exists())
        with self.assertRaises(ValueError): validate_page(page([2,1]))
        invalid=page([1]); invalid['samples'][0]['percent']=101
        with self.assertRaises(ValueError): validate_page(invalid)

    def test_single_writer_lock(self):
        with lock_output(self.output):
            with self.assertRaises(OSError):
                with lock_output(self.output): pass


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--cc',required=True)
    args=parser.parse_args()
    cc=[args.cc,'cc'] if Path(args.cc).stem=='zig' else [args.cc]
    firmware_test(cc)
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(RecorderTests))
    raise SystemExit(0 if result.wasSuccessful() else 1)
