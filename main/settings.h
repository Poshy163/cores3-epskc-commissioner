/*
 * User settings, persisted in NVS namespace "cfg" (which "Forget network"
 * leaves alone; only Factory reset clears it).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t brightness;       /* 10..100 */
    uint8_t sleep_idx;        /* index into settings_sleep_ms[] */
    bool    prefer_router;    /* ask to be promoted rather than stay a child */
    uint8_t new_net_channel;  /* 0 = let OpenThread choose, else 11..26 */
    uint8_t share_minutes;    /* ephemeral key lifetime: 2, 5 or 10 */
    bool    keep_awake_powered;  /* never sleep while USB power is present */
    bool    rest_api;            /* serve the OTBR REST API on port 8081 */
    bool    rest_epskc;          /* also serve the ba-epskc endpoints (PR #267) */
} settings_t;

/* Sleep-timeout choices, in ms; 0 means never. Matches the on-screen roller. */
extern const uint32_t settings_sleep_ms[4];
#define SETTINGS_SLEEP_OPTIONS "30 s\n1 min\n5 min\nNever"
#define SETTINGS_DEFAULT_DEVICE_NAME "cores3-thread-br"
#define SETTINGS_DEVICE_NAME_MAX_LEN 32
#define SETTINGS_DEVICE_NAME_BUFFER_SIZE (SETTINGS_DEVICE_NAME_MAX_LEN + 1)

void settings_load(void);
void settings_save(void);
settings_t *settings_get(void);

/* The single user-facing name shared by Matter TBR Management and MeshCoP. */
esp_err_t settings_get_device_name(char *name, size_t name_size);
esp_err_t settings_set_device_name(const char *name);

/* Wipe this namespace. Used by Factory reset. */
esp_err_t settings_erase(void);

#ifdef __cplusplus
}
#endif
