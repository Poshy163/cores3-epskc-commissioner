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
#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/thread.h"

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

static void br_start_task(void *arg)
{
    esp_netif_t *backbone = (esp_netif_t *) arg;

    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_openthread_set_backbone_netif(backbone);
    esp_err_t err = esp_openthread_border_router_init();
    if (err != ESP_OK) {
        esp_openthread_lock_release();
        ESP_LOGE(TAG, "border_router_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
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
        err = esp_openthread_auto_start(&ds);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "auto_start failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "border router up; no credentials stored yet");
    }
    esp_openthread_lock_release();

    s_br_started = true;
    /* Note: meshcop is published by the stack on attach, not here -- watch for
     * "Failed to publish meshcop mdns service" rather than trusting this line. */
    ESP_LOGI(TAG, "border router init done");
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
    s_br_starting = true;

    /*
     * Deferred to its own task on purpose. This is called from the Wi-Fi
     * IP_EVENT handler, which runs on `sys_evt` -- a task with a small stack.
     * Running border_router_init() + auto_start() there overflows it and
     * reboots the board.
     */
    if (xTaskCreate(br_start_task, "ot_br_start", 8192, backbone, 4, NULL) != pdPASS) {
        s_br_starting = false;
        ESP_LOGE(TAG, "could not start border router task");
        return ESP_ERR_NO_MEM;
    }
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
