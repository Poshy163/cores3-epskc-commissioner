#include <assert.h>
#include <stdio.h>
#include "../main/matter_battery_model.h"

int main(void)
{
    matter_battery_sample_t s = { .at_ms = 1000, .percent = 100,
        .millivolts = 4100, .valid = true, .present = true };
    const int percentages[] = {0, 1, 5, 14, 15, 50, 99, 100};
    for (unsigned i = 0; i < sizeof(percentages)/sizeof(percentages[0]); ++i) {
        s.percent = percentages[i];
        matter_battery_values_t v = matter_battery_convert(&s, 1000);
        assert(v.percent_half == percentages[i] * 2);
        assert(v.status == 1 && v.charge == 3 && v.millivolts == 4100);
        assert(v.level == (s.percent <= 5 ? 2 : s.percent < 15 ? 1 : 0));
    }
    s.estimated = true;
    assert(matter_battery_convert(&s, 1000).percent_half == -1);
    assert(matter_battery_convert(&s, 1000).millivolts == 4100);
    s.estimated = false;
    s.percent = -1;
    assert(matter_battery_convert(&s, 1000).percent_half == -1);
    s.percent = 101;
    assert(matter_battery_convert(&s, 1000).percent_half == -1);
    s.percent = 50;
    s.millivolts = -1;
    assert(matter_battery_convert(&s, 1000).millivolts == -1);
    assert(matter_battery_convert(&s, 1000).percent_half == 100);
    s.millivolts = 4351;
    assert(matter_battery_convert(&s, 1000).millivolts == -1);
    s.millivolts = 4100;
    s.usb = true;
    s.charging = true;
    assert(matter_battery_convert(&s, 1000).charge == 1);
    assert(matter_battery_convert(&s, 1000).status == 2);
    s.full = true;
    assert(matter_battery_convert(&s, 1000).charge == 2);
    s.usb = false; /* stale charger flags cannot imply charging unplugged */
    assert(matter_battery_convert(&s, 1000).charge == 3);
    s.present = false;
    matter_battery_values_t v = matter_battery_convert(&s, 1000);
    assert(v.status == 3 && v.charge == 0 && v.percent_half == -1 && v.millivolts == -1);
    s.present = true;
    assert(matter_battery_convert(&s, 60999).percent_half == 100);
    v = matter_battery_convert(&s, 61000);
    assert(v.status == 0 && v.charge == 0 && v.percent_half == -1 && v.millivolts == -1);
    assert(matter_battery_convert(&s, 999).percent_half == -1);
    s.valid = false;
    assert(matter_battery_convert(&s, 1000).percent_half == -1);
    matter_battery_sample_t startup = {0};
    assert(matter_battery_convert(&startup, 0).percent_half == -1);

    assert(matter_battery_voltage_due(-2, 4100, 0, 1000));
    assert(!matter_battery_voltage_due(4100, 4090, 1000, 60999));
    assert(matter_battery_voltage_due(4100, 4090, 1000, 61000));
    assert(!matter_battery_voltage_due(4100, 4100, 1000, 999999));
    assert(matter_battery_voltage_due(4100, -1, 1000, 1001));
    assert(matter_battery_voltage_due(-1, 4090, 1000, 1001));
    puts("PASS Matter battery conversion, freshness and voltage reporting");
}
