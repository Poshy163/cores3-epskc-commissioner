/*
 * OpenThread host stack driving the on-board ESP32-H2 radio co-processor.
 *
 * Pin assignments come from M5Stack's own border-router sdkconfig, so they are
 * ground truth rather than inferred from the schematic:
 *     PIN_TO_RCP_TX = 10   (the H2's TX -> our RX)
 *     PIN_TO_RCP_RX = 17   (the H2's RX -> our TX)
 *     PIN_TO_RCP_RESET = 7
 */
#include "thread.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "openthread/border_agent.h"
#include "openthread/border_agent_ephemeral_key.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"
#include "openthread/link.h"
#include "openthread/mesh_diag.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"

static const char *TAG = "thread";

#define PIN_RCP_RX    10   /* S3 receives here */
#define PIN_RCP_TX    17   /* S3 transmits here */
#define PIN_RCP_RESET 7

static bool s_ready;
static bool s_br_started;
static bool s_br_starting;
static portMUX_TYPE s_br_start_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_netif_t *s_netif;

/* Mesh Diagnostics calls back on OpenThread's own task with the OT API lock
 * already held. Keep that callback tiny: copy aggregate values behind a
 * spinlock, then let the UI poll the published snapshot. */
static portMUX_TYPE s_topology_lock = portMUX_INITIALIZER_UNLOCKED;
static thread_topology_t s_topology;
static uint64_t s_topology_router_ids;
static bool s_topology_missing_child_table;

static void topology_reset(thread_topology_status_t status)
{
    taskENTER_CRITICAL(&s_topology_lock);
    memset(&s_topology, 0, sizeof(s_topology));
    s_topology.status = status;
    s_topology_router_ids = 0;
    s_topology_missing_child_table = false;
    taskEXIT_CRITICAL(&s_topology_lock);
}

static void topology_discover_cb(otError error, otMeshDiagRouterInfo *router, void *context)
{
    (void) context;
    uint16_t children = 0;
    uint16_t child_border_routers = 0;
    bool child_is_self = false;

    /* Iterators are valid only during this callback. Consume the child table
     * before publishing anything, and do not call any locking wrapper here. */
    if (router != NULL && router->mChildIterator != NULL) {
        otMeshDiagChildInfo child;
        while (otMeshDiagGetNextChildInfo(router->mChildIterator, &child) == OT_ERROR_NONE) {
            children++;
            child_border_routers += child.mIsBorderRouter ? 1 : 0;
            child_is_self = child_is_self || child.mIsThisDevice;
        }
    }

    bool finished = error != OT_ERROR_PENDING;
    taskENTER_CRITICAL(&s_topology_lock);
    if (s_topology.status == THREAD_TOPOLOGY_SCANNING && router != NULL &&
        router->mRouterId < 64) {
        uint64_t bit = UINT64_C(1) << router->mRouterId;
        if ((s_topology_router_ids & bit) == 0) {
            s_topology_router_ids |= bit;
            s_topology_missing_child_table = s_topology_missing_child_table ||
                                             router->mChildIterator == NULL;
            s_topology.routers_seen++;
            s_topology.children_seen += children;
            s_topology.border_routers_seen +=
                (router->mIsBorderRouter ? 1 : 0) + child_border_routers;
            s_topology.includes_self = s_topology.includes_self ||
                                       router->mIsThisDevice || child_is_self;
        }
    }
    if (s_topology.status == THREAD_TOPOLOGY_SCANNING && finished) {
        bool saw_any = s_topology.routers_seen != 0 || s_topology.children_seen != 0;
        if (error == OT_ERROR_NONE && !s_topology_missing_child_table) {
            s_topology.status = THREAD_TOPOLOGY_COMPLETE;
        } else if (saw_any) {
            s_topology.status = THREAD_TOPOLOGY_PARTIAL;
        } else {
            s_topology.status = THREAD_TOPOLOGY_FAILED;
        }
        s_topology.completed_at_ms = (uint64_t) esp_timer_get_time() / 1000;
    }
    taskEXIT_CRITICAL(&s_topology_lock);
}

