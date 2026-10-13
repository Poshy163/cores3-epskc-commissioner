#pragma once

#include "esp_matter.h"

/* Call create before Matter start, then start after endpoint registration. */
esp_err_t matter_battery_create(esp_matter::node_t *node);
esp_err_t matter_battery_start(uint16_t tbr_endpoint);
