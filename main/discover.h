#pragma once

#include <stdint.h>

#include "esp_err.h"

#define BA_NAME_LEN 48
#define BA_MAX      8

typedef struct {
    char name[BA_NAME_LEN];  /* instance name from the PTR record */
    char ip[16];             /* IPv4 */
    uint16_t port;
} ba_entry_t;

esp_err_t discover_init(void);

/*
 * Browse _meshcop-e._udp -- border agents only advertise this while an
 * ephemeral key is active. Returns the number of entries filled.
 */
int discover_border_agents(ba_entry_t *out, int max, uint32_t timeout_ms);