esp_err_t thread_topology_refresh(void)
{
    if (!s_ready) {
        topology_reset(THREAD_TOPOLOGY_FAILED);
        return ESP_ERR_INVALID_STATE;
    }

    /* OT -> topology is the lock order used by callbacks and cancellation.
     * Claim both sides atomically so a concurrent join cannot reset the state
     * between marking it SCANNING and starting the diagnostic query. */
    esp_openthread_lock_acquire(portMAX_DELAY);
    taskENTER_CRITICAL(&s_topology_lock);
    if (s_topology.status == THREAD_TOPOLOGY_SCANNING) {
        taskEXIT_CRITICAL(&s_topology_lock);
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_topology, 0, sizeof(s_topology));
    s_topology.status = THREAD_TOPOLOGY_SCANNING;
    s_topology_router_ids = 0;
    s_topology_missing_child_table = false;
    taskEXIT_CRITICAL(&s_topology_lock);

    const otMeshDiagDiscoverConfig config = {
        .mDiscoverIp6Addresses = false,
        .mDiscoverChildTable = true,
    };
    otError error = otMeshDiagDiscoverTopology(esp_openthread_get_instance(), &config,
                                                topology_discover_cb, NULL);
    if (error != OT_ERROR_NONE) {
        taskENTER_CRITICAL(&s_topology_lock);
        memset(&s_topology, 0, sizeof(s_topology));
        s_topology.status = THREAD_TOPOLOGY_FAILED;
        s_topology_router_ids = 0;
        s_topology_missing_child_table = false;
        taskEXIT_CRITICAL(&s_topology_lock);
    }
    esp_openthread_lock_release();

    if (error == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "topology scan started");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "topology scan could not start (%d)", error);
    if (error == OT_ERROR_NO_BUFS) {
        return ESP_ERR_NO_MEM;
    }
    if (error == OT_ERROR_INVALID_STATE || error == OT_ERROR_BUSY) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}

void thread_topology_get(thread_topology_t *topology)
{
    if (topology == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_topology_lock);
    *topology = s_topology;
    taskEXIT_CRITICAL(&s_topology_lock);
}

bool thread_available(void)
{
    return s_ready;
}

bool thread_has_dataset(void)
{
    if (!s_ready) {
        return false;
    }
    otOperationalDatasetTlvs ds;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &ds);
    esp_openthread_lock_release();
    return err == OT_ERROR_NONE && ds.mLength > 0;
}

