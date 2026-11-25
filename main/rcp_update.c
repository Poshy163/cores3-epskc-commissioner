#include "rcp_update.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_openthread_spinel.h"
#include "esp_rcp_update.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "rcp_update_policy.h"
#include "ui.h"

#define TAG "rcp"
#define RCP_PARTITION "rcp_fw"
#define RCP_MOUNT_POINT "/rcp_fw"
#define NVS_NAMESPACE "rcp_update"
#define KEY_FLASHED "flashed"
#define KEY_ATTEMPTS "attempts"
#define KEY_FORCED "forced"
#define KEY_RECOVERY "recovery"

/* If an NVS write fails inside Spinel's fatal callback, the SDK cannot resume
 * the current radio task. Carry recovery through its software restart anyway. */
#define RCP_WARM_RECOVERY 0x52435052u
static RTC_NOINIT_ATTR volatile uint32_t s_warm_recovery;
static bool s_recovery;

/* Read once from rcp_fw in rcp_update_prepare(), before anything else runs. */
static char s_bundled[RCP_UPDATE_VERSION_SIZE];

static void load_state(rcp_update_status_t *state)
{
    memset(state, 0, sizeof(*state));
    memcpy(state->bundled, s_bundled, sizeof(state->bundled));
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;   /* nothing stored yet */
    }
    size_t length = sizeof(state->flashed);
    if (nvs_get_str(handle, KEY_FLASHED, state->flashed, &length) != ESP_OK) {
        state->flashed[0] = '\0';
    }
    uint8_t value = 0;
    if (nvs_get_u8(handle, KEY_ATTEMPTS, &value) == ESP_OK) {
        state->attempts = value;
    }
    if (nvs_get_u8(handle, KEY_FORCED, &value) == ESP_OK) {
        state->forced = value != 0;
    }
    if (nvs_get_u8(handle, KEY_RECOVERY, &value) == ESP_OK) {
        state->recovery = value != 0;
    }
    nvs_close(handle);
}

/* NULL `flashed` leaves the stored record unchanged. */
static esp_err_t save_state(const char *flashed, uint8_t attempts, bool forced, bool recovery)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (flashed != NULL) {
        err = nvs_set_str(handle, KEY_FLASHED, flashed);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_ATTEMPTS, attempts);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_FORCED, forced ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_RECOVERY, recovery ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void read_bundled_version(void)
{
    s_bundled[0] = '\0';
    esp_vfs_spiffs_conf_t conf = {
        .base_path = RCP_MOUNT_POINT,
        .partition_label = RCP_PARTITION,
        .max_files = 2,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no radio firmware image (%s): %s", RCP_PARTITION, esp_err_to_name(err));
        return;
    }
    err = esp_rcp_load_version_in_storage(s_bundled, sizeof(s_bundled));
    if (err != ESP_OK) {
        s_bundled[0] = '\0';
        ESP_LOGW(TAG, "no radio firmware version in %s: %s", RCP_PARTITION, esp_err_to_name(err));
    }
    s_bundled[sizeof(s_bundled) - 1] = '\0';
}

static void flash_bundled(const rcp_update_status_t *state)
{
    const uint8_t attempt = state->attempts + 1;
    /* Count the attempt first: esp_rcp_update() aborts after its own retries,
     * and a crash mid-flash must still use up this attempt. */
    esp_err_t err = save_state(NULL, attempt, state->forced, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not record update attempt (%s); not flashing", esp_err_to_name(err));
        s_recovery = true;
        return;
    }
    ESP_LOGW(TAG, "flashing radio firmware %s (attempt %u/%u)", s_bundled,
             (unsigned) attempt, (unsigned) RCP_UPDATE_MAX_ATTEMPTS);
    ui_set_status(UI_STATE_BUSY, "Updating radio firmware");

    const int64_t started = esp_timer_get_time();
    err = esp_rcp_update();
    const unsigned elapsed_ms = (unsigned) ((esp_timer_get_time() - started) / 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radio firmware update failed after %u ms: %s; restarting",
                 elapsed_ms, esp_err_to_name(err));
    } else {
        esp_rcp_mark_image_verified(true);
        /* The flash wrote and verified the bytes. Only rcp_update_check_running()
         * seeing the H2 report this version clears the attempt count. */
        err = save_state(s_bundled, attempt, false, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not record the flashed version: %s", esp_err_to_name(err));
        }
        ESP_LOGW(TAG, "radio firmware written in %u ms; restarting", elapsed_ms);
    }
    /* Restart either way, as Espressif's reference updater does. The flasher
     * leaves UART1, the H2 TX pin's drive strength and the BOOT pin set up for
     * the ROM loader, and this boot now runs about 12 s behind its usual
     * order. The next boot starts clean and either verifies the new firmware
     * or spends another attempt. */
    esp_restart();
}

/* OpenThread calls these when the RCP is incompatible or does not come back
 * from a reset. Unregistered, the stack asserts and the S3 reboots into the
 * same failure; here the next boot reflashes, bounded by the attempt count. */
