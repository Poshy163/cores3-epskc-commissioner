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
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"

static const char *TAG = "thread";

#define PIN_RCP_RX    10   /* S3 receives here */
#define PIN_RCP_TX    17   /* S3 transmits here */
#define PIN_RCP_RESET 7

static bool s_ready;
static bool s_br_started;
static bool s_br_starting;
static esp_netif_t *s_netif;

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
const char *g_br_boot_status = "not started";

void thread_run_border_router_start(esp_netif_t *backbone)
{
    if (!s_ready || backbone == NULL) {
        g_br_boot_status = "br start skipped (thread not ready)";
        return;
    }
    if (s_br_started || s_br_starting) {
        return;
    }
    s_br_starting = true;
    g_br_boot_status = "br start running";

    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_openthread_set_backbone_netif(backbone);
    esp_err_t err = esp_openthread_border_router_init();
    if (err != ESP_OK) {
        esp_openthread_lock_release();
        ESP_LOGE(TAG, "border_router_init failed: %s", esp_err_to_name(err));
        g_br_boot_status = "border_router_init FAILED";
        s_br_starting = false;
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
        g_br_boot_status = "re-attach started";
        err = esp_openthread_auto_start(&ds);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "auto_start failed: %s", esp_err_to_name(err));
            g_br_boot_status = "auto_start FAILED";
        }
    } else {
        ESP_LOGI(TAG, "border router up; no credentials stored yet");
        g_br_boot_status = "no stored credentials";
    }
    esp_openthread_lock_release();

    s_br_started = true;
    g_br_boot_status = "border router up";
    /* Note: meshcop is published by the stack on attach, not here -- watch for
     * "Failed to publish meshcop mdns service" rather than trusting this line. */
    ESP_LOGI(TAG, "border router init done");
}

/* Fallback for headless boots (no UI worker): own task, own stack. */
static void br_start_task(void *arg)
{
    thread_run_border_router_start((esp_netif_t *) arg);
    vTaskDelete(NULL);
}

esp_err_t thread_start_border_router(esp_netif_t *backbone)
{
    if (!s_ready || backbone == NULL) {
        return ESP_FAIL;
    }
    if (s_br_started || s_br_starting) {
        return ESP_OK;
    }

    /*
     * Deferred to its own task on purpose. This is called from the Wi-Fi
     * IP_EVENT handler, which runs on `sys_evt` -- a task with a small stack.
     * Running border_router_init() + auto_start() there overflows it and
     * reboots the board.
     */
    if (xTaskCreate(br_start_task, "ot_br_start", 8192, backbone, 4, NULL) != pdPASS) {
        g_br_boot_status = "br task spawn FAILED (no internal RAM)";
        ESP_LOGE(TAG, "could not start border router task");
        return ESP_ERR_NO_MEM;
    }
    g_br_boot_status = "br task spawned";
    return ESP_OK;
}

esp_err_t thread_forget(void)
{
    if (!s_ready) {
        return ESP_FAIL;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();
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

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();

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
        ESP_LOGW(TAG, "could not erase stored network info (%d); attaching anyway", oterr);
    }

    /* auto_start sets the dataset and brings the interface up, and is the same
     * path used on reboot -- keeping both routes identical. */
    esp_err_t err = esp_openthread_auto_start(&ds);
    esp_openthread_lock_release();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "attach failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "dataset applied, attaching...");
    return ESP_OK;
}

esp_err_t thread_form_network(char *name, size_t name_len, int *channel, uint16_t *panid)
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
    if (channel != NULL) {
        *channel = ds.mChannel;
    }
    if (panid != NULL) {
        *panid = ds.mPanId;
    }

    /* Same route as joining someone else's network: erase the old identity,
     * apply the dataset, attach. Sole member of a new PAN -> leader. */
    return thread_join(tlvs.mTlvs, tlvs.mLength);
}

const char *thread_role(void)
{
    if (!s_ready) {
        return "disabled";
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otDeviceRole role = otThreadGetDeviceRole(esp_openthread_get_instance());
    esp_openthread_lock_release();

    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return "disabled";
    case OT_DEVICE_ROLE_DETACHED: return "detached";
    case OT_DEVICE_ROLE_CHILD:    return "child";
    case OT_DEVICE_ROLE_ROUTER:   return "router";
    case OT_DEVICE_ROLE_LEADER:   return "leader";
    default:                      return "?";
    }
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
        err = otBorderAgentEphemeralKeyStart(ins, tap.mTap, lifetime_ms, 0);
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

const char *thread_share_state(void)
{
    if (!s_ready) {
        return "off";
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otBorderAgentEphemeralKeyState st = otBorderAgentEphemeralKeyGetState(esp_openthread_get_instance());
    esp_openthread_lock_release();
    switch (st) {
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
    int n = 0;
    esp_openthread_lock_acquire(portMAX_DELAY);
    uint16_t max = otThreadGetMaxAllowedChildren(ins);
    for (uint16_t i = 0; i < max; i++) {
        otChildInfo ci;
        if (otThreadGetChildInfoByIndex(ins, i, &ci) == OT_ERROR_NONE) {
            n++;
        }
    }
    esp_openthread_lock_release();
    return n;
}
