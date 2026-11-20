"""Compile the real status serializer with project headers and SDK-only stubs."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = [args.cc] + (['cc'] if Path(args.cc).stem == 'zig' else [])
    with tempfile.TemporaryDirectory(prefix='device-status-tests-') as tmp:
        tmp = Path(tmp)
        for name in ['esp_err.h', 'esp_log.h', 'esp_netif.h', 'esp_app_desc.h',
                     'esp_heap_caps.h', 'esp_timer.h']:
            (tmp / name).write_text('#include "device_status_sdk.h"\n', encoding='utf-8')
        binary = tmp / 'test-device-status.exe'
        subprocess.run(compiler + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-I', str(tmp), '-I', str(root / 'tools/tests'), '-I', str(root / 'main'),
                       str(root / 'main/device_status.c'),
                       str(root / 'tools/tests/device_status_harness.c'), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
        assert not result.stderr
        snapshots = [json.loads(line) for line in result.stdout.splitlines()]
        assert len(snapshots) == 10
        full, unavailable, invalid, unknown, empty, failed, unterminated, boundary, stale, negative = snapshots
        assert full['schema'] == 1
        assert full['boot_id'] == 'fedcba9876543210' and full['boot_count'] == 4294967295
        assert full['reset_reason'] == 'SW"\\\r\x01' and full['uptime_ms'] == 987654321
        assert full['firmware'] == {'version': 'rev"\\\n', 'idf': 'v6.0.2\t\x01',
                                    'elf_sha256': bytes(range(32)).hex()}
        assert full['wifi'] == {'connected': True, 'rssi': -62, 'channel': 11,
            'ipv4': '1"\\\n.2', 'ipv6': 'fe80::1\t\x01', 'associated_ms': 1099511627776,
            'disconnect_count': 4000000000, 'last_disconnect_reason': 202,
            'last_disconnect_age_ms': 4294967297, 'supervisor_on': True,
            'retry_step': 3, 'reconnect_pending': False}
        assert full['heap'] == {'internal_free': 123456, 'internal_largest': 65432,
            'internal_minimum': 54321, 'psram_free': 6000000, 'lvgl_min_stack_free': 2500}
        assert full['thread'] == {'available': True, 'role': 'r"\\\n', 'attached': True,
            'attach_duration_s': 12345, 'known_routers': 23, 'direct_children': 13,
            'parent_rssi': -69, 'mac_tx': 4294967295, 'mac_rx': 4000000001,
            'ip_tx_success': 1001, 'ip_rx_success': 2002, 'ip_tx_failure': 3,
            'ip_rx_failure': 4, 'parent_changes': 1234}
        assert full['matter'] == {'status': 'commissioned"\\\n', 'commissioned': True,
            'commissioning_window_open': False, 'endpoint': 1}
        assert full['power'] == {'available': True, 'valid': True, 'fresh': True, 'age_ms': 12345,
            'percent': 73, 'battery_mv': 4098, 'vbus_mv': 5021, 'pmic_temp_c': 41,
            'usb': True, 'charging': False, 'full': False}
        assert full['logs'] == {'available': True, 'oldest_seq': 101, 'latest_seq': 1124,
                               'duplicate_mle_count': 4000000002}
        assert full['remote'] == {'enabled': True, 'reboot_pending': True}
        assert unavailable['boot_id'] is None and unavailable['boot_count'] == 0
        for key in ['rssi', 'channel', 'associated_ms', 'last_disconnect_reason',
                    'last_disconnect_age_ms']:
            assert unavailable['wifi'][key] is None
        assert unavailable['thread']['role'] == 'unavailable'
        assert not unavailable['thread']['available'] and not unavailable['thread']['attached']
        assert unavailable['thread']['parent_rssi'] is None
        assert unavailable['thread']['direct_children'] is None
        assert unavailable['thread']['mac_tx'] == 0
        assert not unavailable['power']['available'] and not unavailable['power']['valid']
        assert not unavailable['logs']['available'] and unavailable['logs']['latest_seq'] == 0
        assert invalid['power']['available'] and not invalid['power']['valid']
        assert invalid['power']['age_ms'] == 12345
        assert invalid['thread']['parent_rssi'] is None and invalid['thread']['direct_children'] is None
        for snapshot in [unavailable, invalid, unknown, empty, failed]:
            for key in ['percent', 'battery_mv', 'vbus_mv', 'pmic_temp_c']:
                assert snapshot['power'][key] is None
        assert unknown['power']['valid'] and unknown['power']['age_ms'] is None
        assert not unknown['power']['fresh']
        assert boundary['power']['fresh'] and boundary['power']['age_ms'] == 30000
        assert not stale['power']['fresh'] and stale['power']['valid']
        assert stale['power']['age_ms'] == 30001 and stale['power']['percent'] == 73
        assert not negative['power']['fresh'] and negative['power']['age_ms'] is None
        for snapshot in [unavailable, invalid, empty, failed]:
            assert not snapshot['power']['fresh']
        for snapshot in [empty, failed]:
            assert not snapshot['power']['available'] and snapshot['power']['age_ms'] is None
        assert unterminated['firmware']['version'] == 'V' * 32
        assert unterminated['firmware']['idf'] == 'I' * 32
        allowed = {'schema', 'boot_id', 'boot_count', 'reset_reason', 'uptime_ms',
                   'firmware', 'wifi', 'heap', 'thread', 'matter', 'power', 'logs', 'remote'}
        forbidden = {'token', 'key', 'password', 'ssid', 'dataset', 'network_key',
                     'qr_code', 'manual_code', 'onboarding', 'authorization'}
        def check_privacy(value):
            if isinstance(value, dict):
                assert forbidden.isdisjoint(value)
                for child in value.values():
                    check_privacy(child)
        for snapshot in snapshots:
            assert set(snapshot) == allowed
            check_privacy(snapshot)
        print('PASS production device status: parsed escaped JSON, availability/nulls, 64-bit ages and counters')
        print('PASS production device status: bounded Thread read, cached power/freshness, buffer bounds, private fields absent')
        print('PASS production device status: PSRAM scratch, OOM/null metadata and all failure paths free allocations')


if __name__ == '__main__':
    main()