static void rcp_reset(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_RCP_RESET,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_RCP_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RCP_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void ot_task(void *arg)
{
    (void) arg;
    esp_openthread_platform_config_t cfg = {
        .radio_config = {
            .radio_mode = RADIO_MODE_UART_RCP,
            .radio_uart_config = {
                .port = UART_NUM_1,
                .uart_config = {
                    .baud_rate = 460800,
                    .data_bits = UART_DATA_8_BITS,
                    .parity = UART_PARITY_DISABLE,
                    .stop_bits = UART_STOP_BITS_1,
                    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                    .rx_flow_ctrl_thresh = 0,
                    .source_clk = UART_SCLK_DEFAULT,
                },
                .rx_pin = PIN_RCP_RX,
                .tx_pin = PIN_RCP_TX,
            },
        },
        .host_config = { .host_connection_mode = HOST_CONNECTION_MODE_NONE },
        .port_config = { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 },
    };

    if (esp_openthread_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_openthread_init failed");
        vTaskDelete(NULL);
        return;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    s_netif = esp_netif_new(&netif_cfg);
    /* Not fatal: aborting here takes the whole device down over a radio that
     * the commissioner half does not need. */
    esp_err_t aerr = s_netif ? esp_netif_attach(s_netif, esp_openthread_netif_glue_init(&cfg))
                             : ESP_FAIL;
    if (aerr != ESP_OK) {
        ESP_LOGE(TAG, "netif attach failed (%s) - Thread disabled", esp_err_to_name(aerr));
        esp_openthread_deinit();
        vTaskDelete(NULL);
        return;
    }

    s_ready = true;
    ESP_LOGI(TAG, "OpenThread stack running (RCP over UART%d)", UART_NUM_1);

    esp_openthread_launch_mainloop();   /* does not return */

    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(s_netif);
    esp_openthread_deinit();
    vTaskDelete(NULL);
}

esp_err_t thread_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    esp_vfs_eventfd_config_t ev = { .max_fds = 3 };
    esp_err_t err = esp_vfs_eventfd_register(&ev);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    rcp_reset();

    if (xTaskCreate(ot_task, "ot_main", 10240, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start OpenThread task");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < 100 && !s_ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return s_ready ? ESP_OK : ESP_FAIL;
}

/* Boot-time BR outcome, queryable later: the logs of this task land in the
 * USB re-enumeration blackout after a port-open reset, so nothing printed
 * here is ever seen. */
static const char *s_br_boot_status = "not started";

const char *thread_border_router_status(void)
{
    return s_br_boot_status;
}

static bool claim_border_router_start(void)
{
    bool claimed = false;
    taskENTER_CRITICAL(&s_br_start_lock);
    if (!s_br_started && !s_br_starting) {
        s_br_starting = true;
        claimed = true;
    }
    taskEXIT_CRITICAL(&s_br_start_lock);
    return claimed;
}

static void finish_border_router_start(bool started)
{
    taskENTER_CRITICAL(&s_br_start_lock);
    s_br_started = started;
    s_br_starting = false;
    taskEXIT_CRITICAL(&s_br_start_lock);
}

/* Run after the caller has atomically reserved s_br_starting. */
static void run_border_router_start_claimed(esp_netif_t *backbone)
{
    if (!s_ready || backbone == NULL) {
        s_br_boot_status = "br start skipped (thread not ready)";
        finish_border_router_start(false);
        return;
    }
    s_br_boot_status = "br start running";

    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_openthread_set_backbone_netif(backbone);
    esp_err_t err = esp_openthread_border_router_init();
    if (err != ESP_OK) {
        esp_openthread_lock_release();
        ESP_LOGE(TAG, "border_router_init failed: %s", esp_err_to_name(err));
        s_br_boot_status = "border_router_init FAILED";
        finish_border_router_start(false);
        return;
    }

    /*
     * Re-attach with whatever is already stored. This is what brings the device
     * back onto its network after a power cycle -- without it the credentials
     * survive but the radio sits idle until someone runs the join flow again.
     * A NULL dataset would make auto_start form a brand new network, which is
     * emphatically not wanted here, so only start when one exists.
     */
    otOperationalDatasetTlvs ds;
    otError oterr = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &ds);
    if (oterr == OT_ERROR_NONE && ds.mLength > 0) {
        ESP_LOGI(TAG, "stored credentials found, re-attaching");
        s_br_boot_status = "border router up; re-attach started";
        err = esp_openthread_auto_start(&ds);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "auto_start failed: %s", esp_err_to_name(err));
            s_br_boot_status = "border router up; auto_start FAILED";
        }
    } else {
        ESP_LOGI(TAG, "border router up; no credentials stored yet");
        s_br_boot_status = "border router up; no stored credentials";
    }
    esp_openthread_lock_release();

    finish_border_router_start(true);
    /* Note: meshcop is published by the stack on attach, not here -- watch for
     * "Failed to publish meshcop mdns service" rather than trusting this line. */
    ESP_LOGI(TAG, "border router init done");
}

void thread_run_border_router_start(esp_netif_t *backbone)
{
    if (!s_ready || backbone == NULL) {
        s_br_boot_status = "br start skipped (thread not ready)";
        return;
    }
    if (!claim_border_router_start()) {
        return;
    }
    run_border_router_start_claimed(backbone);
}

/* Fallback for headless boots (no UI worker): own task, own stack. */
static void br_start_task(void *arg)
{
    run_border_router_start_claimed((esp_netif_t *) arg);
    vTaskDelete(NULL);
}

esp_err_t thread_start_border_router(esp_netif_t *backbone)
{
    if (!s_ready || backbone == NULL) {
        return ESP_FAIL;
    }
    if (!claim_border_router_start()) {
        return ESP_OK;
    }

    /*
     * Deferred to its own task on purpose. This is called from the Wi-Fi
     * IP_EVENT handler, which runs on `sys_evt` -- a task with a small stack.
     * Running border_router_init() + auto_start() there overflows it and
     * reboots the board.
     */
    if (xTaskCreate(br_start_task, "ot_br_start", 8192, backbone, 4, NULL) != pdPASS) {
        finish_border_router_start(false);
        s_br_boot_status = "br task spawn FAILED (no internal RAM)";
        ESP_LOGE(TAG, "could not start border router task");
        return ESP_ERR_NO_MEM;
    }
    s_br_boot_status = "br task spawned";
    return ESP_OK;
}

