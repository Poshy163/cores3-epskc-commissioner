"""Exercise the production handoff/publisher with a deterministic CHIP queue."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include "matter_battery_model.h"
#include "power.h"
using esp_timer_handle_t = void *;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define ESP_LOGW(...) ((void)0)
using CHIP_ERROR = int;
constexpr int CHIP_NO_ERROR = 0;
int64_t test_us = 1000000;
int64_t esp_timer_get_time() { return test_us; }
template<class T> struct nullable {
    int64_t value = -1;
    nullable() = default;
    nullable(T x): value(x) {}
};
struct esp_matter_attr_val_t { int64_t value; };
esp_matter_attr_val_t esp_matter_nullable_uint8(nullable<uint8_t> v) { return {v.value}; }
esp_matter_attr_val_t esp_matter_nullable_uint32(nullable<uint32_t> v) { return {v.value}; }
esp_matter_attr_val_t esp_matter_enum8(int v) { return {v}; }
std::map<uint32_t,int64_t> values;
int fail_attribute = -1, writes = 0;
namespace esp_matter::attribute {
int update(uint16_t, uint32_t, uint32_t id, esp_matter_attr_val_t *v) {
    if ((int)id == fail_attribute) return -1;
    values[id] = v->value; ++writes; return 0;
}
}
namespace chip::app::Clusters::PowerSource {
constexpr uint32_t Id = 47;
enum class PowerSourceStatusEnum { kStandby = 2, kUnavailable = 3 };
enum class BatChargeStateEnum { kIsNotCharging = 3, kIsAtFullCharge = 2 };
enum class BatChargeLevelEnum { kCritical = 2 };
namespace Attributes {
struct BatPercentRemaining { static constexpr uint32_t Id = 12; };
struct BatVoltage { static constexpr uint32_t Id = 11; };
struct Status { static constexpr uint32_t Id = 0; };
struct BatChargeState { static constexpr uint32_t Id = 26; };
struct BatChargeLevel { static constexpr uint32_t Id = 14; };
}
}
bool fail_schedule = false;
std::deque<void(*)(intptr_t)> jobs;
namespace chip::DeviceLayer {
struct Platform {
    int ScheduleWork(void(*fn)(intptr_t), intptr_t) {
        if (fail_schedule) return -1;
        jobs.push_back(fn); return 0;
    }
};
Platform &PlatformMgr() { static Platform p; return p; }
}
void drain() { while (!jobs.empty()) { auto fn=jobs.front(); jobs.pop_front(); fn(0); } }
'''

TEST = r'''
int main() {
    power_status_t sample{};
    sample.present=true; sample.percent=80; sample.batt_mv=4000;
    matter_note_power(&sample, ESP_OK);
    assert(jobs.empty()); // retained before startup
    sample.percent=79; matter_note_power(&sample, ESP_OK);
    s_ready=true; request_publish(nullptr); drain();
    assert(values[12]==158 && values[11]==4000);
    int before=writes;
    request_publish(nullptr); drain(); assert(writes==before);
    // Burst coalesces to one queued job and publishes only latest snapshot.
    for(int i=0;i<100;++i) { sample.percent=i; matter_note_power(&sample, ESP_OK); }
    assert(jobs.size()==1); drain(); assert(values[12]==198);
    // Failed attribute writes do not advance the successful cache.
    fail_attribute=12; sample.percent=50; matter_note_power(&sample, ESP_OK); drain();
    assert(values[12]==198 && s_sent.percent_half==198);
    fail_attribute=-1; request_publish(nullptr); drain(); assert(values[12]==100);
    // A full CHIP queue keeps the snapshot for the next watchdog callback.
    fail_schedule=true; sample.percent=49; matter_note_power(&sample, ESP_OK);
    assert(jobs.empty() && !s_pending);
    fail_schedule=false; request_publish(nullptr); drain(); assert(values[12]==98);
    // ADC noise is held for a minute; validity changes bypass that gate.
    sample.batt_mv=3990; matter_note_power(&sample, ESP_OK); drain(); assert(values[11]==4000);
    test_us+=60000000; matter_note_power(&sample, ESP_OK); drain(); assert(values[11]==3990);
    matter_note_power(nullptr, -1); drain(); assert(values[12]==-1 && values[11]==-1);
    matter_note_power(&sample, ESP_OK); drain(); assert(values[12]==98 && values[11]==3990);
    // Watchdog invalidates frozen input even if the UI produces no new sample.
    test_us+=60000000; request_publish(nullptr); drain();
    assert(values[12]==-1 && values[11]==-1 && values[0]==0 && values[26]==0);
    assert(jobs.empty());
    puts("PASS production Matter battery queue, retries, coalescing and stale watchdog");
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = (ROOT / 'main/matter_battery.cpp').read_text()
    worker = source[source.index('namespace {'):source.index('esp_err_t matter_battery_create(')]
    with tempfile.TemporaryDirectory(prefix='matter-battery-worker-') as directory:
        temp = Path(directory)
        (temp / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n')
        (temp / 'worker.cpp').write_text(MOCKS + worker + TEST)
        compiler = [args.cc]
        if Path(args.cc).stem == 'zig':
            compiler += ['c++']
        else:
            compiler = [str(Path(args.cc).with_name('g++.exe' if args.cc.endswith('.exe') else 'g++'))]
        binary = temp / 'worker.exe'
        subprocess.run(compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror',
                                  '-Wno-unused-variable', '-I', str(temp),
                                  '-I', str(ROOT / 'main'), str(temp / 'worker.cpp'),
                                  '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
