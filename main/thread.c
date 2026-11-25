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

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_meshcop_mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "openthread/border_agent.h"
#include "openthread/border_agent_ephemeral_key.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"
#include "openthread/link.h"
#include "openthread/mesh_diag.h"
#include "openthread/platform/radio.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "rcp_update.h"
#include "settings.h"

static const char *TAG = "thread";

static bool s_ready;
static bool s_br_started;
static bool s_br_starting;
static portMUX_TYPE s_br_start_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_border_router_name[SETTINGS_DEVICE_NAME_BUFFER_SIZE] = SETTINGS_DEFAULT_DEVICE_NAME;

/* ePSKc advertising state; the callback itself lives in the sharing section. */
static bool s_share_cb_set;
static bool s_meshcop_e_published;
static void share_state_cb(void *ctx);

/*
 * mDNS calls must not be made while holding OpenThread's task-switching lock.
 * The responder's task holds its service lock while it waits for lwIP's core
 * lock, and lwIP's thread holds the core lock while it waits for the
 * task-switching lock to transmit on the Thread interface (an MLD report or a
 * routed packet). An mDNS call made with the lock held closes that cycle, and
 * the OpenThread, mDNS, lwIP, esp_timer and UI tasks stop for good. ESP-IDF's
 * own MeshCoP publisher drops the lock around its mDNS calls; these helpers
 * do the same. The caller keeps the API lock, so only lwIP's thread can enter
 * OpenThread meanwhile. Make no OpenThread call between the two.
 */
static void mdns_section_begin(void)
{
    esp_openthread_task_switching_lock_release();
}

static void mdns_section_end(void)
{
    esp_openthread_task_switching_lock_acquire(portMAX_DELAY);
}

/*
 * ---------------- MeshCoP TXT record ----------------
 *
 * ESP-IDF's prebuilt border-router library (6.1.1) publishes _meshcop._udp
 * itself: its state handler adds the service, with an empty TXT record, each
 * time the device attaches, and removes it on detach. Every add also registers
 * the library's own TXT callback, which copies OpenThread's record and then
 * sets mn=BorderRouter and vn=OpenThread; every remove registers NULL.
 * OpenThread keeps one such callback, so each attach used to drop the
 * firmware's, and the model name went back to BorderRouter at the next TXT
 * change. The firmware therefore links with
 * --wrap=otBorderAgentSetMeshCoPServiceChangedCallback (main/CMakeLists.txt):
 * once the border router has started, every registration, the library's
 * included, installs meshcop_service_changed_cb instead. That callback writes
 * OpenThread's record with this device's model name, and does the same for
 * the ephemeral-key service (_meshcop-e), which carries the same record, as
 * ot-br-posix publishes it.
 *
 * meshcop_txt_apply() makes mDNS calls only, so it runs inside an mDNS
 * section; meshcop_txt_apply_locked() reads OpenThread's record first and is
 * called with the stack lock held.
 */
#define MESHCOP_VENDOR_NAME "OpenThread"   /* the library's, and _meshcop-e's */

