/* mDNS discovery of Thread border agents advertising an active ephemeral key. */
#include "discover.h"

#include <string.h>

#include "esp_log.h"
#include "mdns.h"
#include "nvs.h"

static const char *TAG = "discover";
static bool s_started;

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
    /* The hostname doubles as the meshcop instance name in HA's Thread panel,
     * so it is user-settable (console `name` command). Namespace "cfg", not
     * "epskc": the latter is erased wholesale by Forget network. */
    char host[33] = "cores3-thread-br";
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(host);
        nvs_get_str(h, "host", host, &len);
        nvs_close(h);
    }
    mdns_hostname_set(host);
    s_started = true;
    return ESP_OK;
}

int discover_border_agents(ba_entry_t *out, int max, uint32_t timeout_ms)
{
    if (discover_init() != ESP_OK) {
        return 0;
    }

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_meshcop-e", "_udp", timeout_ms, BA_MAX, &results);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "query failed: %s", esp_err_to_name(err));
        return 0;
    }

    int n = 0;
    for (mdns_result_t *r = results; r != NULL && n < max; r = r->next) {
        /* Only usable if we got an A record to talk to. */
        if (r->addr == NULL) {
            continue;
        }
        esp_ip4_addr_t v4 = { 0 };
        bool have_v4 = false;
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                v4 = a->addr.u_addr.ip4;
                have_v4 = true;
                break;
            }
        }
        if (!have_v4) {
            continue;
        }

        snprintf(out[n].name, BA_NAME_LEN, "%s",
                 r->instance_name ? r->instance_name : "(unnamed)");
        snprintf(out[n].ip, sizeof(out[n].ip), IPSTR, IP2STR(&v4));
        out[n].port = r->port;
        ESP_LOGI(TAG, "found %s at %s:%u", out[n].name, out[n].ip, out[n].port);
        n++;
    }

    mdns_query_results_free(results);
    return n;
}
