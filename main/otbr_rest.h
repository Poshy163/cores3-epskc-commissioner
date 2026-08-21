/*
 * Minimal OpenThread Border Router REST API, the subset Home Assistant's
 * `otbr` integration (python-otbr-api) actually calls.
 *
 * Point HA at http://<device-ip>:8081. Served plain HTTP with no
 * authentication, exactly like ot-br-posix: anyone who can reach the port can
 * read the Active Operational Dataset, which contains the network key. Keep it
 * on a trusted network.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define OTBR_REST_PORT 8081

/* Idempotent; safe to call again on Wi-Fi reconnect. */
esp_err_t otbr_rest_start(void);

/*
 * Add or remove the ba-epskc routes at runtime. Removing them is not
 * the same as answering 404 with a body: an unregistered route is
 * indistinguishable from a build that never had the endpoints, which is what
 * a client uses to decide the router has no ePSKc support.
 */
void otbr_rest_set_epskc(bool on);
void otbr_rest_stop(void);
bool otbr_rest_running(void);
