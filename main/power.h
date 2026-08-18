/*
 * Battery state from the CoreS3's AXP2101 PMIC fuel gauge.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int  percent;    /* 0-100, -1 if no battery / gauge unreadable */
    int  batt_mv;    /* VBAT rail in mV, -1 if unreadable; valid even when
                      * the present bit is clear, which is what tells a truly
                      * absent battery from a mis-detected one */
    bool present;    /* battery detected by the PMIC */
    bool charging;
    bool full;       /* charger reports charge-done */
    bool vbus;       /* 5V input (USB) present */
    uint8_t raw_st1; /* reg 0x00, for diagnosing detection issues */
    uint8_t raw_st2; /* reg 0x01 */
} power_status_t;

/* Safe to call from any task; the I2C driver serialises bus access. */
esp_err_t power_read(power_status_t *out);