esp_err_t thread_forget(void)
{
    if (!s_ready) {
        return ESP_FAIL;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();
    otMeshDiagCancel(ins);
    topology_reset(THREAD_TOPOLOGY_NEVER);
    otThreadSetEnabled(ins, false);
    otIp6SetEnabled(ins, false);
    otError err = otInstanceErasePersistentInfo(ins);
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "erase failed: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Thread credentials erased");
    return ESP_OK;
}

esp_err_t thread_join(const uint8_t *tlvs, size_t len)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "stack not running");
        return ESP_FAIL;
    }
    if (len == 0 || len > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        ESP_LOGE(TAG, "dataset length %u out of range", (unsigned) len);
        return ESP_ERR_INVALID_ARG;
    }

    otOperationalDatasetTlvs ds;
    memset(&ds, 0, sizeof(ds));
    memcpy(ds.mTlvs, tlvs, len);
    ds.mLength = (uint8_t) len;

    /* This OpenThread release validates TLV framing, values and duplicates in
     * otDatasetParseTlvs(), but predates otDatasetIsValid(). Check the complete
     * Active Dataset contract explicitly before touching the working network. */
    otOperationalDataset parsed;
    otError parse_err = otDatasetParseTlvs(&ds, &parsed);
    const otOperationalDatasetComponents *c = &parsed.mComponents;
    bool complete = parse_err == OT_ERROR_NONE &&
                    c->mIsActiveTimestampPresent && c->mIsChannelPresent &&
                    c->mIsChannelMaskPresent && c->mIsExtendedPanIdPresent &&
                    c->mIsMeshLocalPrefixPresent && c->mIsNetworkKeyPresent &&
                    c->mIsNetworkNamePresent && c->mIsPanIdPresent &&
                    c->mIsPskcPresent && c->mIsSecurityPolicyPresent;
    if (!complete) {
        ESP_LOGE(TAG, "invalid operational dataset (%d)", parse_err);
        return ESP_ERR_INVALID_ARG;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();
    otMeshDiagCancel(ins);
    topology_reset(THREAD_TOPOLOGY_NEVER);

    /* Keep an in-memory rollback copy. WIFI_STORAGE_FLASH has an equivalent
     * safeguard in app_wifi_join(); Thread replacement should be no easier to
     * strand than Wi-Fi replacement. */
    otOperationalDatasetTlvs previous;
    bool had_previous = otDatasetGetActiveTlvs(ins, &previous) == OT_ERROR_NONE &&
                        previous.mLength > 0;

    /*
     * Wipe stored network info before applying the new dataset.
     *
     * This board's `nvs` partition still holds OpenThread settings from the
     * stock border-router firmware. Without this, the stack logs "Attempting to
     * restore prev role: leader", tries to resume an rloc from a partition that
     * no longer exists, and sits in `detached` sending Link Requests instead of
     * attaching as a child. Erase requires the stack to be disabled first.
     */
    otThreadSetEnabled(ins, false);
    otIp6SetEnabled(ins, false);
    otError oterr = otInstanceErasePersistentInfo(ins);
    if (oterr != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "could not erase stored network info (%d); candidate not applied", oterr);
        if (had_previous) {
            esp_err_t restore_err = esp_openthread_auto_start(&previous);
            if (restore_err != ESP_OK) {
                ESP_LOGE(TAG, "could not resume previous dataset: %s",
                         esp_err_to_name(restore_err));
            }
        }
        esp_openthread_lock_release();
        return ESP_FAIL;
    }

    /* auto_start sets the dataset and brings the interface up, and is the same
     * path used on reboot -- keeping both routes identical. */
    esp_err_t err = esp_openthread_auto_start(&ds);
    if (err != ESP_OK) {
        if (had_previous) {
            esp_err_t restore_err = esp_openthread_auto_start(&previous);
            if (restore_err == ESP_OK) {
                ESP_LOGW(TAG, "new dataset rejected; previous dataset restored");
            } else {
                ESP_LOGE(TAG, "new dataset and rollback both failed: %s",
                         esp_err_to_name(restore_err));
            }
        } else {
            /* auto_start may persist the candidate before a later enable step
             * fails. With no previous dataset, erase it so a reported failure
             * cannot become the next boot's credentials. */
            otThreadSetEnabled(ins, false);
            otIp6SetEnabled(ins, false);
            otError erase_err = otInstanceErasePersistentInfo(ins);
            if (erase_err != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "failed candidate could not be erased: %d", erase_err);
            }
        }
    }
    esp_openthread_lock_release();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "attach failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "dataset applied, attaching...");
    return ESP_OK;
}

