"""Record /power/history to durable CSV over Wi-Fi, without USB power.

The device buffers ~24 hours in RAM. This recorder drains that buffer every
30 seconds and fsyncs the CSV. Keep this PC awake and on the same LAN during
the test. Reboots/overwritten history are explicit events, not bridged data.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import csv
from datetime import datetime, timedelta, timezone
import json
import math
import os
from pathlib import Path
import re
import time
import urllib.parse
import urllib.request


SAMPLE_FIELDS = [
    'seq', 'uptime_ms', 'valid', 'percent', 'batt_mv', 'vbus_mv', 'vsys_mv',
    'pmic_temp_c', 'present', 'usb', 'charging', 'full', 'estimated',
    'screen_percent', 'raw_st1', 'raw_st2',
]
FIELDS = ['boot_id', 'sample_utc_estimate', 'received_utc', *SAMPLE_FIELDS]
BOOL_FIELDS = ['valid', 'present', 'usb', 'charging', 'full', 'estimated']


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec='milliseconds')


def integer(value, lower=0, upper=0xFFFFFFFF):
    return type(value) is int and lower <= value <= upper


def validate_page(page):
    if not isinstance(page, dict) or page.get('schema') != 1:
        raise ValueError('unsupported history schema')
    if not re.fullmatch(r'[0-9a-f]{16}', str(page.get('boot_id', ''))):
        raise ValueError('invalid boot id')
    for field in ('oldest_seq', 'latest_seq', 'capacity'):
        if not integer(page.get(field)):
            raise ValueError('invalid history bounds')
    if not integer(page.get('device_uptime_ms'), upper=2**63 - 1):
        raise ValueError('invalid device uptime')
    if page['oldest_seq'] > page['latest_seq'] or page['capacity'] == 0:
        raise ValueError('inconsistent history bounds')
    rows = page.get('samples')
    if not isinstance(rows, list) or len(rows) > 120:
        raise ValueError('invalid sample count')
    previous = 0
    for row in rows:
        if not isinstance(row, dict) or any(field not in row for field in SAMPLE_FIELDS):
            raise ValueError('incomplete sample')
        if not integer(row['seq'], max(previous + 1, page['oldest_seq']), page['latest_seq']):
            raise ValueError('unordered or out-of-range sample')
        previous = row['seq']
        if not integer(row['uptime_ms'], upper=page['device_uptime_ms']):
            raise ValueError('invalid sample time')
        if any(type(row[field]) is not bool for field in BOOL_FIELDS):
            raise ValueError('invalid sample flag')
        for field in ('percent', 'batt_mv', 'vbus_mv', 'vsys_mv', 'pmic_temp_c'):
            upper = 100 if field == 'percent' else 32767
            lower = -32767 if field == 'pmic_temp_c' else 0
            if row[field] is not None and not integer(row[field], lower, upper):
                raise ValueError('invalid measurement')
        for field, maximum in [('screen_percent', 100), ('raw_st1', 255), ('raw_st2', 255)]:
            if not integer(row[field], upper=maximum):
                raise ValueError('invalid screen/register value')
    return page


class Recorder:
    def __init__(self, output: Path):
        self.output = output
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.status_path = output.with_suffix('.status.json')
        self.events_path = output.with_suffix('.events.jsonl')
        self.boot_id = None
        self.last_seq = 0
        self.count = 0
        self.battery_count = 0
        self.last_row = None
        self.last_received = None
        self.last_error = None
        self.capture_until_utc = None
        self.keeps_pc_awake = False
        # A killed process may leave a partial last CSV row. Re-fetch it using
        # the cursor reconstructed from the last complete row.
        if output.exists() and output.stat().st_size:
            with output.open('r+b') as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - 4096))
                tail = f.read()
                if not tail.endswith(b'\n'):
                    pos = tail.rfind(b'\n')
                    if pos < 0:
                        raise ValueError('CSV has no complete trailing row')
                    f.truncate(size - len(tail) + pos + 1)
            with output.open(newline='', encoding='utf-8') as f:
                reader = csv.DictReader(f)
                if reader.fieldnames != FIELDS:
                    raise ValueError('CSV schema mismatch')
                for row in reader:
                    if None in row or any(value is None for value in row.values()):
                        raise ValueError('malformed CSV row')
                    for field in SAMPLE_FIELDS:
                        if field in BOOL_FIELDS:
                            row[field] = {'True': True, 'False': False}[row[field]]
                        else:
                            row[field] = int(row[field]) if row[field] else None
                    self.boot_id = row['boot_id']
                    self.last_seq = int(row['seq'])
                    self.count += 1
                    self.battery_count += row['valid'] and row['present'] and not row['usb']
                    self.last_row = row
                    self.last_received = row['received_utc']

    def event(self, kind, **details):
        with self.events_path.open('a', encoding='utf-8') as f:
            f.write(json.dumps({'utc': utc_now(), 'event': kind, **details}) + '\n')
            f.flush()
            os.fsync(f.fileno())

    def status(self, **extra):
        row = self.last_row or {}
        data = {
            'pid': os.getpid(), 'updated_utc': utc_now(),
            'boot_id': self.boot_id, 'last_seq': self.last_seq,
            'rows_saved': self.count, 'battery_rows_saved': self.battery_count,
            'last_received_utc': self.last_received,
            'last_sample_utc_estimate': row.get('sample_utc_estimate'),
            'last_percent': row.get('percent'), 'last_batt_mv': row.get('batt_mv'),
            'last_usb': row.get('usb'), 'last_valid': row.get('valid'),
            'last_error': self.last_error, **extra,
            'capture_until_utc': self.capture_until_utc,
            'keeps_pc_awake': self.keeps_pc_awake,
        }
        temporary = self.status_path.with_suffix('.tmp')
        temporary.write_text(json.dumps(data, indent=2), encoding='utf-8')
        os.replace(temporary, self.status_path)

    def ingest(self, page, received=None):
        page = validate_page(page)
        received = received or datetime.now(timezone.utc)
        boot = page['boot_id']
        if boot != self.boot_id:
            previous_seq = self.last_seq
            self.event('boot' if self.boot_id is None else 'device_reboot',
                       boot_id=boot, previous_boot_id=self.boot_id)
            self.boot_id, self.last_seq = boot, 0
            self.last_row = None
            self.last_received = None
            if previous_seq:
                # This page was fetched with the previous boot's cursor. It
                # may omit available early rows even when it contains samples.
                self.status(running=True, stale=True)
                return page['latest_seq'] > 0
        rows = [row for row in page['samples'] if row['seq'] > self.last_seq]
        if not rows and self.last_seq < page['latest_seq']:
            raise ValueError('history page made no progress')
        if rows:
            new_file = not self.output.exists() or self.output.stat().st_size == 0
            with self.output.open('a', newline='', encoding='utf-8') as f:
                writer = csv.DictWriter(f, fieldnames=FIELDS)
                if new_file:
                    writer.writeheader()
                for row in rows:
                    if row['seq'] != self.last_seq + 1:
                        self.event('missing_samples', boot_id=boot,
                                   expected_seq=self.last_seq + 1, first_available_seq=row['seq'])
                    estimate = received - timedelta(milliseconds=page['device_uptime_ms'] - row['uptime_ms'])
                    saved = {field: row[field] for field in SAMPLE_FIELDS}
                    saved.update(boot_id=boot,
                                 sample_utc_estimate=estimate.isoformat(timespec='milliseconds'),
                                 received_utc=received.isoformat(timespec='milliseconds'))
                    writer.writerow(saved)
                    # Commit before advancing the cursor: a failed write must
                    # never make a retry skip a sample that is not on disk.
                    f.flush()
                    os.fsync(f.fileno())
                    self.last_seq = row['seq']
                    self.count += 1
                    self.battery_count += row['valid'] and row['present'] and not row['usb']
                    self.last_row, self.last_received = saved, saved['received_utc']
        self.last_error = None
        newest = page['samples'][-1] if page['samples'] else self.last_row
        age = page['device_uptime_ms'] - int(newest['uptime_ms']) if newest else None
        self.status(running=True, sample_age_ms=age, stale=age is None or age > 35000)
        return self.last_seq < page['latest_seq']


def lock_output(output):
    lock = output.with_suffix('.lock').open('a+b')
    lock.seek(0)
    if not lock.read(1):
        lock.write(b'0')
        lock.flush()
    lock.seek(0)
    try:
        if os.name == 'nt':
            import msvcrt
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        lock.close()
        raise
    return lock


@contextmanager
def keep_awake(enabled):
    if enabled and os.name != 'nt':
        raise ValueError('--keep-awake is available only on Windows')
    if enabled:
        import ctypes
        if not ctypes.windll.kernel32.SetThreadExecutionState(0x80000001):
            raise OSError('could not prevent automatic system sleep')
    try:
        yield
    finally:
        if enabled:
            ctypes.windll.kernel32.SetThreadExecutionState(0x80000000)


def fetch(url, after):
    request = urllib.request.Request(
        url.rstrip('/') + f'/power/history?after={after}&limit=120',
        headers={'Cache-Control': 'no-cache'})
    with urllib.request.urlopen(request, timeout=8) as response:
        body = response.read(262145)
        if len(body) > 262144:
            raise ValueError('history response too large')
        return validate_page(json.loads(body))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True, help='http://device-ip:8081')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--interval', type=float, default=30)
    parser.add_argument('--hours', type=float, default=24, help='stop after this many hours (default 24)')
    parser.add_argument('--keep-awake', action='store_true', help='prevent automatic Windows system sleep during capture')
    parser.add_argument('--once', action='store_true', help='download available history and exit')
    args = parser.parse_args()
    url = urllib.parse.urlsplit(args.url)
    if url.scheme != 'http' or not url.hostname or url.username or url.password or url.query or url.fragment or url.path not in ('', '/'):
        parser.error('--url must be a plain HTTP origin without credentials')
    if not math.isfinite(args.interval) or args.interval < 5:
        parser.error('--interval must be at least 5 seconds')
    if not math.isfinite(args.hours) or args.hours <= 0:
        parser.error('--hours must be positive')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with lock_output(args.output), keep_awake(args.keep_awake):
        recorder = Recorder(args.output)
        recorder.keeps_pc_awake = args.keep_awake
        recorder.capture_until_utc = (datetime.now(timezone.utc) + timedelta(hours=args.hours)).isoformat(timespec='milliseconds')
        deadline = time.monotonic() + args.hours * 3600
        try:
            while time.monotonic() < deadline:
                try:
                    page = fetch(args.url, recorder.last_seq)
                except (OSError, ValueError, KeyError) as error:
                    kind = type(error).__name__
                    if kind != recorder.last_error:
                        recorder.event('collection_error', error=kind)
                    recorder.last_error = kind
                    recorder.status(running=True, stale=True)
                    if args.once:
                        raise
                    time.sleep(args.interval)
                    continue
                # Storage errors stop collection: retrying a partial CSV write
                # in-place could silently duplicate or concatenate rows.
                more = recorder.ingest(page)
                if more:
                    time.sleep(0.05)
                    continue
                if args.once:
                    break
                time.sleep(args.interval)
        except KeyboardInterrupt:
            pass
        finally:
            recorder.keeps_pc_awake = False
            recorder.status(running=False)


if __name__ == '__main__':
    main()