static void meshcop_txt_apply(const char *service_type,
                              const otBorderAgentMeshCoPServiceTxtData *txt)
{
    if (!mdns_service_exists(service_type, "_udp", NULL)) {
        /* The first callback can fire before the service is added; the
         * responder would log an error for the missing record, and the next
         * change applies it anyway. */
        ESP_LOGD(TAG, "%s not published yet", service_type);
        return;
    }

    const uint16_t length = txt->mLength < sizeof(txt->mData) ? txt->mLength : sizeof(txt->mData);
    size_t capacity = 2;   /* vn and mn */
    for (uint16_t at = 0; at < length; at += 1u + txt->mData[at]) {
        capacity++;
    }
    /* The items, then a copy of the record with each key ended at its '=',
     * so the keys are C strings in place. Values keep explicit lengths:
     * several are binary. */
    mdns_txt_item_with_value_len_t *items = malloc(capacity * sizeof(*items) + length);
    if (items == NULL) {
        ESP_LOGW(TAG, "%s TXT record not refreshed: out of memory", service_type);
        return;
    }
    uint8_t *data = (uint8_t *) &items[capacity];
    memcpy(data, txt->mData, length);

    size_t count = 0;
    for (uint16_t at = 0; at < length;) {
        const uint8_t entry_len = data[at];
        uint8_t *entry = &data[at + 1];
        at += 1u + entry_len;
        if (at > length) {
            break;   /* truncated entry */
        }
        uint8_t *eq = memchr(entry, '=', entry_len);
        if (eq == NULL || eq == entry) {
            continue;
        }
        *eq = '\0';
        const char *key = (const char *) entry;
        if (strcmp(key, "vn") == 0 || strcmp(key, "mn") == 0) {
            continue;   /* set below */
        }
        items[count++] = (mdns_txt_item_with_value_len_t) {
            .key = key,
            .value = eq + 1,
            .value_len = (uint8_t) (entry + entry_len - (eq + 1)),
        };
    }
    items[count++] = (mdns_txt_item_with_value_len_t) {
        .key = "vn",
        .value = (const uint8_t *) MESHCOP_VENDOR_NAME,
        .value_len = sizeof(MESHCOP_VENDOR_NAME) - 1,
    };
    items[count++] = (mdns_txt_item_with_value_len_t) {
        .key = "mn",
        .value = (const uint8_t *) THREAD_MESHCOP_MODEL_NAME,
        .value_len = sizeof(THREAD_MESHCOP_MODEL_NAME) - 1,
    };

    /* One call replaces the whole record: one announcement, and no key
     * outlives OpenThread dropping it (sq and bb go with the BBR role). */
    esp_err_t err = mdns_service_txt_set_with_explicit_txt_item_value_len(
        service_type, "_udp", items, (uint8_t) count);
    free(items);
    if (err == ESP_ERR_NOT_FOUND) {
        /* Removed since the check; the next change applies the record. */
        ESP_LOGD(TAG, "%s not published any more", service_type);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s TXT record not refreshed: %s", service_type, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "%s TXT record refreshed (%u entries)", service_type, (unsigned) count);
}

static void meshcop_txt_apply_locked(otInstance *instance, const char *service_type)
{
    if (!otBorderAgentIsActive(instance)) {
        return;   /* nothing is published while detached */
    }
    otBorderAgentMeshCoPServiceTxtData txt;
    if (otBorderAgentGetMeshCoPServiceTxtData(instance, &txt) != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "could not read the MeshCoP TXT data");
        return;
    }
    mdns_section_begin();
    meshcop_txt_apply(service_type, &txt);
    mdns_section_end();
}

/* Runs on the OpenThread task with the stack lock held. */
static void meshcop_service_changed_cb(void *context)
{
    (void) context;
    otInstance *instance = esp_openthread_get_instance();
    if (otBorderAgentIsActive(instance)) {
        mdns_section_begin();
        const bool published = mdns_service_exists("_meshcop", "_udp", NULL);
        mdns_section_end();
        if (!published) {
            /* Called with the lock held: the library manages it itself. */
            esp_err_t err = esp_openthread_publish_meshcop_mdns(s_border_router_name);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "MeshCoP publication retry failed: %s", esp_err_to_name(err));
                return;
            }
        }
        /* An early role notification can precede the Border Agent socket,
         * and the library reads the port only when it adds the service. */
        const uint16_t port = otBorderAgentGetUdpPort(instance);
        mdns_section_begin();
        esp_err_t err = mdns_service_port_set("_meshcop", "_udp", port);
        mdns_section_end();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MeshCoP port refresh failed: %s", esp_err_to_name(err));
        }
    }
    meshcop_txt_apply_locked(instance, "_meshcop");
    if (s_meshcop_e_published) {
        meshcop_txt_apply_locked(instance, "_meshcop-e");
    }
}

/*
 * Every TXT callback registration lands here (see the MeshCoP section above):
 * the firmware's, and the library's on each publish (its own callback) and
 * remove (NULL). Until the border router has started the library's choice
 * stands, so an early publication cannot run meshcop_service_changed_cb before
 * the saved name is applied. Callers hold the stack lock, as OpenThread
 * requires. Registering also makes OpenThread invoke the callback soon after,
 * so the record is rewritten right after the library has added the service.
 */
static bool s_meshcop_cb_claimed;

void __real_otBorderAgentSetMeshCoPServiceChangedCallback(
    otInstance *instance, otBorderAgentMeshCoPServiceChangedCallback callback, void *context);
void __wrap_otBorderAgentSetMeshCoPServiceChangedCallback(
    otInstance *instance, otBorderAgentMeshCoPServiceChangedCallback callback, void *context);

