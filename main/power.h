/*
 * Battery and power state from the CoreS3's AXP2101 PMIC, plus the S3's own
 * die temperature.
 *
 * What the AXP2101 cannot do: measure current. Its ADC has VBAT, VBUS, VSYS,
 * a thermistor input and die temperature -- no battery-current channel (the
 * older AXP192 had one). So there is no live mW figure; the closest honest
 * substitute is the discharge rate derived from the gauge over time, below.
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
    int  vbus_mv;    /* USB input voltage, -1 if unreadable */
    int  vsys_mv;    /* system rail, -1 if unreadable */
    int  pmic_temp_c;/* PMIC die temperature, INT32_MIN if unreadable */
    bool present;    /* battery detected by the PMIC */
    bool percent_estimated; /* percent came from the cell voltage because the
                             * gauge restarted (battery switched out and back
                             * in) and still reports 0 % */
    bool charging;
    bool full;       /* charger reports charge-done */
    bool vbus;       /* 5V input (USB) present */
    const char *charge_detail;  /* "pre-charge", "constant current", ... */
    uint8_t raw_st1; /* reg 0x00, for diagnosing detection issues */
    uint8_t raw_st2; /* reg 0x01 */
} power_status_t;

/* Safe to call from any task; the I2C driver serialises bus access. */
esp_err_t power_read(power_status_t *out);

/* ESP32-S3 internal temperature sensor. False if it could not be installed. */
bool power_esp_temp(float *celsius);

/*
 * Discharge-rate tracking. Feed one sample roughly every minute; pass
 * on_battery=false (USB present) to reset the history, since charging makes
 * the numbers meaningless. power_discharge_rate() reports false until there
 * is at least ten minutes of battery-only history and a measurable drop.
 */
void power_note_sample(int percent, bool on_battery);
bool power_discharge_rate(float *pct_per_hour, int *minutes_left);