esp_err_t thread_form_network(uint8_t channel, char *name, size_t name_len,
                              int *channel_out, uint16_t *panid)
{
    if (!s_ready) {
        return ESP_FAIL;
    }
    otInstance *ins = esp_openthread_get_instance();
    otOperationalDataset ds;
    otOperationalDatasetTlvs tlvs;

    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otDatasetCreateNewNetwork(ins, &ds);
    if (err == OT_ERROR_NONE) {
        /* Recognisable on the HA Thread panel; suffix keeps repeated
         * generations distinct. */
        snprintf(ds.mNetworkName.m8, sizeof(ds.mNetworkName.m8), "CoreS3-%02X%02X",
                 ds.mExtendedPanId.m8[6], ds.mExtendedPanId.m8[7]);
        ds.mComponents.mIsNetworkNamePresent = true;
        if (channel >= 11 && channel <= 26) {
            ds.mChannel = channel;
            ds.mComponents.mIsChannelPresent = true;
        }
        otDatasetConvertToTlvs(&ds, &tlvs);
    }
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "could not generate dataset (%d)", err);
        return ESP_FAIL;
    }

    if (name != NULL) {
        snprintf(name, name_len, "%s", ds.mNetworkName.m8);
    }
    if (channel_out != NULL) {
        *channel_out = ds.mChannel;
    }
    if (panid != NULL) {
        *panid = ds.mPanId;
    }

    /* Same route as joining someone else's network: erase the old identity,
     * apply the dataset, attach. Sole member of a new PAN -> leader. */
    return thread_join(tlvs.mTlvs, tlvs.mLength);
}

static const char *role_name(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return "disabled";
    case OT_DEVICE_ROLE_DETACHED: return "detached";
    case OT_DEVICE_ROLE_CHILD:    return "child";
    case OT_DEVICE_ROLE_ROUTER:   return "router";
    case OT_DEVICE_ROLE_LEADER:   return "leader";
    default:                      return "?";
    }
}

/* Caller holds the OpenThread API lock. */
static uint16_t count_children_locked(otInstance *instance)
{
    uint16_t count = 0;
    uint16_t max = otThreadGetMaxAllowedChildren(instance);
    for (uint16_t i = 0; i < max; i++) {
        otChildInfo child;
        if (otThreadGetChildInfoByIndex(instance, i, &child) == OT_ERROR_NONE) {
            count++;
        }
    }
    return count;
}