void __wrap_otBorderAgentSetMeshCoPServiceChangedCallback(
    otInstance *instance, otBorderAgentMeshCoPServiceChangedCallback callback, void *context)
{
    if (s_meshcop_cb_claimed) {
        if (callback != NULL && callback != meshcop_service_changed_cb) {
            ESP_LOGI(TAG, "_meshcop TXT callback kept after the library published");
        }
        callback = meshcop_service_changed_cb;
        context = NULL;
    }
    __real_otBorderAgentSetMeshCoPServiceChangedCallback(instance, callback, context);
}

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

esp_err_t thread_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* ESP-Matter owns the single OpenThread host instance and task.  The rest
     * of this module deliberately only adopts that instance: starting the old
     * standalone mainloop here as well would put two tasks on the H2's UART. */
    otInstance *instance = esp_openthread_get_instance();
    for (int i = 0; i < 100 && !otInstanceIsInitialized(instance); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!otInstanceIsInitialized(instance)) {
        ESP_LOGE(TAG, "Matter-owned OpenThread stack did not become ready");
        return ESP_ERR_TIMEOUT;
    }
    s_ready = true;
    ESP_LOGI(TAG, "adopted Matter OpenThread stack (H2 RCP over UART1)");

    /* Prove the H2 runs the bundled radio firmware, or schedule a reflash. */
    char rcp_version[RCP_UPDATE_VERSION_SIZE];
    if (thread_rcp_version(rcp_version, sizeof(rcp_version))) {
        rcp_update_check_running(rcp_version);
    }
    return ESP_OK;
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

    esp_err_t err = settings_get_device_name(s_border_router_name, sizeof(s_border_router_name));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "using default border-router name: %s", esp_err_to_name(err));
    }
    err = esp_openthread_set_meshcop_instance_name(s_border_router_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not set MeshCoP instance name: %s", esp_err_to_name(err));
        s_br_boot_status = "MeshCoP name setup FAILED";
        finish_border_router_start(false);
        return;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_openthread_set_backbone_netif(backbone);
    err = esp_openthread_border_router_init();
    if (err != ESP_OK) {
        esp_openthread_lock_release();
        ESP_LOGE(TAG, "border_router_init failed: %s", esp_err_to_name(err));
        s_br_boot_status = "border_router_init FAILED";
        finish_border_router_start(false);
        return;
    }
    /* Matter may restore/attach Thread before this deferred BR startup. Drop
     * any early publication so its default instance name or pre-agent port
     * cannot survive after the saved name has been applied. */
    esp_openthread_remove_meshcop_mdns();
    /* OpenThread invokes this immediately and on record changes. Reconcile
     * publication as well as TXT so startup ordering cannot strand discovery.
     * From here on the wrapper keeps it installed whenever the library
     * publishes or removes _meshcop._udp. */
    s_meshcop_cb_claimed = true;
    otBorderAgentSetMeshCoPServiceChangedCallback(esp_openthread_get_instance(),
                                                  meshcop_service_changed_cb, NULL);
    /* border_router_init installed the library's own ephemeral-key callback,
     * which publishes a bare _meshcop-e record with no TXT data and would
     * silently replace ours. OpenThread keeps one callback, so claim it now,
     * after the library, and it stays ours. */
    otBorderAgentEphemeralKeySetCallback(esp_openthread_get_instance(), share_state_cb, NULL);
    s_share_cb_set = true;

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

bool thread_rcp_version(char *out, size_t cap)
{
    if (!s_ready || out == NULL || cap == 0) {
        return false;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    const char *version = otPlatRadioGetVersionString(esp_openthread_get_instance());
    snprintf(out, cap, "%s", version != NULL ? version : "");
    esp_openthread_lock_release();
    return out[0] != '\0';
}

esp_err_t thread_set_border_router_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) > SETTINGS_DEVICE_NAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_border_router_name, sizeof(s_border_router_name), "%s", name);
    if (!s_ready || !s_br_started) {
        return ESP_OK;   /* the border-router start applies it */
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();
    /* Withdraw the old instance first: the responder treats a renamed service
     * as a new one, and two records for one border agent confuse clients. */
    if (s_meshcop_e_published) {
        mdns_section_begin();
        mdns_service_remove("_meshcop-e", "_udp");
        mdns_section_end();
        s_meshcop_e_published = false;
    }
    esp_openthread_remove_meshcop_mdns();
    esp_err_t err = esp_openthread_set_meshcop_instance_name(s_border_router_name);
    otDeviceRole role = otThreadGetDeviceRole(ins);
    bool attached = role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
                    role == OT_DEVICE_ROLE_LEADER;
    if (err == ESP_OK && attached) {
        err = esp_openthread_publish_meshcop_mdns(s_border_router_name);
        if (err == ESP_OK) {
            meshcop_txt_apply_locked(ins, "_meshcop");
        }
    }
    /* If a share is active, re-publish it under the requested instance name
     * instead of leaving the old discovery record behind. */
    share_state_cb(NULL);
    esp_openthread_lock_release();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not re-publish MeshCoP as \"%s\": %s", name, esp_err_to_name(err));
    }
    return err;
}

