#pragma once

/*
 * Keeps the ESP32-H2 radio co-processor on the firmware bundled in this
 * image's rcp_fw partition (built from rcp/). The H2 is flashed over the same
 * UART OpenThread uses, with the S3 driving its BOOT and RESET straps, before
 * OpenThread opens that UART.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RCP_UPDATE_VERSION_SIZE 100

typedef struct {
    uart_port_t uart;
    gpio_num_t rx_pin;      /* S3 pin receiving from the H2 */
    gpio_num_t tx_pin;      /* S3 pin transmitting to the H2 */
    gpio_num_t reset_pin;
    gpio_num_t boot_pin;
} rcp_update_pins_t;

typedef struct {
    char bundled[RCP_UPDATE_VERSION_SIZE];  /* empty without an rcp_fw image */
    char flashed[RCP_UPDATE_VERSION_SIZE];  /* bundled version last written */
    uint8_t attempts;                       /* flashes since the last verified boot */
    bool forced;                            /* reflash requested for the next boot */
} rcp_update_status_t;

/* Call once, after the default NVS partition is up and before OpenThread
 * starts. Registers OpenThread's RCP-unusable callbacks and flashes the H2 if
 * the bundled image is new, requested, or never verified. Never fatal: on any
 * error the H2 keeps its current firmware. */
esp_err_t rcp_update_prepare(const rcp_update_pins_t *pins);

/* Call once OpenThread reports the running RCP version. A match proves the
 * bundled image works and clears the attempt count; a mismatch schedules a
 * reflash and restarts while automatic attempts remain. */
void rcp_update_check_running(const char *running_version);

esp_err_t rcp_update_get_status(rcp_update_status_t *out);

/* Request a reflash on the next boot with a fresh attempt budget. The caller
 * restarts the device. */
esp_err_t rcp_update_request(void);

#ifdef __cplusplus
}
#endif
