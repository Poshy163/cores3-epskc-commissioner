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

/* Apply a freshly retrieved dataset and attach to that network. */
esp_err_t thread_join(const uint8_t *dataset_tlvs, size_t len);

/* Wipe stored Thread credentials and detach. */
esp_err_t thread_forget(void);

/* True if an Active Operational Dataset is stored. */
bool thread_has_dataset(void);

/* "disabled" / "detached" / "child" / "router" / "leader", for the UI. */
const char *thread_role(void);

/* True once attached as child, router or leader. */
bool thread_attached(void);
