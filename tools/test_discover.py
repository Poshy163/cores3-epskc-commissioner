"""Run the production border-agent address choice from main/discover.c on a host."""
import argparse
from pathlib import Path
import subprocess
import tempfile

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { uint32_t addr[4]; uint8_t zone; } esp_ip6_addr_t;
typedef struct {
    union { esp_ip6_addr_t ip6; esp_ip4_addr_t ip4; } u_addr;
    uint8_t type;
} esp_ip_addr_t;
#define ESP_IPADDR_TYPE_V4 0
#define ESP_IPADDR_TYPE_V6 6
typedef struct mdns_ip_addr_s {
    esp_ip_addr_t addr;
    struct mdns_ip_addr_s *next;
} mdns_ip_addr_t;
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int) ((a)->addr & 0xff), (int) (((a)->addr >> 8) & 0xff), \
                  (int) (((a)->addr >> 16) & 0xff), (int) (((a)->addr >> 24) & 0xff)

/* lwIP keeps IPv6 addresses in network byte order. */
typedef esp_ip6_addr_t ip6_addr_t;
#define IP6ADDR_STRLEN_MAX 46
static const uint8_t *v6_bytes(const ip6_addr_t *a) { return (const uint8_t *) a->addr; }
#define ip6_addr_islinklocal(a) (v6_bytes(a)[0] == 0xfe && (v6_bytes(a)[1] & 0xc0) == 0x80)
#define ip6_addr_ismulticast(a) (v6_bytes(a)[0] == 0xff)
static bool ip6_addr_isany(const ip6_addr_t *a)
{
    for (int i = 0; i < 16; i++) {
        if (v6_bytes(a)[i] != 0) {
            return false;
        }
    }
    return true;
}
static char *ip6addr_ntoa_r(const ip6_addr_t *a, char *buf, int len)
{
    const uint8_t *b = v6_bytes(a);
    int n = snprintf(buf, (size_t) len, "%x:%x:%x:%x:%x:%x:%x:%x",
                     b[0] << 8 | b[1], b[2] << 8 | b[3], b[4] << 8 | b[5], b[6] << 8 | b[7],
                     b[8] << 8 | b[9], b[10] << 8 | b[11], b[12] << 8 | b[13], b[14] << 8 | b[15]);
    return n > 0 && n < len ? buf : NULL;
}

typedef struct esp_netif_obj { int unused; } esp_netif_t;
static esp_netif_t s_sta;
static bool s_sta_present = true;
static const char *s_sta_name = "st1";
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{
    assert(strcmp(key, "WIFI_STA_DEF") == 0);
    return s_sta_present ? &s_sta : NULL;
}
static esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *netif, char *name)
{
    assert(netif == &s_sta);
    strcpy(name, s_sta_name);
    return name[0] ? ESP_OK : ESP_FAIL;
}

#include "discover_production.inc"

static mdns_ip_addr_t v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    mdns_ip_addr_t r = { .addr = { .type = ESP_IPADDR_TYPE_V4 } };
    r.addr.u_addr.ip4.addr = (uint32_t) a | (uint32_t) b << 8 | (uint32_t) c << 16 | (uint32_t) d << 24;
    return r;
}

static mdns_ip_addr_t v6(const uint8_t bytes[16])
{
    mdns_ip_addr_t r = { .addr = { .type = ESP_IPADDR_TYPE_V6 } };
    memcpy(r.addr.u_addr.ip6.addr, bytes, 16);
    return r;
}

static mdns_ip_addr_t *chain(mdns_ip_addr_t *items, int n)
{
    for (int i = 0; i < n; i++) {
        items[i].next = i + 1 < n ? &items[i + 1] : NULL;
    }
    return items;
}

int main(void)
{
    static const uint8_t ll[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x6a, 0xee, 0x8f, 0xff, 0xfe, 0xd8, 0x34, 0x6c };
    static const uint8_t ula[16] = { 0xfd, 0xc5, 0xff, 0x44, 0x49, 0xb9, 0x49, 0x67, 0x6a, 0xee, 0x8f, 0xff, 0xfe, 0xd8, 0x34, 0x6c };
    static const uint8_t global[16] = { 0x24, 0x04, 0x0e, 0x80, 0xa1, 0xf9, 0, 0, 0x6a, 0xee, 0x8f, 0xff, 0xfe, 0xd8, 0x34, 0x6c };
    static const uint8_t mcast[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfb };
    static const uint8_t any[16] = { 0 };
    char out[48];

    /* IPv4 wins wherever it appears, as before. */
    mdns_ip_addr_t a[] = { v6(ula), v6(ll), v4(192, 168, 1, 62) };
    assert(pick_address(chain(a, 3), out, sizeof(out)) && strcmp(out, "192.168.1.62") == 0);

    /* AAAA-only agents: a routable address beats a link-local one in any order. */
    mdns_ip_addr_t b[] = { v6(ll), v6(ula) };
    assert(pick_address(chain(b, 2), out, sizeof(out)) &&
           strcmp(out, "fdc5:ff44:49b9:4967:6aee:8fff:fed8:346c") == 0);
    mdns_ip_addr_t c[] = { v6(global), v6(ll) };
    assert(pick_address(chain(c, 2), out, sizeof(out)) &&
           strcmp(out, "2404:e80:a1f9:0:6aee:8fff:fed8:346c") == 0);

    /* Link-local only: scoped to the Wi-Fi station so lwIP can route it. */
    mdns_ip_addr_t d[] = { v6(ll) };
    assert(pick_address(chain(d, 1), out, sizeof(out)) &&
           strcmp(out, "fe80:0:0:0:6aee:8fff:fed8:346c%st1") == 0);
    char small[20];
    assert(!pick_address(chain(d, 1), small, sizeof(small)));
    s_sta_present = false;
    assert(!pick_address(chain(d, 1), out, sizeof(out)));
    s_sta_present = true;
    s_sta_name = "";
    assert(!pick_address(chain(d, 1), out, sizeof(out)));
    s_sta_name = "st1";

    /* Records that cannot be dialled are skipped. */
    mdns_ip_addr_t e[] = { v6(mcast), v6(any) };
    assert(!pick_address(chain(e, 2), out, sizeof(out)));
    assert(!pick_address(NULL, out, sizeof(out)));

    puts("PASS border-agent address choice: IPv4 first, routable IPv6, scoped link-local");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'main/discover.c').read_text(encoding='utf-8')
    choice = source[source.index('static bool pick_address('):
                    source.index('esp_err_t discover_init(void)')]
    compiler = [args.cc] + (['cc'] if Path(args.cc).stem == 'zig' else [])
    with tempfile.TemporaryDirectory(prefix='discover-tests-') as directory:
        directory = Path(directory)
        (directory / 'discover_production.inc').write_text(choice, encoding='utf-8')
        harness = directory / 'discover_harness.c'
        harness.write_text(HARNESS, encoding='utf-8')
        exe = directory / 'discover.exe'
        subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-I', str(directory), str(harness), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
