#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 * Start the Matter 1.5.1 Thread Border Router node.
 *
 * Wi-Fi and the default event loop may already be running. ESP-Matter adopts
 * them, while OpenThread is initialized and tasked by ESP-Matter itself.
 */
esp_err_t matter_init(void);

matter_runtime_status_t matter_get_status(void);
const char *matter_status_string(matter_runtime_status_t status);
esp_err_t matter_get_last_error(void);
bool matter_is_commissioned(void);
bool matter_is_commissioning_window_open(void);
uint16_t matter_get_thread_border_router_endpoint_id(void);

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
 * Return the device's QR payload and manual setup code.
 *
 * Codes are available while the device is uncommissioned, or while an
 * explicitly opened commissioning window is active. Both output buffers are
 * always NUL-terminated on success. Use the buffer-size constants above.
 */
esp_err_t matter_get_onboarding_codes(char *qr_code, size_t qr_code_size,
                                      char *manual_code, size_t manual_code_size);

#ifdef __cplusplus
}
#endif
