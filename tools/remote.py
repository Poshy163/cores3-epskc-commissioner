"""CoreS3 remote status, verified reboot, and sanitized diagnostic bundles.

Remote support is disabled by default; set it up over USB with the device's
remote console command. Supply its token through CORES3_REMOTE_TOKEN or a
--token-file, never as a command-line token.

  python tools/remote.py --ip 192.168.1.62 status
  python tools/remote.py --ip 192.168.1.62 --token-file device.token reboot
  python tools/remote.py --ip 192.168.1.62 bundle --output diagnostics.zip
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile


TOKEN_ENV = 'CORES3_REMOTE_TOKEN'
MAX_RESPONSE = 1 << 20
MAX_PAGES = 128
ROLES = {'disabled', 'detached', 'child', 'router', 'leader', 'unknown', 'unavailable'}
ATTACHED_ROLES = {'child', 'router', 'leader'}
LOCAL_NETWORKS = tuple(map(ipaddress.ip_network, [
    '10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16', '169.254.0.0/16',
    '127.0.0.0/8', 'fc00::/7', 'fe80::/10', '::1/128',
]))


class RemoteError(Exception):
    """Messages are controlled locally; response bodies/exceptions are private."""
    def __init__(self, message, *, kind='response', uncertain=False):
        super().__init__(message)
        self.kind = kind
        self.uncertain = uncertain


def require(condition, message='Device returned an invalid response.'):
    if not condition:
        raise RemoteError(message, kind='invalid_response')


def integer(value, minimum=0, maximum=2**63 - 1):
    return type(value) is int and minimum <= value <= maximum


def local_origin(host, port=8081):
    """No hostname resolution, public addresses, URL credentials, or selectors."""
    try:
        require(isinstance(host, str) and len(host) <= 128)
        literal, separator, scope = host.partition('%')
        address = ipaddress.ip_address(literal)
        require(not separator or (address.version == 6 and address.is_link_local
                                 and re.fullmatch(r'[A-Za-z0-9_.-]{1,40}', scope)))
        require(any(address.version == network.version and address in network
                    for network in LOCAL_NETWORKS))
        require(type(port) is int and 1 <= port <= 65535)
    except (ValueError, RemoteError):
        raise RemoteError('Use an explicit local numeric IPv4 or IPv6 address and valid port.',
                          kind='arguments') from None
    canonical = address.compressed + ('%25' + scope if separator else '')
    return f'http://[{canonical}]:{port}' if address.version == 6 else f'http://{canonical}:{port}'


def load_token(token_file=None, environ=None):
    try:
        if token_file is not None:
            with Path(token_file).open('r', encoding='ascii') as stream:
                token = stream.read(130).strip()
        else:
            token = (os.environ if environ is None else environ).get(TOKEN_ENV, '').strip()
    except (OSError, UnicodeError):
        raise RemoteError('Could not read the token file; its contents were not logged.',
                          kind='arguments') from None
    if not re.fullmatch(r'[0-9a-f]{64}', token):
        raise RemoteError('Set CORES3_REMOTE_TOKEN or --token-file to the 64 lowercase hex token '
                          'retrieved locally over USB.', kind='arguments')
    return token


def redact(value, token, *, free_text=False):
    """Keep structured diagnostics; conservatively suppress credential log lines."""
    if isinstance(value, str):
        value = re.sub(re.escape(token), '[REDACTED]', value, flags=re.IGNORECASE)
        value = re.sub(r'(?i)\bBearer\s+\S+', 'Bearer [REDACTED]', value)
        if free_text:
            if re.search(r'(?i)\b(password|passcode|pskc|network[ _-]?key|tap|token|'
                         r'authorization|pairing|manual[ _-]?code|qr[ _-]?data)\b|MT:', value):
                return '[REDACTED SENSITIVE LOG LINE]'
            value = re.sub(r'\b[0-9a-fA-F]{32,}\b', '[REDACTED HEX]', value)
            value = re.sub(r'\b\d{9,11}\b|\b\d{3}[ -]\d{3}[ -]\d{3}\b',
                           '[REDACTED CODE]', value)
        return value
    if isinstance(value, list):
        return [redact(item, token, free_text=free_text) for item in value]
    if isinstance(value, dict):
        return {redact(str(key), token): redact(item, token, free_text=free_text)
                for key, item in value.items()}
    return value


def select_scalars(source, names):
    require(isinstance(source, dict))
    return {name: source[name] for name in names if name in source and
            (source[name] is None or type(source[name]) in (str, int, bool))}


def select_diagnostics(source, *, numbers=(), flags=(), text=(), signed=()):
    """Retain supported diagnostics without allowing free text in numeric fields."""
    require(isinstance(source, dict))
    result = {}
    for name in (*numbers, *flags, *text, *signed):
        if name not in source:
            continue
        value = source[name]
        if value is not None:
            if name in flags:
                require(type(value) is bool)
            elif name in text:
                require(isinstance(value, str) and len(value) <= 256)
            else:
                require(integer(value, -(2**31) if name in signed else 0))
        result[name] = value
    return result


def validate_status(raw, token):
    require(isinstance(raw, dict) and type(raw.get('schema')) is int and raw['schema'] == 1)
    require('boot_id' in raw and (raw.get('boot_id') is None or (isinstance(raw['boot_id'], str)
            and re.fullmatch(r'[0-9a-f]{16}', raw['boot_id']))))
    require(integer(raw.get('uptime_ms')))
    firmware = raw.get('firmware')
    require(isinstance(firmware, dict))
    require(all(isinstance(firmware.get(key), str) and len(firmware[key]) <= 128
                for key in ('version', 'idf', 'elf_sha256')))
    require(re.fullmatch(r'[0-9a-f]{64}', firmware['elf_sha256']) is not None)
    wifi, thread, matter = (raw.get(key) for key in ('wifi', 'thread', 'matter'))
    require(isinstance(wifi, dict) and type(wifi.get('connected')) is bool)
    require(isinstance(thread, dict) and thread.get('role') in ROLES
            and type(thread.get('attached')) is bool)
    require('available' not in thread or type(thread['available']) is bool)
    require(isinstance(matter, dict) and isinstance(matter.get('status'), str)
            and len(matter['status']) <= 40 and type(matter.get('commissioned')) is bool)
    result = {
        'schema': 1, 'boot_id': raw['boot_id'], 'uptime_ms': raw['uptime_ms'],
        'firmware': select_scalars(firmware, ['version', 'idf', 'elf_sha256']),
        'wifi': select_diagnostics(wifi,
            numbers=['channel', 'associated_ms', 'disconnect_count', 'last_disconnect_reason',
                     'last_disconnect_age_ms', 'retry_step'],
            flags=['connected', 'supervisor_on', 'reconnect_pending'],
            text=['ipv4', 'ipv6'], signed=['rssi']),
        'heap': select_diagnostics(raw.get('heap'), numbers=['internal_free', 'internal_largest',
                    'internal_minimum', 'psram_free', 'lvgl_min_stack_free']),
        'thread': select_diagnostics(thread,
            flags=['available', 'attached'], text=['role'], signed=['parent_rssi'],
            numbers=['attach_duration_s', 'known_routers', 'direct_children', 'mac_tx', 'mac_rx',
                     'ip_tx_success', 'ip_rx_success', 'ip_tx_failure', 'ip_rx_failure', 'parent_changes']),
        'matter': select_diagnostics(matter, text=['status'], numbers=['endpoint'],
                    flags=['commissioned', 'commissioning_window_open']),
        'power': select_diagnostics(raw.get('power'),
            numbers=['age_ms', 'percent', 'battery_mv', 'vbus_mv'], signed=['pmic_temp_c'],
            flags=['available', 'valid', 'fresh', 'usb', 'charging', 'full']),
    }
    result.update(select_diagnostics(raw, numbers=['boot_count'], text=['reset_reason']))
    if isinstance(raw.get('logs'), dict):
        result['logs'] = select_diagnostics(raw['logs'], flags=['available'],
                    numbers=['oldest_seq', 'latest_seq', 'duplicate_mle_count'])
    if isinstance(raw.get('remote'), dict):
        result['remote'] = select_diagnostics(raw['remote'], flags=['enabled', 'reboot_pending'])
    return redact(result, token)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Client:
    def __init__(self, host, token, *, port=8081, timeout=5, opener=None,
                 clock=time.monotonic, sleep=time.sleep):
        self.origin = local_origin(host, port)
        require(isinstance(token, str) and re.fullmatch(r'[0-9a-f]{64}', token))
        require(type(timeout) in (int, float) and math.isfinite(timeout) and 0 < timeout <= 30,
                'HTTP timeout must be greater than zero and at most 30 seconds.')
        self._token = token
        self.timeout, self.clock, self.sleep = timeout, clock, sleep
        self.opener = opener if opener is not None else urllib.request.build_opener(
            urllib.request.ProxyHandler({}), NoRedirect())

    def request(self, path, *, query=None, method='GET', body=None, timeout=None,
                missing_ok=False):
        require(path in {'/device/status', '/device/reboot', '/logs', '/power/history'},
                'This helper does not access credential endpoints.')
        require((method == 'GET' and body is None) or
                (method == 'POST' and path == '/device/reboot' and body == 'reboot'))
        limit = self.timeout if timeout is None else min(self.timeout, timeout)
        require(math.isfinite(limit) and limit > 0, 'Request deadline expired.')
        suffix = '?' + urllib.parse.urlencode(query) if query else ''
        headers = {'Accept': 'application/json', 'Cache-Control': 'no-store'}
        if path.startswith('/device/'):
            headers['Authorization'] = 'Bearer ' + self._token
        payload = None
        if body is not None:
            headers['Content-Type'] = 'application/json'
            payload = json.dumps(body).encode('ascii')
        request = urllib.request.Request(self.origin + path + suffix, data=payload,
                                         headers=headers, method=method)
        try:
            with self.opener.open(request, timeout=limit) as response:
                code = response.status
                payload = response.read(MAX_RESPONSE + 1)
            require(len(payload) <= MAX_RESPONSE, 'Device response exceeded the size limit.')
            def reject_constant(value):
                raise ValueError('nonfinite JSON')
            result = json.loads(payload.decode('utf-8'), parse_constant=reject_constant)
            return code, result
        except urllib.error.HTTPError as error:
            code = error.code
            error.close()
            if code == 404 and missing_ok:
                return 404, None
            if code == 401:
                raise RemoteError('Device rejected the token. Check the environment variable '
                                  'or token file.', kind='unauthorized') from None
            if code == 503:
                raise RemoteError('Remote support is disabled or unavailable. Set it up '
                                  'locally over USB with the remote console command.',
                                  kind='unavailable') from None
            raise RemoteError('Device returned an HTTP error; redirects and response-body '
                              'logging are disabled.', kind='http',
                              uncertain=method == 'POST' and code not in
                              {400, 403, 404, 405, 409, 413, 415, 422}) from None
        except RemoteError as error:
            if method == 'POST':
                error.uncertain = True
            raise
        except Exception:
            raise RemoteError('Could not complete the device request; private request and '
                              'response details were not logged.', kind='transport',
                              uncertain=method == 'POST') from None

    def status(self, *, timeout=None):
        code, raw = self.request('/device/status', timeout=timeout)
        require(code == 200)
        return validate_status(raw, self._token)

    @staticmethod
    def health(before, after):
        thread = after['thread']
        summary = {
            'wifi_connected': after['wifi']['connected'],
            'thread_available': thread.get('available', True),
            'thread_attached': thread['attached'] and thread['role'] in ATTACHED_ROLES,
            'matter_status': after['matter']['status'],
            'matter_commissioned': after['matter']['commissioned'],
            'commissioning_state_preserved': (before['matter']['commissioned'] ==
                                              after['matter']['commissioned']),
            'firmware_unchanged': before['firmware'] == after['firmware'],
        }
        summary['ready'] = all(summary[key] for key in ['wifi_connected', 'thread_available',
                         'thread_attached', 'commissioning_state_preserved', 'firmware_unchanged']) \
            and summary['matter_status'] in {'commissioned', 'commissionable'}
        return summary

    def reboot(self, *, poll_seconds=90):
        require(type(poll_seconds) in (int, float) and math.isfinite(poll_seconds)
                and 0 < poll_seconds <= 90, 'Reboot observation must be between 0 and 90 seconds.')
        before = self.status()
        require(before['boot_id'] is not None,
                'Boot identity is unavailable; a reboot cannot be verified safely.')
        report = {'request_accepted': None, 'request_outcome': 'unknown',
                  'post_attempts': 1, 'reboot_verified': False, 'health_verified': False,
                  'before_boot_id': before['boot_id'], 'after_boot_id': None,
                  'verification_scope': 'Changed API boot identity and reported health; '
                                        'physical reset cause and long-term stability are untested.'}
        try:
            code, ack = self.request('/device/reboot', method='POST', body='reboot')
            if code == 202 and isinstance(ack, dict) and ack.get('accepted') is True \
                    and ack.get('reboot_pending') is True:
                report['request_accepted'], report['request_outcome'] = True, 'accepted'
        except RemoteError as error:
            if not error.uncertain:
                raise
            report['request_note'] = 'POST outcome is unknown; no retry was made.'
        started = self.clock()
        deadline = started + poll_seconds
        latest = None
        while self.clock() < deadline:
            try:
                latest = self.status(timeout=deadline - self.clock())
                if latest['boot_id'] is not None and latest['boot_id'] != before['boot_id']:
                    report['reboot_verified'] = True
                    report['after_boot_id'] = latest['boot_id']
                    report['health'] = self.health(before, latest)
                    if report['health']['ready']:
                        report['health_verified'] = True
                        break
            except RemoteError as error:
                report['last_poll_error'] = error.kind
            remaining = deadline - self.clock()
            if remaining > 0:
                self.sleep(min(1, remaining))
        report['observation_seconds'] = round(self.clock() - started, 3)
        if latest is not None:
            report['last_status'] = latest
        if not report['reboot_verified']:
            report['message'] = 'A changed boot identity was not observed; reboot is unverified.'
        elif not report['health_verified']:
            report['message'] = 'Restart was observed, but reported node health did not become ready.'
        return redact(report, self._token)

    def _pages(self, path, *, boot=None, deadline):
        rows_key, limit = ('lines', 200) if path == '/logs' else ('samples', 120)
        result = {'available': False, 'complete': False, 'pages': 0, rows_key: [], 'notes': []}
        cursor, identity, target = 0, None, None
        for _ in range(MAX_PAGES):
            remaining = deadline - self.clock()
            if remaining <= 0:
                result['notes'].append('Bundle deadline reached.')
                return result
            query = {'after': cursor, 'limit': limit}
            if boot is not None:
                query['boot'] = boot
            try:
                code, page = self.request(path, query=query, missing_ok=True, timeout=remaining)
                if code == 404:
                    result['notes'].append('Endpoint or retained boot is unavailable.')
                    return result
                require(code == 200 and isinstance(page, dict) and type(page.get('schema')) is int
                        and page['schema'] == 1)
                require(isinstance(page.get('boot_id'), str)
                        and re.fullmatch(r'[0-9a-f]{16}', page['boot_id']) is not None)
                require(all(integer(page.get(key), maximum=0xffffffff)
                            for key in ('oldest_seq', 'latest_seq', 'capacity')))
                require(0 < page['capacity'] <= 20000 and page['oldest_seq'] <= page['latest_seq'])
                require(isinstance(page.get(rows_key), list) and len(page[rows_key]) <= limit)
                require(boot is None or page.get('boot') == boot)
                if identity is None:
                    identity, target = page['boot_id'], page['latest_seq']
                    result.update(available=True, boot_id=identity, target_latest_seq=target,
                                  oldest_seq=page['oldest_seq'], capacity=page['capacity'])
                    if page['oldest_seq'] > 1:
                        result['notes'].append('Earlier rows were overwritten before collection.')
                    if path == '/logs':
                        result.update(select_scalars(page, ['boot_count', 'reset_reason',
                                                            'duplicate_mle_count']))
                require(page['boot_id'] == identity, 'Boot changed during diagnostic pagination.')
                last = cursor
                for row in page[rows_key]:
                    require(isinstance(row, dict) and integer(row.get('seq'),
                            max(last + 1, page['oldest_seq']), page['latest_seq'])
                            and integer(row.get('uptime_ms')))
                    last = row['seq']
                    if last > target:
                        continue
                    if path == '/logs':
                        require(isinstance(row.get('text'), str) and len(row['text']) <= 4096)
                        clean = {'seq': last, 'uptime_ms': row['uptime_ms'],
                                 'text': redact(row['text'], self._token, free_text=True)}
                    else:
                        clean = select_diagnostics(row,
                            numbers=['seq', 'uptime_ms', 'percent', 'batt_mv', 'vbus_mv', 'vsys_mv',
                                     'screen_percent', 'raw_st1', 'raw_st2'], signed=['pmic_temp_c'],
                            flags=['valid', 'present', 'usb', 'charging', 'full', 'estimated'])
                        require(type(clean.get('valid')) is bool)
                    result[rows_key].append(clean)
                result['pages'] += 1
                if target == 0 or last >= target:
                    result['complete'] = True
                    return result
                if last <= cursor:
                    result['notes'].append('Unretrievable tail; pagination made no progress.')
                    return result
                cursor = last
            except RemoteError as error:
                result['notes'].append('Collection stopped: ' + error.kind + '.')
                return result
        result['notes'].append('Diagnostic page limit reached.')
        return result

    def bundle(self):
        before = self.status()
        deadline = self.clock() + 120
        result = {'schema': 1, 'collected_utc': datetime.now(timezone.utc).isoformat(),
                  'status': before,
                  'redaction': 'Status fields are allowlisted; credential log lines, long hex '
                               'values, pairing codes, and the device token are suppressed.',
                  'limitations': ['Free-form log redaction is pattern based; review before sharing.',
                                  'A bundle is an observation, not a consistent memory snapshot.']}
        result['logs'] = {boot: self._pages('/logs', boot=boot, deadline=deadline)
                          for boot in ('current', 'previous')}
        result['power_history'] = self._pages('/power/history', deadline=deadline)
        result['device_boot_unchanged_during_collection'] = None
        if self.clock() < deadline:
            try:
                after = self.status(timeout=deadline - self.clock())
                result['status_after'] = after
                if before['boot_id'] is not None and after['boot_id'] is not None:
                    result['device_boot_unchanged_during_collection'] = before['boot_id'] == after['boot_id']
                else:
                    result['limitations'].append('Boot identity unavailable; continuity is unverified.')
            except RemoteError:
                result['limitations'].append('Final status could not be collected.')
        return redact(result, self._token)


def write_bundle(path, bundle):
    path = Path(path)
    require(path.suffix.lower() in {'.json', '.zip'}, 'Bundle output must end in .json or .zip.')
    payload = (json.dumps(bundle, indent=2, allow_nan=False) + '\n').encode('utf-8')
    created = False
    try:
        with path.open('xb') as stream:
            created = True
            if path.suffix.lower() == '.zip':
                with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
                    archive.writestr('diagnostics.json', payload)
            else:
                stream.write(payload)
    except OSError:
        if created:
            try:
                path.unlink()
            except OSError:
                pass
        raise RemoteError('Could not write the bundle; check the output directory and use a new '
                          'filename. Existing files are never replaced.', kind='output') from None


class SafeParser(argparse.ArgumentParser):
    def __init__(self, *args, **kwargs):
        kwargs['allow_abbrev'] = False
        super().__init__(*args, **kwargs)

    def error(self, message):
        # argparse's default error repeats unknown arguments, possibly including
        # a token incorrectly supplied with an unsupported --token option.
        self.print_usage(sys.stderr)
        self.exit(2, 'Invalid arguments. Use --help; tokens belong in the environment or a file.\n')


def main(argv=None):
    parser = SafeParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ip', required=True, help='explicit local numeric IPv4 or IPv6 address')
    parser.add_argument('--port', type=int, default=8081)
    parser.add_argument('--token-file', type=Path)
    parser.add_argument('--timeout', type=float, default=5, help='per-request timeout, at most 30 seconds')
    commands = parser.add_subparsers(dest='command', required=True, parser_class=SafeParser)
    commands.add_parser('status')
    reboot = commands.add_parser('reboot', help='restart the device once, then verify its new boot')
    reboot.add_argument('--poll-seconds', type=float, default=90, help='observation deadline, at most 90 seconds')
    bundle = commands.add_parser('bundle')
    bundle.add_argument('--output', required=True, type=Path, help='new .zip or .json file')
    args = parser.parse_args(argv)
    token = None
    try:
        token = load_token(args.token_file)
        client = Client(args.ip, token, port=args.port, timeout=args.timeout)
        if args.command == 'status':
            result, success = client.status(), True
        elif args.command == 'reboot':
            result = client.reboot(poll_seconds=args.poll_seconds)
            success = result['reboot_verified'] and result['health_verified']
        else:
            result = client.bundle()
            write_bundle(args.output, result)
            result, success = {'written': str(args.output), 'credential_endpoints_accessed': False}, True
        print(json.dumps(redact(result, token), indent=2, allow_nan=False))
        return 0 if success else 1
    except RemoteError as error:
        message = str(error) if token is None else redact(str(error), token)
        print(message, file=sys.stderr)
        return 1
    except Exception:
        print('Operation failed; private request, response, and token details were not logged.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