const char *thread_role(void)
{
    if (!s_ready) {
        return "disabled";
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otDeviceRole role = otThreadGetDeviceRole(esp_openthread_get_instance());
    esp_openthread_lock_release();
    return role_name(role);
}

bool thread_attached(void)
{
    const char *r = thread_role();
    return strcmp(r, "child") == 0 || strcmp(r, "router") == 0 || strcmp(r, "leader") == 0;
}

bool thread_link_rssi(int8_t *rssi)
{
    if (!s_ready) {
        return false;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otThreadGetParentAverageRssi(esp_openthread_get_instance(), rssi);
    esp_openthread_lock_release();

    /* 127 is OpenThread's "no measurement yet" sentinel, which shows up in the
     * window between attaching and the first parent frame. */
    return err == OT_ERROR_NONE && *rssi != 127;
}

bool thread_activity_get(thread_activity_t *activity)
{
    if (!s_ready || activity == NULL) {
        return false;
    }

    memset(activity, 0, sizeof(*activity));
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    otDeviceRole role = otThreadGetDeviceRole(instance);
    activity->attached = role == OT_DEVICE_ROLE_CHILD ||
                         role == OT_DEVICE_ROLE_ROUTER ||
                         role == OT_DEVICE_ROLE_LEADER;
    snprintf(activity->role, sizeof(activity->role), "%s", role_name(role));

    if (activity->attached) {
        activity->attach_duration_s = otThreadGetCurrentAttachDuration(instance);
        uint8_t max_router_id = otThreadGetMaxRouterId(instance);
        for (uint16_t router_id = 0; router_id <= max_router_id; router_id++) {
            otRouterInfo router_info;
            if (otThreadGetRouterInfo(instance, router_id, &router_info) == OT_ERROR_NONE &&
                router_info.mAllocated) {
                activity->known_routers++;
            }
        }
    }

    if (role == OT_DEVICE_ROLE_CHILD) {
        otRouterInfo parent;
        if (otThreadGetParentInfo(instance, &parent) == OT_ERROR_NONE) {
            activity->parent_valid = true;
            activity->parent_rloc16 = parent.mRloc16;
            activity->parent_lqi = parent.mLinkQualityIn;
        }
        int8_t rssi;
        if (otThreadGetParentAverageRssi(instance, &rssi) == OT_ERROR_NONE && rssi != 127) {
            activity->parent_rssi_valid = true;
            activity->parent_rssi = rssi;
        }
    } else if (role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER) {
        activity->direct_children_valid = true;
        activity->direct_children = count_children_locked(instance);
    }

    const otMacCounters *mac = otLinkGetCounters(instance);
    if (mac != NULL) {
        activity->mac_tx_total = mac->mTxTotal;
        activity->mac_rx_total = mac->mRxTotal;
    }
    const otIpCounters *ip = otThreadGetIp6Counters(instance);
    if (ip != NULL) {
        activity->ip_tx_success = ip->mTxSuccess;
        activity->ip_rx_success = ip->mRxSuccess;
        activity->ip_tx_failure = ip->mTxFailure;
        activity->ip_rx_failure = ip->mRxFailure;
    }
    const otMleCounters *mle = otThreadGetMleCounters(instance);
    if (mle != NULL) {
        activity->parent_changes = mle->mParentChanges;
    }
    esp_openthread_lock_release();
    return true;
}

/* ---------------- sharing (ePSKc border-agent side) ---------------- */

static bool s_share_cb_set;
static bool s_meshcop_e_published;

/*
 * IDF wires the _meshcop-e publish/remove events but nothing in it ever posts
 * them; advertising the ephemeral-key listener is the application's job.
 * Runs on the OpenThread task with the stack lock held.
 */
static void share_state_cb(void *ctx)
{
    (void) ctx;
    otInstance *ins = esp_openthread_get_instance();
    otBorderAgentEphemeralKeyState st = otBorderAgentEphemeralKeyGetState(ins);

    if (st == OT_BORDER_AGENT_STATE_STARTED && !s_meshcop_e_published) {
        uint16_t port = otBorderAgentEphemeralKeyGetUdpPort(ins);
        const char *nn = otThreadGetNetworkName(ins);
        mdns_txt_item_t txt[] = {
            { "rv", "1" },
            { "tv", "1.4.0" },
            { "vn", "OpenThread" },
            { "mn", "BorderRouter" },
            { "nn", nn ? nn : "" },
        };
        esp_err_t err = mdns_service_add(NULL, "_meshcop-e", "_udp", port, txt,
                                         sizeof(txt) / sizeof(txt[0]));
        s_meshcop_e_published = (err == ESP_OK);
        ESP_LOGI(TAG, "ephemeral key listening on udp/%u (%s)", port,
                 err == ESP_OK ? "advertised" : "advertise FAILED");
    } else if ((st == OT_BORDER_AGENT_STATE_STOPPED || st == OT_BORDER_AGENT_STATE_DISABLED)
               && s_meshcop_e_published) {
        mdns_service_remove("_meshcop-e", "_udp");
        s_meshcop_e_published = false;
        ESP_LOGI(TAG, "ephemeral key closed, _meshcop-e withdrawn");
    }
}

esp_err_t thread_share_start(char *code, size_t cap, uint32_t lifetime_ms)
{
    return thread_share_start_on(code, cap, lifetime_ms, 0);
}

esp_err_t thread_share_start_on(char *code, size_t cap, uint32_t lifetime_ms, uint16_t port)
{
    if (!s_ready || !s_br_started || cap < 10) {
        return ESP_ERR_INVALID_STATE;
    }
    otInstance *ins = esp_openthread_get_instance();
    otBorderAgentEphemeralKeyTap tap;
    otError err;

    esp_openthread_lock_acquire(portMAX_DELAY);
    if (!s_share_cb_set) {
        otBorderAgentEphemeralKeySetCallback(ins, share_state_cb, NULL);
        s_share_cb_set = true;
    }
    otBorderAgentEphemeralKeySetEnabled(ins, true);
    err = otBorderAgentEphemeralKeyGenerateTap(&tap);
    if (err == OT_ERROR_NONE) {
        /* Port 0: let the stack pick, and read it back in the callback. */
        err = otBorderAgentEphemeralKeyStart(ins, tap.mTap, lifetime_ms, port);
    }
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "ephemeral key start failed (%d)", err);
        return ESP_FAIL;
    }
    snprintf(code, cap, "%s", tap.mTap);
    return ESP_OK;
}

