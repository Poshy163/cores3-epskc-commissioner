#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "rcp_update.h"
#include "rcp_update_policy.h"
#include "ui.h"

static jmp_buf reset;
static int reset_reason = ESP_RST_POWERON;
static int flashes;
static bool image_present = true;
static bool writes_fail;
static const char *image = "bundled H2";
static uint8_t attempts, forced, recovery;
static char flashed[RCP_UPDATE_VERSION_SIZE];
static void (*reset_failed)(void), (*incompatible)(void);

void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test"; }
int64_t esp_timer_get_time(void) { return 0; }
void ui_set_status(ui_state_t state, const char *text) { (void)state; (void)text; }
int esp_reset_reason(void) { return reset_reason; }
_Noreturn void esp_restart(void) { reset_reason = ESP_RST_SW; longjmp(reset, 1); }
void esp_openthread_set_compatibility_error_callback(void (*cb)(void)) { incompatible = cb; }
void esp_openthread_set_coprocessor_reset_failure_callback(void (*cb)(void)) { reset_failed = cb; }
esp_err_t esp_rcp_update_init(const esp_rcp_update_config_t *config) { (void)config; return ESP_OK; }
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t esp_vfs_spiffs_unregister(const char *label) { (void)label; return ESP_OK; }
esp_err_t esp_rcp_update(void) { ++flashes; return ESP_OK; }
void esp_rcp_mark_image_verified(bool verified) { assert(verified); }
esp_err_t esp_rcp_load_version_in_storage(char *out, size_t size)
{
    if (!image_present) return ESP_FAIL;
    assert(strlen(image) < size);
    strcpy(out, image);
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle)
{
    assert(strcmp(ns, "rcp_update") == 0);
    *handle = 1;
    return mode == NVS_READWRITE && writes_fail ? ESP_FAIL : ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *size)
{
    (void)h;
    assert(strcmp(key, "flashed") == 0 && strlen(flashed) < *size);
    if (!flashed[0]) return ESP_ERR_NVS_NOT_FOUND;
    strcpy(out, flashed);
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    (void)h;
    assert(strcmp(key, "flashed") == 0 && strlen(value) < sizeof(flashed));
    strcpy(flashed, value);
    return ESP_OK;
}
static uint8_t *field(const char *key)
{
    if (strcmp(key, "attempts") == 0) return &attempts;
    if (strcmp(key, "forced") == 0) return &forced;
    assert(strcmp(key, "recovery") == 0);
    return &recovery;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    (void)h; *out = *field(key); return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    (void)h; *field(key) = value; return ESP_OK;
}

/* A normal return means the caller reached the part of boot where the
 * recovery console and Wi-Fi diagnostics can start. A flash restarts. */
static bool boot(void)
{
    if (setjmp(reset) != 0) return false;
    const rcp_update_pins_t pins = {0};
    assert(rcp_update_prepare(&pins) == ESP_OK);
    return true;
}

static void fail_radio(void (*notify)(void))
{
    if (setjmp(reset) == 0) {
        notify();
        assert(!"fatal radio callback unexpectedly returned");
    }
}

int main(void)
{
    /* Each flash is followed by one Spinel probe, even the last flash. */
    for (unsigned attempt = 1; attempt <= RCP_UPDATE_MAX_ATTEMPTS; ++attempt) {
        assert(!boot());
        assert(attempts == attempt && flashes == (int)attempt);
        assert(boot() && !rcp_update_in_recovery());
        fail_radio(attempt == 1 ? incompatible : reset_failed);
    }
    assert(boot() && rcp_update_in_recovery());
    assert(flashes == RCP_UPDATE_MAX_ATTEMPTS && recovery);
    reset_reason = ESP_RST_POWERON;
    assert(boot() && rcp_update_in_recovery());
    rcp_update_status_t status;
    assert(rcp_update_get_status(&status) == ESP_OK && status.recovery);

    /* Manual recovery flashes once, then accepts and records a healthy H2. */
    assert(rcp_update_request() == ESP_OK && !recovery && attempts == 0);
    assert(!boot() && flashes == RCP_UPDATE_MAX_ATTEMPTS + 1);
    assert(boot() && !rcp_update_in_recovery());
    rcp_update_check_running(image);
    assert(attempts == 0 && !forced);
    assert(boot() && flashes == RCP_UPDATE_MAX_ATTEMPTS + 1);

    /* Existing app-only installations must still start a working stock H2.
     * An unusable stock H2 enters recovery without a flash. */
    image_present = false;
    assert(boot() && !rcp_update_in_recovery());
    assert(rcp_update_request() == ESP_ERR_NOT_FOUND);
    fail_radio(incompatible);
    assert(boot() && rcp_update_in_recovery());
    assert(rcp_update_request() == ESP_OK && !forced);
    assert(boot() && !rcp_update_in_recovery() && flashes == RCP_UPDATE_MAX_ATTEMPTS + 1);

    /* Storage faults must not reboot an incompatible H2 indefinitely. */
    image_present = true;
    assert(boot() && !rcp_update_in_recovery());
    writes_fail = true;
    fail_radio(reset_failed);
    assert(!recovery && boot() && rcp_update_in_recovery());
    assert(rcp_update_request() == ESP_FAIL);
    assert(boot() && rcp_update_in_recovery());
    writes_fail = false;
    assert(rcp_update_request() == ESP_OK);
    assert(!boot() && boot() && !rcp_update_in_recovery());
    return 0;
}