esp_err_t thread_forget(void)
{
    otInstance *ins = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(ins)) {
        /* Recovery path for a failed Matter/RCP startup: erase only the
         * OpenThread namespace, leaving unrelated legacy NVS data alone. */
        nvs_handle_t h;
        esp_err_t err = nvs_open_from_partition("nvs", "openthread", NVS_READWRITE, &h);
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
        if (err == ESP_OK) {
            topology_reset(THREAD_TOPOLOGY_NEVER);
            ESP_LOGI(TAG, "offline Thread credentials erased");
        }
        return err;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
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

/*
 * otInstanceErasePersistentInfo() wipes every OpenThread setting, including
 * the Border Agent ID that identifies this border router to Home Assistant and
 * other commissioners. The ID stays cached in RAM, so the change only shows at
 * the next boot, when OpenThread generates a fresh random one. Write the
 * previous ID back after the wipe. otBorderAgentSetId() skips both the save and
 * the MeshCoP TXT refresh when the value equals the cached ID, so write a
 * one-bit-different ID first. Call with the OpenThread lock held.
 */
static void restore_border_agent_id(otInstance *ins, const otBorderAgentId *id)
{
    otBorderAgentId other = *id;
    other.mId[0] ^= 0x01;
    if (otBorderAgentSetId(ins, &other) != OT_ERROR_NONE ||
        otBorderAgentSetId(ins, id) != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "could not restore the Border Agent ID; a new one appears after reboot");
    }
}

/* Called with the OpenThread lock held. A detached stack is already enabled
 * and handles its own reattach attempts. Repeated imports must not interrupt
 * a parent search or detach an attached router and its children. */