void thread_share_stop(void)
{
    if (!s_ready) {
        return;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otBorderAgentEphemeralKeyStop(esp_openthread_get_instance());
    esp_openthread_lock_release();
}

/* One read of the stack state; the two callers word it differently. */
static otBorderAgentEphemeralKeyState epskc_state(void)
{
    if (!s_ready) {
        return OT_BORDER_AGENT_STATE_DISABLED;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otBorderAgentEphemeralKeyState st =
        otBorderAgentEphemeralKeyGetState(esp_openthread_get_instance());
    esp_openthread_lock_release();
    return st;
}

const char *thread_share_state(void)
{
    switch (epskc_state()) {
    case OT_BORDER_AGENT_STATE_STARTED:   return "waiting";
    case OT_BORDER_AGENT_STATE_CONNECTED: return "connected";
    case OT_BORDER_AGENT_STATE_ACCEPTED:  return "accepted";
    default:                              return "off";
    }
}

bool thread_dataset_hex(char *out, size_t cap)
{
    if (!s_ready) {
        return false;
    }
    otOperationalDatasetTlvs ds;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &ds);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE || ds.mLength == 0 || cap < (size_t) ds.mLength * 2 + 1) {
        return false;
    }
    for (size_t i = 0; i < ds.mLength; i++) {
        snprintf(out + 2 * i, 3, "%02x", ds.mTlvs[i]);
    }
    return true;
}

int thread_child_count(void)
{
    if (!s_ready) {
        return 0;
    }
    otInstance *ins = esp_openthread_get_instance();
    esp_openthread_lock_acquire(portMAX_DELAY);
    int n = count_children_locked(ins);
    esp_openthread_lock_release();
    return n;
}

/* ---------------- router preference ---------------- */

static bool s_prefer_router;
static int64_t s_last_promote_us;

void thread_set_prefer_router(bool on)
{
    s_prefer_router = on;
    if (!s_ready) {
        return;
    }
    /*
     * Router eligibility stays ON regardless of this setting. Clearing it does
     * not merely keep us a child: an ineligible device cannot become leader
     * either, so forming a new network left the device stuck in `detached`
     * forever, publishing no meshcop record and invisible to Home Assistant.
     * The preference only decides whether we actively ask to be promoted while
     * we are a child of someone else's mesh.
     */
    esp_openthread_lock_acquire(portMAX_DELAY);
    otThreadSetRouterEligible(esp_openthread_get_instance(), true);
    esp_openthread_lock_release();
    if (on) {
        s_last_promote_us = 0;   /* allow an immediate attempt */
        thread_apply_router_preference();
    }
}

