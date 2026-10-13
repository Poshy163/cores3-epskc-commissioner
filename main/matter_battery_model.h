#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Scalar snapshot: no PMIC pointers or SDK objects cross the task boundary. */
typedef struct {
    uint64_t at_ms;
    int percent;
    int millivolts;
    bool valid, present, estimated, usb, charging, full;
} matter_battery_sample_t;

typedef struct {
    int percent_half; /* -1 means Matter null */
    int millivolts;   /* -1 means Matter null */
    int status;      /* PowerSourceStatusEnum */
    int charge;      /* BatChargeStateEnum */
    int level;       /* BatChargeLevelEnum; -1 retains last known level */
} matter_battery_values_t;

static inline matter_battery_values_t matter_battery_convert(
    const matter_battery_sample_t *s, uint64_t now_ms)
{
    matter_battery_values_t v = { -1, -1, 0, 0, -1 };
    if (!s->valid || now_ms < s->at_ms || now_ms - s->at_ms >= 60000) {
        return v;
    }
    if (!s->present) {
        v.status = 3; /* unavailable */
        return v;
    }
    v.status = s->usb ? 2 : 1; /* standby on USB, active on battery */
    v.charge = !s->usb ? 3 : s->full ? 2 : s->charging ? 1 : 3;
    if (!s->estimated && s->percent >= 0 && s->percent <= 100) {
        v.percent_half = s->percent * 2;
        v.level = s->percent <= 5 ? 2 : s->percent < 15 ? 1 : 0;
    }
    if (s->millivolts >= 2500 && s->millivolts <= 4350) {
        v.millivolts = s->millivolts;
    }
    return v;
}

static inline bool matter_battery_voltage_due(int previous, int next,
                                             uint64_t last_ms, uint64_t now_ms)
{
    return previous != next &&
        (previous < 0 || next < 0 || now_ms - last_ms >= 60000);
}
