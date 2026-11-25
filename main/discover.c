/* mDNS discovery of Thread border agents advertising an active ephemeral key. */
#include "discover.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip6_addr.h"
#include "mdns.h"

static const char *TAG = "discover";
static bool s_started;

/*
 * The address the ePSKc client should use: IPv4 when the agent has it, as
 * before; otherwise a global or ULA IPv6 address, and a link-local one only as
 * a last resort, scoped to the Wi-Fi station ("fe80::...%st1") so lwIP knows
 * which link it is on. Agents on IPv6-only networks publish only AAAA records
 * and were skipped. meshcop.c resolves all three forms numerically.
 */
static bool pick_address(const mdns_ip_addr_t *list, char *out, size_t cap)
{
    const esp_ip6_addr_t *routable = NULL;
    const esp_ip6_addr_t *link_local = NULL;
    for (const mdns_ip_addr_t *a = list; a != NULL; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            snprintf(out, cap, IPSTR, IP2STR(&a->addr.u_addr.ip4));
            return true;
        }
        if (a->addr.type != ESP_IPADDR_TYPE_V6) {
            continue;
        }
        const ip6_addr_t *v6 = (const ip6_addr_t *) &a->addr.u_addr.ip6;
        if (ip6_addr_islinklocal(v6)) {
            link_local = link_local ? link_local : &a->addr.u_addr.ip6;
        } else if (!ip6_addr_isany(v6) && !ip6_addr_ismulticast(v6) && routable == NULL) {
            routable = &a->addr.u_addr.ip6;
        }
    }
    const esp_ip6_addr_t *chosen = routable ? routable : link_local;
    if (chosen == NULL) {
        return false;
    }
    char text[IP6ADDR_STRLEN_MAX];
    if (ip6addr_ntoa_r((const ip6_addr_t *) chosen, text, sizeof(text)) == NULL) {
        return false;
    }
    if (chosen == routable) {
        snprintf(out, cap, "%s", text);
        return true;
    }
    char ifname[8] = "";
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL || esp_netif_get_netif_impl_name(sta, ifname) != ESP_OK || ifname[0] == '\0') {
        return false;
    }
    return snprintf(out, cap, "%s%%%s", text, ifname) < (int) cap;
}

esp_err_t discover_init(void)
{
    if (s_started) {
        return ESP_OK;
    }
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
        return err;
    }
    /* Matter owns the responder's global host label. The saved user-facing
     * name is applied to the MeshCoP service instance and TBR cluster instead. */
    s_started = true;
    return ESP_OK;
}

int discover_border_agents(ba_entry_t *out, int max, uint32_t timeout_ms)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    if (discover_init() != ESP_OK) {
        return 0;
    }

    mdns_result_t *results = NULL;
    size_t result_limit = (size_t) (max < BA_MAX ? max : BA_MAX);
    esp_err_t err = mdns_query_ptr("_meshcop-e", "_udp", timeout_ms, result_limit, &results);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "query failed: %s", esp_err_to_name(err));
        return 0;
    }

    int n = 0;
    for (mdns_result_t *r = results; r != NULL && n < max; r = r->next) {
        /* Only usable with an address record to talk to. */
        if (r->addr == NULL || !pick_address(r->addr, out[n].ip, sizeof(out[n].ip))) {
            continue;
        }

        snprintf(out[n].name, BA_NAME_LEN, "%s",
                 r->instance_name ? r->instance_name : "(unnamed)");
        out[n].port = r->port;
        ESP_LOGI(TAG, "found %s at %s port %u", out[n].name, out[n].ip, out[n].port);
        n++;
    }

    mdns_query_results_free(results);
    return n;
}
