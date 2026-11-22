#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "power.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Map an ESP log level onto CHIP's own category filter. CHIP's ESP32 logger
 * prints the "I (t) chip[XX]:" prefix with a bare printf before handing the
 * message to esp_log, so the per-tag ESP filter alone leaves empty prefix
 * lines; this gates the category at the source. Safe before matter_init().
 */
void matter_set_log_verbosity(esp_log_level_t level);

/* Includes room for the terminating NUL. */
#define MATTER_QR_CODE_BUFFER_SIZE       129
#define MATTER_MANUAL_CODE_BUFFER_SIZE    32

typedef enum {
    MATTER_RUNTIME_STOPPED = 0,
    MATTER_RUNTIME_STARTING,
    MATTER_RUNTIME_COMMISSIONABLE,
    MATTER_RUNTIME_COMMISSIONED,
    MATTER_RUNTIME_ERROR,
} matter_runtime_status_t;

/**
 * Start the ESP-Matter 1.6.1 Thread Border Router node.
 *
 * Wi-Fi and the default event loop may already be running. ESP-Matter adopts
 * them, while OpenThread is initialized and tasked by ESP-Matter itself.
 */
esp_err_t matter_init(void);

/* Nonblocking handoff of the existing PMIC sample; safe before Matter start. */
void matter_note_power(const power_status_t *sample, esp_err_t error);

matter_runtime_status_t matter_get_status(void);
const char *matter_status_string(matter_runtime_status_t status);
esp_err_t matter_get_last_error(void);
bool matter_is_commissioned(void);
bool matter_is_commissioning_window_open(void);
uint16_t matter_get_thread_border_router_endpoint_id(void);

typedef enum {
    MATTER_WINDOW_CLOSED = 0,
    MATTER_WINDOW_BASIC,     /* accepts this device's own setup code */
    MATTER_WINDOW_ENHANCED,  /* opened by a controller, with its own one-time code */
} matter_window_t;

matter_window_t matter_get_commissioning_window(void);

/* Seconds left in a window this firmware opened, or -1 when unknown: a window
 * a controller opened, or the initial one of an uncommissioned device. */
int matter_get_commissioning_window_seconds_left(void);

/* Controllers (fabrics) this node is commissioned into. */
unsigned matter_get_fabric_count(void);

/* Ask the CHIP event loop to close whichever commissioning window is open.
 * Asynchronous like matter_open_commissioning_window(); ESP_OK if none is. */
esp_err_t matter_close_commissioning_window(void);

/* Whether Bluetooth pairing (Settings > Matter) was on when Matter started.
 * The setting is read only then, so a change applies after a restart. Even
 * when on, Bluetooth serves only a first pairing: ESP-Matter shuts it down
 * once the device belongs to a controller. */
bool matter_ble_commissioning_enabled(void);

/**
 * Ask the CHIP event loop to open a basic commissioning window.
 *
 * The request is asynchronous. ESP_OK means it was accepted; poll
 * matter_is_commissioning_window_open() for the resulting state. The standard
 * basic-window range supported here is 180..900 seconds.
 */
esp_err_t matter_open_commissioning_window(uint16_t timeout_seconds);

/**
 * Erase Matter attributes and fabrics, then reboot.
 *
 * The reset and reboot are scheduled on the CHIP event loop. A successful
 * return means the request was accepted; the function itself does not wait for
 * the reboot. Once Matter is running, the upstream reset always schedules the
 * full CHIP cleanup and reboot even if its preliminary namespace cleanup logs
 * a warning. The application's separate factory-reset path remains
 * responsible for any non-Matter settings it also wants to erase.
 */
esp_err_t matter_factory_reset(void);

/**
 * Apply a new border-router name live: the TBR Management BorderRouterName
 * attribute and the root node's NodeLabel. ESP_ERR_INVALID_STATE before the
 * stack has started; matter_init() reads the saved name itself.
 */
esp_err_t matter_set_border_router_name(const char *name);

/**
 * Return the device's QR payload and manual setup code.
 *
 * Codes are available while the device is uncommissioned, or while a basic
 * commissioning window is open. An enhanced window, opened by a controller,
 * accepts only that controller's code, so this returns ESP_ERR_INVALID_STATE.
 * Both output buffers are always NUL-terminated on success. Use the
 * buffer-size constants above.
 */
esp_err_t matter_get_onboarding_codes(char *qr_code, size_t qr_code_size,
                                      char *manual_code, size_t manual_code_size);

#ifdef __cplusplus
}
#endif
