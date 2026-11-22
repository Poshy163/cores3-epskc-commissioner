#pragma once

#include "esp_matter.h"

/*
 * Temperature Sensor endpoint carrying the ESP32-S3's die temperature, so
 * Home Assistant shows it as the border router's temperature. Call create
 * before Matter start, then start after endpoint registration.
 */
esp_err_t matter_temperature_create(esp_matter::node_t *node);
esp_err_t matter_temperature_start(void);
uint16_t matter_temperature_endpoint(void);
