#pragma once

/*
 * When to flash the ESP32-H2 radio co-processor with the image bundled in the
 * rcp_fw partition. Header-only so tools/test_rcp_update_policy.c can exercise
 * it on the host.
 *
 * `attempts` counts flashes since the last boot that proved the H2 runs the
 * bundled image. It is cleared only by that proof, not by a flash that merely
 * wrote and verified the bytes: an image that flashes cleanly but does not
 * talk must not be reflashed forever.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RCP_UPDATE_MAX_ATTEMPTS 3

typedef enum {
    RCP_UPDATE_SKIP_NO_IMAGE,   /* rcp_fw holds no usable image */
    RCP_UPDATE_SKIP_CURRENT,    /* the H2 was last flashed with this image */
    RCP_UPDATE_FLASH,           /* flash before OpenThread opens the UART */
    RCP_UPDATE_GIVE_UP,         /* automatic attempts used up; `rcp update` retries */
} rcp_update_action_t;

static inline bool rcp_update_has_version(const char *version)
{
    return version != NULL && version[0] != '\0';
}

/* Before OpenThread starts. `flashed` is the bundled version this firmware last
 * wrote to the H2 (NULL or empty when unknown); `forced` asks for a reflash
 * even when it matches. */
static inline rcp_update_action_t rcp_update_decide(const char *bundled, const char *flashed,
                                                    bool forced, uint8_t attempts)
{
    if (!rcp_update_has_version(bundled)) {
        return RCP_UPDATE_SKIP_NO_IMAGE;
    }
    if (!forced && rcp_update_has_version(flashed) && strcmp(bundled, flashed) == 0) {
        return RCP_UPDATE_SKIP_CURRENT;
    }
    if (attempts >= RCP_UPDATE_MAX_ATTEMPTS) {
        return RCP_UPDATE_GIVE_UP;
    }
    return RCP_UPDATE_FLASH;
}

/* After OpenThread starts: true when the H2 reports a version and it differs
 * from the bundled one, so the next boot should reflash. Unknown versions on
 * either side never ask for a reflash. */
static inline bool rcp_update_running_differs(const char *bundled, const char *running)
{
    return rcp_update_has_version(bundled) && rcp_update_has_version(running) &&
           strcmp(bundled, running) != 0;
}
