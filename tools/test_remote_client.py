"""Exercise the remote PC helper with fake HTTP, clock, and files only."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request
import zipfile

import remote


TOKEN = 'a' * 64  # Synthetic test value; no environment/device token is used.


def status(boot='1111111111111111', **changes):
    value = {
        'schema': 1, 'boot_id': boot, 'boot_count': 4, 'reset_reason': 'software', 'uptime_ms': 123456,
        'firmware': {'version': 'test', 'idf': 'v6.0.2', 'elf_sha256': 'b' * 64},
        'wifi': {'connected': True, 'ipv4': '192.168.1.62', 'ipv6': 'fd00::1', 'rssi': -60,
                 'channel': 6, 'associated_ms': 123000, 'disconnect_count': 2,
                 'last_disconnect_reason': 201, 'last_disconnect_age_ms': 120000,
                 'supervisor_on': True, 'retry_step': 0, 'reconnect_pending': False},
        'heap': {'internal_free': 40000, 'internal_largest': 20000,
                 'internal_minimum': 30000, 'psram_free': 5000000, 'lvgl_min_stack_free': 2000},
        'thread': {'available': True, 'attached': True, 'role': 'router', 'attach_duration_s': 80,
                   'known_routers': 5, 'direct_children': 2, 'parent_rssi': None,
                   'mac_tx': 500, 'mac_rx': 1200, 'ip_tx_success': 90, 'ip_rx_success': 80,
                   'ip_tx_failure': 3, 'ip_rx_failure': 2, 'parent_changes': 1},
        'matter': {'status': 'commissioned', 'commissioned': True,
                   'commissioning_window_open': False, 'endpoint': 1},
        'power': {'available': True, 'valid': True, 'fresh': True, 'age_ms': 1000, 'percent': 80,
                  'battery_mv': 4000, 'vbus_mv': 5000, 'pmic_temp_c': 30,
                  'usb': True, 'charging': False, 'full': False},
        'remote': {'enabled': True, 'reboot_pending': False},
        'logs': {'available': True, 'oldest_seq': 1, 'latest_seq': 32, 'duplicate_mle_count': 45},
    }
    value.update(changes)
    return value


class Response:
    def __init__(self, data, code=200, raw=False):
        self.status = code
        self.body = data if raw else json.dumps(data).encode('utf-8')
    def read(self, limit):
        return self.body[:limit]
    def __enter__(self):
        return self
    def __exit__(self, *args):
        return False


class FakeOpener:
    def __init__(self, responses):
        self.responses = list(responses)
        self.calls = []
    def open(self, request, timeout):
        self.calls.append((request, timeout))
        if not self.responses:
            raise AssertionError('Unexpected fake HTTP request')
        response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        if callable(response):
            response = response(request, timeout)
        return response


class Clock:
    def __init__(self):
        self.now = 0
    def __call__(self):
        return self.now
    def sleep(self, seconds):
        self.now += seconds


def client(responses, clock=None):
    opener = FakeOpener(responses)
    clock = clock or Clock()
    return remote.Client('192.168.1.62', TOKEN, opener=opener, clock=clock, sleep=clock.sleep), opener


def http_error(code):
    # Both response and exception text intentionally contain the fake token.
    return urllib.error.HTTPError('http://private/' + TOKEN, code, TOKEN, {}, io.BytesIO(TOKEN.encode()))


def log_page(rows, latest=3, *, boot='current', identity='1111111111111111', oldest=1):
    return {'schema': 1, 'boot': boot, 'boot_id': identity, 'boot_count': 7,
            'reset_reason': 'software', 'oldest_seq': oldest, 'latest_seq': latest,
            'capacity': 1024, 'duplicate_mle_count': 999,
            'lines': [{'seq': seq, 'uptime_ms': seq * 1000, 'text': text} for seq, text in rows]}


def power_page(sequences, latest=3):
    return {'schema': 1, 'boot_id': '2222222222222222', 'oldest_seq': 1,
            'latest_seq': latest, 'capacity': 8640, 'device_uptime_ms': 10000,
            'samples': [{'seq': seq, 'uptime_ms': seq * 1000, 'valid': True,
                         'percent': 80, 'batt_mv': 4000, 'present': True, 'usb': False,
                         'charging': False, 'full': False, 'estimated': False,
                         'screen_percent': 50, 'raw_st1': 0, 'raw_st2': 0,
                         'vbus_mv': 0, 'vsys_mv': 4000, 'pmic_temp_c': 30,
                         'unexpected_secret': TOKEN} for seq in sequences]}


class RemoteTests(unittest.TestCase):
    def test_local_numeric_origins_only(self):
        self.assertEqual(remote.local_origin('192.168.1.62'), 'http://192.168.1.62:8081')
        self.assertEqual(remote.local_origin('fd12::1'), 'http://[fd12::1]:8081')
        self.assertEqual(remote.local_origin('fe80::1%12'), 'http://[fe80::1%2512]:8081')
        for host in ['8.8.8.8', '1.1.1.1', '2606:4700:4700::1111', 'example.com',
                     'http://192.168.1.62', '192.168.1.62/path', '0.0.0.0', '::',
                     '224.0.0.1', '255.255.255.255', '::ffff:8.8.8.8',
                     'fd00::1%bad', 'fe80::1%12\r\nAuthorization:bad']:
            with self.subTest(host=host), self.assertRaises(remote.RemoteError):
                remote.local_origin(host)

    def test_token_environment_and_file(self):
        self.assertTrue(remote.load_token(environ={remote.TOKEN_ENV: TOKEN}) == TOKEN)
        for value in ['', 'a' * 63, 'a' * 65, 'A' * 64, 'g' * 64, TOKEN + '\nextra']:
            with self.assertRaises(remote.RemoteError) as error:
                remote.load_token(environ={remote.TOKEN_ENV: value})
            self.assertTrue(TOKEN not in str(error.exception))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'device.token'
            path.write_text(TOKEN + '\n', encoding='ascii')
            self.assertTrue(remote.load_token(path, environ={remote.TOKEN_ENV: 'wrong'}) == TOKEN)
            path.write_bytes(b'\xff')
            with self.assertRaises(remote.RemoteError):
                remote.load_token(path)

    def test_proxy_bypass_and_redirect_refusal(self):
        with patch.object(remote.urllib.request, 'build_opener') as build:
            remote.Client('192.168.1.62', TOKEN)
        handlers = build.call_args.args
        self.assertTrue(any(isinstance(h, urllib.request.ProxyHandler) and h.proxies == {} for h in handlers))
        redirect = next(h for h in handlers if isinstance(h, remote.NoRedirect))
        self.assertIsNone(redirect.redirect_request(None, None, 302, '', {}, 'http://8.8.8.8/'))

    def test_status_auth_allowlist_and_redaction(self):
        raw = status()
        raw['unexpected'] = {'password': 'private'}
        raw['wifi']['password'] = 'private-password'
        raw['power']['token'] = TOKEN
        raw['firmware']['version'] = TOKEN.upper()
        api, opener = client([Response(raw)])
        result = api.status()
        encoded = json.dumps(result)
        self.assertTrue(TOKEN not in encoded.lower() and 'private-password' not in encoded)
        self.assertEqual(result['firmware']['version'], '[REDACTED]')
        request, timeout = opener.calls[0]
        self.assertEqual(request.full_url, 'http://192.168.1.62:8081/device/status')
        self.assertTrue(request.get_header('Authorization') == 'Bearer ' + TOKEN)
        self.assertEqual(timeout, 5)

    def test_disabled_unauthorized_redirect_and_private_transport_errors(self):
        for response, kind in [(http_error(401), 'unauthorized'), (http_error(503), 'unavailable'),
                               (http_error(302), 'http'), (OSError(TOKEN), 'transport')]:
            api, opener = client([response])
            with self.assertRaises(remote.RemoteError) as error:
                api.status()
            self.assertEqual(error.exception.kind, kind)
            self.assertTrue(TOKEN not in str(error.exception))
            self.assertEqual(len(opener.calls), 1)

    def test_invalid_or_oversized_status(self):
        cases = [Response(status(schema=True)), Response(status(boot_id='bad')),
                 Response(status(thread={'role': 'router', 'attached': 'yes'})),
                 Response(b'not json ' + TOKEN.encode(), raw=True),
                 Response(b'x' * (remote.MAX_RESPONSE + 1), raw=True)]
        for response in cases:
            api, _ = client([response])
            with self.assertRaises(remote.RemoteError) as error:
                api.status()
            self.assertTrue(TOKEN not in str(error.exception))
        api, _ = client([Response(status(boot_id=None))])
        self.assertIsNone(api.status()['boot_id'])

    def test_all_actual_diagnostics_survive_status_and_bundle(self):
        raw = status()
        validated = remote.validate_status(raw, TOKEN)
        self.assertEqual(validated, raw)
        api, _ = client([Response(raw), http_error(404), http_error(404),
                         http_error(404), Response(raw)])
        bundle = api.bundle()
        self.assertEqual(bundle['status'], raw)
        self.assertEqual(bundle['status_after'], raw)
        for before, after in [(False, False), (True, True), (False, True), (True, False)]:
            old = status(matter={'status': 'commissionable' if not before else 'commissioned',
                                 'commissioned': before})
            new = status(matter={'status': 'commissionable' if not after else 'commissioned',
                                 'commissioned': after})
            health = remote.Client.health(old, new)
            self.assertEqual(health['commissioning_state_preserved'], before == after)
            self.assertEqual(health['ready'], before == after)

    def test_numeric_diagnostic_fields_cannot_smuggle_text(self):
        for section, key in [('heap', 'internal_free'), ('power', 'percent'),
                             ('wifi', 'last_disconnect_reason'), ('thread', 'mac_tx')]:
            raw = status()
            raw[section][key] = 'private-password'
            with self.assertRaises(remote.RemoteError):
                remote.validate_status(raw, TOKEN)
        page = power_page([1])
        page['samples'][0]['batt_mv'] = 'private-password'
        api, _ = client([Response(status()), http_error(404), http_error(404),
                         Response(page), Response(status())])
        bundle = api.bundle()
        self.assertFalse(bundle['power_history']['complete'])
        self.assertTrue('private-password' not in json.dumps(bundle))

    def test_reboot_waits_for_new_boot_and_ready_health(self):
        not_ready = status('3333333333333333', thread={'available': False, 'attached': False,
                                                    'role': 'unavailable'})
        ready = status('3333333333333333')
        api, opener = client([Response(status()), Response({'accepted': True, 'reboot_pending': True}, 202),
                              OSError(TOKEN), Response(status()), Response(not_ready), Response(ready)])
        result = api.reboot(poll_seconds=10)
        self.assertTrue(result['request_accepted'] and result['reboot_verified'] and result['health_verified'])
        posts = [request for request, _ in opener.calls if request.method == 'POST']
        self.assertEqual(len(posts), 1)
        self.assertEqual(posts[0].data, b'"reboot"')
        self.assertEqual(posts[0].get_header('Content-type'), 'application/json')
        self.assertTrue(posts[0].get_header('Authorization') == 'Bearer ' + TOKEN)
        self.assertTrue(all(0 < timeout <= 5 for _, timeout in opener.calls))
        self.assertTrue(TOKEN not in json.dumps(result))

    def test_reboot_unknown_post_is_never_retried(self):
        for response in [OSError(TOKEN), Response({'unexpected': TOKEN}, 202),
                         Response(b'bad ' + TOKEN.encode(), 202, raw=True)]:
            api, opener = client([Response(status()), response, Response(status('3333333333333333'))])
            result = api.reboot(poll_seconds=3)
            self.assertIsNone(result['request_accepted'])
            self.assertTrue(result['reboot_verified'] and result['health_verified'])
            self.assertEqual(sum(req.method == 'POST' for req, _ in opener.calls), 1)
            self.assertTrue(TOKEN not in json.dumps(result))

    def test_reboot_auth_rejection_does_not_poll_or_retry(self):
        api, opener = client([Response(status()), http_error(401)])
        with self.assertRaises(remote.RemoteError) as error:
            api.reboot(poll_seconds=3)
        self.assertEqual(error.exception.kind, 'unauthorized')
        self.assertEqual(len(opener.calls), 2)

    def test_reboot_202_alone_is_not_success(self):
        api, opener = client([Response(status()), Response({'accepted': True, 'reboot_pending': True}, 202)]
                              + [Response(status()) for _ in range(3)])
        result = api.reboot(poll_seconds=3)
        self.assertTrue(result['request_accepted'])
        self.assertFalse(result['reboot_verified'] or result['health_verified'])
        self.assertEqual(result['observation_seconds'], 3)
        self.assertEqual(sum(req.method == 'POST' for req, _ in opener.calls), 1)

    def test_reboot_boot_change_without_preserved_commissioning_is_limited(self):
        unhealthy = status('3333333333333333', matter={'status': 'commissionable', 'commissioned': False})
        api, _ = client([Response(status()), Response({'accepted': True, 'reboot_pending': True}, 202)]
                        + [Response(unhealthy) for _ in range(3)])
        result = api.reboot(poll_seconds=3)
        self.assertTrue(result['reboot_verified'])
        self.assertFalse(result['health_verified'])
        self.assertFalse(result['health']['commissioning_state_preserved'])
        api, opener = client([Response(status(boot_id=None))])
        with self.assertRaises(remote.RemoteError):
            api.reboot(poll_seconds=3)
        self.assertEqual(len(opener.calls), 1)

    def test_bundle_paginates_initial_bounds_and_excludes_credentials(self):
        api, opener = client([
            Response(status()),
            Response(log_page([(1, 'normal'), (2, 'password: private-password')])),
            Response(log_page([(3, TOKEN), (4, 'newer than initial snapshot')], latest=4)),
            http_error(404),
            Response(power_page([1, 2])), Response(power_page([3])), Response(status()),
        ])
        bundle = api.bundle()
        self.assertTrue(bundle['logs']['current']['complete'])
        self.assertEqual([row['seq'] for row in bundle['logs']['current']['lines']], [1, 2, 3])
        self.assertEqual([row['seq'] for row in bundle['power_history']['samples']], [1, 2, 3])
        self.assertFalse(bundle['logs']['previous']['available'])
        self.assertTrue(bundle['device_boot_unchanged_during_collection'])
        encoded = json.dumps(bundle)
        self.assertTrue(TOKEN not in encoded and 'private-password' not in encoded)
        self.assertTrue('unexpected_secret' not in encoded)
        for request, _ in opener.calls:
            self.assertTrue('/dataset' not in request.full_url and '/ba-epskc' not in request.full_url)
            if '/device/' not in request.full_url:
                self.assertIsNone(request.get_header('Authorization'))
        self.assertIn('after=2', opener.calls[2][0].full_url)
        with tempfile.TemporaryDirectory() as directory:
            for suffix in ['.json', '.zip']:
                path = Path(directory) / ('diagnostics' + suffix)
                remote.write_bundle(path, bundle)
                if suffix == '.zip':
                    with zipfile.ZipFile(path) as archive:
                        self.assertEqual(archive.namelist(), ['diagnostics.json'])
                        payload = archive.read('diagnostics.json').decode()
                else:
                    payload = path.read_text(encoding='utf-8')
                self.assertTrue(TOKEN not in payload and 'private-password' not in payload)
                original = path.read_bytes()
                with self.assertRaises(remote.RemoteError):
                    remote.write_bundle(path, {})
                self.assertTrue(path.read_bytes() == original)

    def test_bundle_nonprogress_and_boot_changes_do_not_loop_or_mix(self):
        api, opener = client([Response(status()), Response(log_page([], latest=3)),
                              http_error(404), Response(power_page([], latest=3)), Response(status())])
        bundle = api.bundle()
        self.assertFalse(bundle['logs']['current']['complete'])
        self.assertFalse(bundle['power_history']['complete'])
        self.assertEqual(len(opener.calls), 5)
        api, opener = client([Response(status()), Response(log_page([(1, 'one')])),
            Response(log_page([(2, 'different boot')], identity='4444444444444444')),
            http_error(404), http_error(404), Response(status('3333333333333333'))])
        bundle = api.bundle()
        self.assertEqual(len(bundle['logs']['current']['lines']), 1)
        self.assertFalse(bundle['logs']['current']['complete'])
        self.assertFalse(bundle['device_boot_unchanged_during_collection'])

    def test_log_redaction_patterns(self):
        for text in [TOKEN, TOKEN.upper(), 'Bearer secret-data', 'token: abc', 'tap=123456789',
                     'Network Key len=16 00112233445566778899aabbccddeeff', 'MT:NOTAREALQRCODE',
                     'manual code: 34970112332', 'password: a-short-secret']:
            value = remote.redact(text, TOKEN, free_text=True)
            self.assertTrue(text not in value)
        self.assertEqual(remote.redact('ephemeral key closed', TOKEN, free_text=True),
                         'ephemeral key closed')

    def test_cli_never_echoes_token_argument_or_private_exception(self):
        stream = io.StringIO()
        with contextlib.redirect_stderr(stream), self.assertRaises(SystemExit):
            remote.main(['--ip', '192.168.1.62', '--token', TOKEN, 'status'])
        self.assertTrue(TOKEN not in stream.getvalue())
        stream = io.StringIO()
        with patch.dict(remote.os.environ, {remote.TOKEN_ENV: TOKEN}), \
             patch.object(remote.Client, 'status', side_effect=OSError(TOKEN)), \
             contextlib.redirect_stderr(stream):
            self.assertEqual(remote.main(['--ip', '192.168.1.62', 'status']), 1)
        self.assertTrue(TOKEN not in stream.getvalue())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', help='accepted for the shared host-test runner; no C compiler is used')
    parser.parse_args()
    unittest.main(argv=['test_remote_client.py'], verbosity=2)
