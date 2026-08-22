#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

/*
 * OpenThread on the ESP32-S3, talking spinel over UART to the ESP32-H2 RCP.
 *
 * The H2 still runs the RCP firmware from the stock border-router image; only
 * the S3 application was replaced, so the radio side is untouched.
 */
esp_err_t thread_init(void);

/* True once the host stack and RCP are available. */
bool thread_available(void);

/*
 * Promote to a full Border Router: publishes _meshcop._udp on the Wi-Fi side
 * (which is what makes it appear in Home Assistant's Thread panel) and routes
 * IPv6 between the mesh and the LAN.
 *
 * Must be called once Wi-Fi has an address -- the backbone netif has to be up.
 * If credentials are already stored, this also re-attaches to that network,
 * which is how the device comes back after a reboot.
 */
esp_err_t thread_start_border_router(esp_netif_t *backbone);

/*
 * The blocking body of the above, for callers that already own a task with a
 * big enough stack (the UI worker). thread_start_border_router() spawns a
 * fresh 8 KB task for it, and that spawn fails silently when internal RAM is
 * fragmented at boot -- the border router then never starts.
 */
void thread_run_border_router_start(esp_netif_t *backbone);

/* Latest border-router startup outcome, retained after the boot logs scroll
 * past. The returned string has static lifetime. */
const char *thread_border_router_status(void);

/* Apply a freshly retrieved dataset and attach to that network. */
esp_err_t thread_join(const uint8_t *dataset_tlvs, size_t len);

/*
 * Form a brand-new network with freshly generated credentials (random key,
 * PSKc, extended PAN, channel) and attach to it. As the only device it
 * becomes leader, and the border agent publishes it as its own network in
 * Home Assistant's Thread panel. Outputs identify the network on the UI.
 */
esp_err_t thread_form_network(uint8_t channel, char *name, size_t name_len,
                              int *channel_out, uint16_t *panid);

/* Wipe stored Thread credentials and detach. */
esp_err_t thread_forget(void);

/* True if an Active Operational Dataset is stored. */
bool thread_has_dataset(void);

/* "disabled" / "detached" / "child" / "router" / "leader", for the UI. */
const char *thread_role(void);

/* True once attached as child, router or leader. */
bool thread_attached(void);

/*
 * Average RSSI in dBm of the radio link to the parent. False when there is no
 * parent to measure: a router or leader has none, and OpenThread reports
 * INVALID_STATE for those roles.
 */
bool thread_link_rssi(int8_t *rssi);

/*
 * One coherent, lock-protected view of the local Thread attachment. MAC frame
 * and IPv6 packet counters run since stack start; callers can retain samples
 * to calculate a recent traffic window without resetting the stack.
 */
typedef struct {
    char role[9];
    bool attached;
    uint32_t attach_duration_s;
    bool parent_valid;
    uint16_t parent_rloc16;
    bool parent_rssi_valid;
    int8_t parent_rssi;
    uint8_t parent_lqi;
    uint8_t known_routers;
    bool direct_children_valid;
    uint16_t direct_children;
    uint32_t mac_tx_total;
    uint32_t mac_rx_total;
    uint32_t ip_tx_success;
    uint32_t ip_rx_success;
    uint32_t ip_tx_failure;
    uint32_t ip_rx_failure;
    uint16_t parent_changes;
} thread_activity_t;

bool thread_activity_get(thread_activity_t *activity);

/*
 * Network-wide Mesh Diagnostics are asynchronous and best-effort. "Seen"
 * counts describe devices whose routers answered this scan; they must never be
 * presented as an exact census of every device connected to the network.
 */
typedef enum {
    THREAD_TOPOLOGY_NEVER,
    THREAD_TOPOLOGY_SCANNING,
    THREAD_TOPOLOGY_COMPLETE,
    THREAD_TOPOLOGY_PARTIAL,
    THREAD_TOPOLOGY_FAILED,
} thread_topology_status_t;

typedef struct {
    thread_topology_status_t status;
    uint16_t routers_seen;
    uint16_t children_seen;
    uint16_t border_routers_seen;
    bool includes_self;
    uint64_t completed_at_ms;
} thread_topology_t;

esp_err_t thread_topology_refresh(void);
void thread_topology_get(thread_topology_t *topology);

/*
 * Share this device's network over ePSKc: generates a 9-digit code, opens the
 * ephemeral-key DTLS listener and advertises _meshcop-e._udp so a commissioner
 * (Home Assistant, the PC tool) can pull the credentials. `code` receives the
 * digits. One-shot: the key dies on first use, on timeout, or on stop.
 */
esp_err_t thread_share_start(char *code, size_t cap, uint32_t lifetime_ms);
void thread_share_stop(void);
/* "off" / "waiting" / "connected" / "accepted" */
const char *thread_share_state(void);

/* Active Operational Dataset as lowercase hex TLVs, the form HA's Thread panel
 * accepts for "add network". Contains the network key. */
bool thread_dataset_hex(char *out, size_t cap);

/* Children attached directly to this device (meaningful as router/leader). */
int thread_child_count(void);

/*
 * Router preference. Off (default): stay whatever the mesh makes us, which on
 * a mesh already rich in routers means child. On: mark ourselves router-
 * eligible and, whenever we find ourselves a child, ask to be promoted.
 * thread_apply_router_preference() is cheap and rate-limited; call it from a
 * periodic timer.
 */
void thread_set_prefer_router(bool on);
void thread_apply_router_preference(void);

/*
 * Values the OTBR REST API reports. All return false when the stack is not
 * running or the value is unavailable; buffers get lowercase hex, no prefix.
 */
bool thread_border_agent_id_hex(char *out, size_t cap);   /* 16 bytes -> 32 chars */
bool thread_ext_address_hex(char *out, size_t cap);       /* 8 bytes  -> 16 chars */
bool thread_ext_panid_hex(char *out, size_t cap);         /* 8 bytes  -> 16 chars */
const char *thread_network_name(void);
uint16_t thread_rloc16(void);

/* Apply an Active Operational Dataset given as hex TLVs (REST PUT). */
esp_err_t thread_set_dataset_hex(const char *hex);

/* Bring the Thread interface up or down (REST POST /node/state). */
esp_err_t thread_set_enabled(bool on);

/*
 * ePSKc over REST (python-otbr-api #267). Separate from thread_share_state()
 * because the REST contract uses OpenThread's own state names rather than the
 * friendlier words the touch UI shows.
 */
bool thread_epskc_feature_enabled(void);
void thread_epskc_set_feature_enabled(bool on);
const char *thread_share_state_rest(void);   /* disabled|stopped|started|connected|accepted */
uint16_t thread_share_port(void);
/* port 0 lets the stack choose. */
esp_err_t thread_share_start_on(char *code, size_t cap, uint32_t lifetime_ms, uint16_t port);

/*
 * Live network identity, straight from the stored Active Dataset. Use this
 * rather than a cached copy: credentials can change without the UI being
 * involved at all (a dataset pushed over REST by Home Assistant, or `newnet`
 * from the console), and a cache silently goes stale.
 */
bool thread_network_info(char *name, size_t name_len, int *channel, uint16_t *panid);