static void rcp_unusable(const char *why)
{
    rcp_update_status_t state;
    load_state(&state);
    if (rcp_update_has_version(s_bundled) && state.attempts < RCP_UPDATE_MAX_ATTEMPTS &&
        save_state(NULL, state.attempts, true, false) == ESP_OK) {
        ESP_LOGE(TAG, "radio co-processor %s; reflashing on restart", why);
        esp_restart();
    }
    s_warm_recovery = RCP_WARM_RECOVERY;
    esp_err_t err = save_state(NULL, state.attempts, false, true);
    ESP_LOGE(TAG, "radio co-processor %s; restarting in recovery (NVS: %s)",
             why, esp_err_to_name(err));
    esp_restart();
}

static void rcp_incompatible(void)
{
    rcp_unusable("is incompatible");
}

static void rcp_reset_failed(void)
{
    rcp_unusable("did not come back from reset");
}

esp_err_t rcp_update_prepare(const rcp_update_pins_t *pins)
{
    if (pins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rcp_update_status_t state;
    load_state(&state);
    s_recovery = state.recovery ||
                 (esp_reset_reason() == ESP_RST_SW && s_warm_recovery == RCP_WARM_RECOVERY);
    if (!s_recovery) {
        s_warm_recovery = 0;
    }
    esp_openthread_set_compatibility_error_callback(rcp_incompatible);
    esp_openthread_set_coprocessor_reset_failure_callback(rcp_reset_failed);
    esp_rcp_update_config_t config = {
        .rcp_type = RCP_TYPE_UART,
        .uart_rx_pin = pins->rx_pin,
        .uart_tx_pin = pins->tx_pin,
        .uart_port = pins->uart,
        .uart_baudrate = 115200,          /* the H2 ROM loader's baud rate */
        .reset_pin = pins->reset_pin,
        .boot_pin = pins->boot_pin,
        .update_baudrate = 460800,
        .firmware_dir = RCP_MOUNT_POINT "/ot_rcp",
        .target_chip = ESP32H2_CHIP,
    };
    esp_err_t err = esp_rcp_update_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radio firmware updater unavailable: %s", esp_err_to_name(err));
        return err;
    }
    read_bundled_version();
    if (s_recovery) {
        ESP_LOGW(TAG, "radio recovery active; use the USB console's rcp update command to retry");
        esp_vfs_spiffs_unregister(RCP_PARTITION);
        return ESP_OK;
    }
    switch (rcp_update_decide(s_bundled, state.flashed, state.forced, state.attempts)) {
    case RCP_UPDATE_SKIP_NO_IMAGE:
        break;
    case RCP_UPDATE_SKIP_CURRENT:
        ESP_LOGI(TAG, "bundled radio firmware already flashed: %s", s_bundled);
        break;
    case RCP_UPDATE_FLASH:
        flash_bundled(&state);
        break;
    case RCP_UPDATE_GIVE_UP:
        ESP_LOGE(TAG, "radio firmware %s not verified after %u attempts; run `rcp update` to retry",
                 s_bundled, (unsigned) state.attempts);
        break;
    }
    /* The version is cached; the image is only read again on a later boot. */
    esp_vfs_spiffs_unregister(RCP_PARTITION);
    return ESP_OK;
}

bool rcp_update_in_recovery(void)
{
    return s_recovery;
}

void rcp_update_check_running(const char *running_version)
{
    if (!rcp_update_has_version(s_bundled) || !rcp_update_has_version(running_version)) {
        return;
    }
    rcp_update_status_t state;
    load_state(&state);
    if (!rcp_update_running_differs(s_bundled, running_version)) {
        if (state.attempts != 0 || state.forced || strcmp(state.flashed, s_bundled) != 0) {
            esp_err_t err = save_state(s_bundled, 0, false, false);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "could not record the verified radio firmware: %s", esp_err_to_name(err));
            }
        }
        ESP_LOGI(TAG, "radio firmware verified: %s", running_version);
        return;
    }
    if (state.attempts >= RCP_UPDATE_MAX_ATTEMPTS) {
        ESP_LOGE(TAG, "radio runs %s, not the bundled %s; no automatic attempts left",
                 running_version, s_bundled);
        return;
    }
    if (save_state(NULL, state.attempts, true, false) != ESP_OK) {
        ESP_LOGE(TAG, "radio runs %s, not the bundled %s; could not schedule a reflash",
                 running_version, s_bundled);
        return;
    }
    ESP_LOGW(TAG, "radio runs %s, not the bundled %s; reflashing on restart",
             running_version, s_bundled);
    esp_restart();
}

esp_err_t rcp_update_get_status(rcp_update_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    load_state(out);
    out->recovery = s_recovery;
    return ESP_OK;
}

esp_err_t rcp_update_request(void)
{
    bool reflash = rcp_update_has_version(s_bundled);
    if (!reflash && !s_recovery) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = save_state(NULL, 0, reflash, false);
    if (err == ESP_OK) {
        s_warm_recovery = 0;
    }
    return err;
}
