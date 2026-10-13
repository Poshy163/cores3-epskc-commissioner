"""Read the CoreS3's log over Wi-Fi from GET /logs on the REST port.

  python tools/logtail.py --url http://<device-ip>:8081             this boot so far
  python tools/logtail.py --url http://<device-ip>:8081 --follow    ...and keep going
  python tools/logtail.py --url http://<device-ip>:8081 --previous  the boot before
                                                                    the last crash/reset
  python tools/logtail.py --url ... --follow --output cores3.log    also append to a file

The device keeps the last 1024 lines of this boot and of the previous one. With
--follow, a reboot is detected by its new boot ID: lines the old boot wrote
after the last poll are fetched from its retained copy before moving on, so a
crash does not swallow its own last words. Lines overwritten before they were
fetched are reported, never silently skipped.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

PAGE_LIMIT = 200


def integer(value, lower=0, upper=0xFFFFFFFF):
    return type(value) is int and lower <= value <= upper


def validate_page(page, after=0):
    """Reject anything but a well-formed page of lines after `after`."""
    if not isinstance(page, dict) or page.get('schema') != 1:
        raise ValueError('unsupported /logs schema')
    if page.get('boot') not in ('current', 'previous') or not isinstance(page.get('boot_id'), str):
        raise ValueError('missing boot identity')
    for key in ('boot_count', 'oldest_seq', 'latest_seq', 'capacity'):
        if not integer(page.get(key)):
            raise ValueError(f'invalid {key}')
    if not isinstance(page.get('reset_reason'), str) or not isinstance(page.get('lines'), list):
        raise ValueError('invalid page')
    previous = after
    for line in page['lines']:
        if not isinstance(line, dict) or not integer(line.get('seq'), 1) or not integer(line.get('uptime_ms')):
            raise ValueError('invalid line')
        if not isinstance(line.get('text'), str):
            raise ValueError('invalid line text')
        if line['seq'] <= previous or line['seq'] > page['latest_seq']:
            raise ValueError('lines out of order')
        previous = line['seq']
    return page


def http_fetch(base, boot, after, timeout=5):
    """One page, or None when the device has no such boot (404)."""
    query = urllib.parse.urlencode({'boot': boot, 'after': after, 'limit': PAGE_LIMIT})
    try:
        with urllib.request.urlopen(f'{base}/logs?{query}', timeout=timeout) as response:
            body = response.read(1 << 20)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise
    return validate_page(json.loads(body.decode('utf-8', 'replace')), after)


class Tail:
    """Turns pages into an ordered line stream with explicit gaps and reboots."""

    def __init__(self, fetch, write, note):
        self.fetch = fetch      # fetch(boot, after) -> page | None
        self.write = write      # write(text) for each log line
        self.note = note        # note(message) for tool messages
        self.boot_id = None
        self.cursor = 0

    def _consume(self, page):
        lines = page['lines']
        if lines and lines[0]['seq'] > self.cursor + 1:
            self.note(f'{lines[0]["seq"] - self.cursor - 1} line(s) overwritten before they were read')
        for line in lines:
            self.write(line['text'])
            self.cursor = line['seq']
        return bool(lines) and self.cursor < page['latest_seq']

    def drain(self, boot):
        """Everything after the cursor for this boot. Returns the last page."""
        while True:
            page = self.fetch(boot, self.cursor)
            if page is None:
                return None
            if self.boot_id is None:
                self.boot_id = page['boot_id']
            if not self._consume(page):
                if page['latest_seq'] > self.cursor and not page['lines']:
                    # Unreadable tail (torn by a crash): nothing more to fetch.
                    self.cursor = page['latest_seq']
                return page

    def poll(self):
        """Follow mode: new lines of the running boot, bridging a reboot."""
        page = self.fetch('current', self.cursor if self.boot_id else 0)
        if page is None:
            raise ValueError('device returned no current log')
        if self.boot_id is None:
            self.boot_id = page['boot_id']
        elif page['boot_id'] != self.boot_id:
            old = self.fetch('previous', self.cursor)
            if old is not None and old['boot_id'] == self.boot_id:
                if self._consume(old):
                    self.drain('previous')
            else:
                self.note('earlier boot not retained (power cut?); its last lines are lost')
            self.note(f'device rebooted; this boot started by: {page["reset_reason"]}')
            self.boot_id = page['boot_id']
            self.cursor = 0
            self.drain('current')   # that page was cut at the old cursor
            return
        if self._consume(page):
            self.drain('current')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--url', required=True, help='http://device-ip:8081')
    parser.add_argument('--previous', action='store_true', help='the boot before the last reset')
    parser.add_argument('--follow', '-f', action='store_true', help='keep printing new lines')
    parser.add_argument('--interval', type=float, default=2, help='seconds between polls (default 2)')
    parser.add_argument('--output', type=Path, help='also append lines to this file')
    args = parser.parse_args()
    url = urllib.parse.urlsplit(args.url)
    if url.scheme != 'http' or not url.hostname or url.username or url.password or url.query or url.fragment or url.path not in ('', '/'):
        parser.error('--url must be a plain HTTP origin without credentials')
    if not math.isfinite(args.interval) or args.interval < 0.5:
        parser.error('--interval must be at least 0.5 seconds')
    if args.previous and args.follow:
        parser.error('--previous is a finished boot; it cannot be followed')
    base = args.url.rstrip('/')
    out = args.output.open('a', encoding='utf-8') if args.output else None

    def write(text):
        print(text, flush=True)
        if out:
            out.write(text + '\n')
            out.flush()

    def note(message):
        print(f'--- {message}', file=sys.stderr, flush=True)
        if out:
            out.write(f'--- {message}\n')
            out.flush()

    tail = Tail(lambda boot, after: http_fetch(base, boot, after), write, note)
    try:
        if not args.follow:
            page = tail.drain('previous' if args.previous else 'current')
            if page is None:
                note('no previous boot log: the last start was a power-on or new firmware'
                     if args.previous else 'the device has no /logs (switched off, or older firmware)')
                return 1
            note(f'{page["boot"]} boot {page["boot_count"]}; this boot started by: {page["reset_reason"]}')
            return 0
        last_error = None
        while True:
            try:
                tail.poll()
                last_error = None
            except (OSError, ValueError) as error:
                kind = f'{type(error).__name__}: {error}'
                if kind != last_error:
                    note(f'device unreachable ({kind}); retrying')
                last_error = kind
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0
    finally:
        if out:
            out.close()


if __name__ == '__main__':
    sys.exit(main())
