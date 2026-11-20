#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Authenticated management snapshot. It never reads stored credentials,
 * onboarding codes or an active ephemeral key. Power comes from the cached
 * history rather than new I2C reads on the HTTP task. */
#define DEVICE_STATUS_JSON_SIZE 4096
size_t device_status_json(char *out, size_t capacity, bool reboot_pending);