static esp_err_t resume_active_dataset(otInstance *ins)
{
    if (otThreadGetDeviceRole(ins) != OT_DEVICE_ROLE_DISABLED) {
        return ESP_OK;
    }
    bool ip6_was_up = otIp6IsEnabled(ins);
    otError err = ip6_was_up ? OT_ERROR_NONE : otIp6SetEnabled(ins, true);
    if (err == OT_ERROR_NONE) {
        err = otThreadSetEnabled(ins, true);
        if (err != OT_ERROR_NONE && !ip6_was_up) {
            otIp6SetEnabled(ins, false);
        }
    }
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "could not resume active dataset (%d)", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t thread_join(const uint8_t *tlvs, size_t len)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "stack not running");
        return ESP_FAIL;
    }
    if (tlvs == NULL || len == 0 || len > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        ESP_LOGE(TAG, "invalid dataset buffer or length (%u)", (unsigned) len);
        return ESP_ERR_INVALID_ARG;
    }

    otOperationalDatasetTlvs ds;
    memset(&ds, 0, sizeof(ds));
    memcpy(ds.mTlvs, tlvs, len);
    ds.mLength = (uint8_t) len;

    if (!otDatasetIsValid(&ds, true)) {
        ESP_LOGE(TAG, "invalid active operational dataset");
        return ESP_ERR_INVALID_ARG;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();
    /* Keep an in-memory rollback copy. WIFI_STORAGE_FLASH has an equivalent
     * safeguard in app_wifi_join(); Thread replacement should be no easier to
     * strand than Wi-Fi replacement. */
    otOperationalDatasetTlvs previous;
    otError read_err = otDatasetGetActiveTlvs(ins, &previous);
    if (read_err != OT_ERROR_NONE && read_err != OT_ERROR_NOT_FOUND) {
        esp_openthread_lock_release();
        ESP_LOGE(TAG, "could not read current dataset (%d)", read_err);
        return ESP_FAIL;
    }
    bool had_previous = read_err == OT_ERROR_NONE && previous.mLength > 0;
    /* Compare all TLV types and values, including the timestamp and optional
     * TLVs. Their serialization order can differ across commissioners. */
    if (had_previous && otDatasetTlvsCompare(&ds, &previous)) {
        esp_err_t err = resume_active_dataset(ins);
        esp_openthread_lock_release();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "dataset unchanged; keeping the current network");
        }
        return err;
    }
    otMeshDiagCancel(ins);
    topology_reset(THREAD_TOPOLOGY_NEVER);
    otBorderAgentId ba_id;
    bool had_ba_id = otBorderAgentGetId(ins, &ba_id) == OT_ERROR_NONE;

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
    if (oterr == OT_ERROR_NONE && had_ba_id) {
        restore_border_agent_id(ins, &ba_id);
    }
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
            } else if (had_ba_id) {
                restore_border_agent_id(ins, &ba_id);
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

static bool activity_get(thread_activity_t *activity, TickType_t wait)
{
    if (!s_ready || activity == NULL) {
        return false;
    }

    memset(activity, 0, sizeof(*activity));
    if (!esp_openthread_lock_acquire(wait)) {
        return false;
    }
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

bool thread_activity_get(thread_activity_t *activity)
{
    return activity_get(activity, portMAX_DELAY);
}

bool thread_activity_get_with_timeout(thread_activity_t *activity, uint32_t timeout_ms)
{
    /* Management reads have a hard upper bound even if a future caller passes
     * a value that would overflow pdMS_TO_TICKS. */
    const uint32_t bounded_ms = timeout_ms > 1000 ? 1000 : timeout_ms;
    return activity_get(activity, pdMS_TO_TICKS(bounded_ms));
}

/* ---------------- sharing (ePSKc border-agent side) ---------------- */

/*
 * Advertise the ephemeral-key listener the way ot-br-posix does: the same
 * instance name and the same TXT record as _meshcop._udp, on the port
 * OpenThread opened for the key. Runs on the OpenThread task with the stack
 * lock held.
 */
static void share_state_cb(void *ctx)
{
    (void) ctx;
    otInstance *ins = esp_openthread_get_instance();
    otBorderAgentEphemeralKeyState st = otBorderAgentEphemeralKeyGetState(ins);

    bool active = st == OT_BORDER_AGENT_STATE_STARTED ||
                  st == OT_BORDER_AGENT_STATE_CONNECTED ||
                  st == OT_BORDER_AGENT_STATE_ACCEPTED;
    if (active && !s_meshcop_e_published) {
        uint16_t port = otBorderAgentEphemeralKeyGetUdpPort(ins);
        /* Copied: no OpenThread memory is read inside the mDNS section. */
        char nn[OT_NETWORK_NAME_MAX_SIZE + 1];
        const char *name = otThreadGetNetworkName(ins);
        snprintf(nn, sizeof(nn), "%s", name ? name : "");
        mdns_txt_item_t txt[] = {
            { "rv", "1" },
            { "tv", "1.4.0" },
            { "vn", MESHCOP_VENDOR_NAME },
            { "mn", THREAD_MESHCOP_MODEL_NAME },
            { "nn", nn },
        };
        mdns_section_begin();
        esp_err_t err = mdns_service_add(esp_openthread_get_meshcop_instance_name(),
                                         "_meshcop-e", "_udp", port, txt,
                                         sizeof(txt) / sizeof(txt[0]));
        mdns_section_end();
        s_meshcop_e_published = (err == ESP_OK);
        if (err == ESP_OK) {
            meshcop_txt_apply_locked(ins, "_meshcop-e");
        }
        ESP_LOGI(TAG, "ephemeral key listening on udp/%u (%s)", port,
                 err == ESP_OK ? "advertised" : "advertise FAILED");
    } else if ((st == OT_BORDER_AGENT_STATE_STOPPED || st == OT_BORDER_AGENT_STATE_DISABLED)
               && s_meshcop_e_published) {
        mdns_section_begin();
        mdns_service_remove("_meshcop-e", "_udp");
        mdns_section_end();
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
    if (hex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
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