void thread_apply_router_preference(void)
{
    if (!s_prefer_router || !s_ready) {
        return;
    }
    /* A promotion request is an Address Solicit to the leader; once every
     * 30 s is plenty and keeps a refusing leader from being spammed. */
    int64_t now = esp_timer_get_time();
    if (now - s_last_promote_us < 30LL * 1000 * 1000) {
        return;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();
    if (otThreadGetDeviceRole(ins) == OT_DEVICE_ROLE_CHILD) {
        s_last_promote_us = now;
        otError err = otThreadBecomeRouter(ins);
        ESP_LOGI(TAG, "asked for router role (%d)", err);
    }
    esp_openthread_lock_release();
}

/* ---------------- values for the REST API ---------------- */

static void bytes_to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = H[b[i] >> 4];
        out[2 * i + 1] = H[b[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

bool thread_border_agent_id_hex(char *out, size_t cap)
{
    if (!s_ready || cap < OT_BORDER_AGENT_ID_LENGTH * 2 + 1) {
        return false;
    }
    otBorderAgentId id;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otBorderAgentGetId(esp_openthread_get_instance(), &id);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE) {
        return false;
    }
    bytes_to_hex(id.mId, OT_BORDER_AGENT_ID_LENGTH, out);
    return true;
}

bool thread_ext_address_hex(char *out, size_t cap)
{
    if (!s_ready || cap < OT_EXT_ADDRESS_SIZE * 2 + 1) {
        return false;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    const otExtAddress *a = otLinkGetExtendedAddress(esp_openthread_get_instance());
    if (a != NULL) {
        bytes_to_hex(a->m8, OT_EXT_ADDRESS_SIZE, out);
    }
    esp_openthread_lock_release();
    return a != NULL;
}

bool thread_ext_panid_hex(char *out, size_t cap)
{
    if (!s_ready || cap < OT_EXT_PAN_ID_SIZE * 2 + 1) {
        return false;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    const otExtendedPanId *x = otThreadGetExtendedPanId(esp_openthread_get_instance());
    if (x != NULL) {
        bytes_to_hex(x->m8, OT_EXT_PAN_ID_SIZE, out);
    }
    esp_openthread_lock_release();
    return x != NULL;
}

const char *thread_network_name(void)
{
    if (!s_ready) {
        return "";
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    const char *n = otThreadGetNetworkName(esp_openthread_get_instance());
    esp_openthread_lock_release();
    return n ? n : "";
}

uint16_t thread_rloc16(void)
{
    if (!s_ready) {
        return 0;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    uint16_t r = otThreadGetRloc16(esp_openthread_get_instance());
    esp_openthread_lock_release();
    return r;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

esp_err_t thread_set_dataset_hex(const char *hex)
{
    size_t len = strlen(hex);
    if (len == 0 || (len % 2) != 0 || len / 2 > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tlvs[OT_OPERATIONAL_DATASET_MAX_LENGTH];
    for (size_t i = 0; i < len / 2; i++) {
        int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        tlvs[i] = (uint8_t) ((hi << 4) | lo);
    }
    return thread_join(tlvs, len / 2);
}

esp_err_t thread_set_enabled(bool on)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();
    if (!on) {
        otMeshDiagCancel(ins);
        topology_reset(THREAD_TOPOLOGY_NEVER);
    }
    otError err = otIp6SetEnabled(ins, on);
    if (err == OT_ERROR_NONE) {
        err = otThreadSetEnabled(ins, on);
    }
    esp_openthread_lock_release();
    return err == OT_ERROR_NONE ? ESP_OK : ESP_FAIL;
}

bool thread_epskc_feature_enabled(void)
{
    return epskc_state() != OT_BORDER_AGENT_STATE_DISABLED;
}

void thread_epskc_set_feature_enabled(bool on)
{
    if (!s_ready) {
        return;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();
    if (on && !s_share_cb_set) {
        otBorderAgentEphemeralKeySetCallback(ins, share_state_cb, NULL);
        s_share_cb_set = true;
    }
    otBorderAgentEphemeralKeySetEnabled(ins, on);
    esp_openthread_lock_release();
}

const char *thread_share_state_rest(void)
{
    switch (epskc_state()) {
    case OT_BORDER_AGENT_STATE_STOPPED:   return "stopped";
    case OT_BORDER_AGENT_STATE_STARTED:   return "started";
    case OT_BORDER_AGENT_STATE_CONNECTED: return "connected";
    case OT_BORDER_AGENT_STATE_ACCEPTED:  return "accepted";
    default:                              return "disabled";
    }
}

uint16_t thread_share_port(void)
{
    if (!s_ready) {
        return 0;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    uint16_t p = otBorderAgentEphemeralKeyGetUdpPort(esp_openthread_get_instance());
    esp_openthread_lock_release();
    return p;
}

bool thread_network_info(char *name, size_t name_len, int *channel, uint16_t *panid)
{
    if (!s_ready) {
        return false;
    }
    otOperationalDataset ds;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otDatasetGetActive(esp_openthread_get_instance(), &ds);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE) {
        return false;
    }
    if (name != NULL && name_len > 0) {
        if (ds.mComponents.mIsNetworkNamePresent) {
            snprintf(name, name_len, "%s", ds.mNetworkName.m8);
        } else {
            name[0] = '\0';
        }
    }
    if (channel != NULL) {
        *channel = ds.mComponents.mIsChannelPresent ? ds.mChannel : 0;
    }
    if (panid != NULL) {
        *panid = ds.mComponents.mIsPanIdPresent ? ds.mPanId : 0;
    }
    return true;
}
