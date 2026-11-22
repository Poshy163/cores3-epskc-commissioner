#include "matter_temperature.h"
#include "power.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include "esp_log.h"
#include "esp_timer.h"
#include <app/clusters/temperature-measurement-server/CodegenIntegration.h>
#include <platform/CHIPDeviceLayer.h>

static_assert(CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT >= 4,
              "Root, border router, battery and temperature require four Matter endpoints");

namespace {
namespace tm = chip::app::Clusters::TemperatureMeasurement;
constexpr char kTag[] = "matter_temp";

/* power.c installs the die sensor for 10..80 C; MeasuredValue is in 0.01 C. */
constexpr int16_t kMinCentiC = 1000;
constexpr int16_t kMaxCentiC = 8000;
constexpr uint64_t kSampleUs = 30ULL * 1000 * 1000;
/* A half-degree change is reported at once; a steady reading is refreshed
 * every five minutes so a subscriber's last value never goes stale. */
constexpr int32_t kStepCentiC = 50;
constexpr uint64_t kRefreshMs = 5ULL * 60 * 1000;
constexpr int32_t kUnknown = INT32_MIN;

uint16_t s_endpoint;
esp_timer_handle_t s_timer;
std::atomic<int32_t> s_latest{kUnknown};
std::atomic<bool> s_scheduled{false};
/* Written only by the CHIP event loop. */
bool s_sent_any;
int32_t s_sent = kUnknown;
uint64_t s_sent_at_ms;

void publish(intptr_t)
{
    s_scheduled.store(false, std::memory_order_release);
    const int32_t value = s_latest.load(std::memory_order_acquire);
    const uint64_t now_ms = esp_timer_get_time() / 1000;
    bool changed;
    if (!s_sent_any || value == kUnknown || s_sent == kUnknown) {
        changed = !s_sent_any || value != s_sent;
    } else {
        changed = std::abs(value - s_sent) >= kStepCentiC;   /* both within 1000..8000 */
    }
    if (!changed && now_ms - s_sent_at_ms < kRefreshMs) {
        return;
    }
    /* Temperature Measurement is a code-driven cluster in this SDK: the
     * cluster object owns MeasuredValue and reports changes itself, so
     * esp_matter::attribute::update() would refuse the write. */
    const auto measured = value == kUnknown
                          ? chip::app::DataModel::Nullable<int16_t>()
                          : chip::app::DataModel::MakeNullable(static_cast<int16_t>(value));
    CHIP_ERROR err = tm::SetMeasuredValue(s_endpoint, measured);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(kTag, "MeasuredValue update failed: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
    s_sent = value;
    s_sent_any = true;
    s_sent_at_ms = now_ms;
}

/* esp_timer task: read the sensor here, publish on the CHIP event loop. */
void sample(void *)
{
    float celsius;
    int32_t value = kUnknown;
    if (power_esp_temp(&celsius) && std::isfinite(celsius)) {
        value = static_cast<int32_t>(std::lround(celsius * 100.0f));
        value = std::min<int32_t>(std::max<int32_t>(value, kMinCentiC), kMaxCentiC);
    }
    s_latest.store(value, std::memory_order_release);
    if (!s_scheduled.exchange(true, std::memory_order_acq_rel) &&
        chip::DeviceLayer::PlatformMgr().ScheduleWork(publish, 0) != CHIP_NO_ERROR) {
        s_scheduled.store(false, std::memory_order_release);
    }
}
} // namespace

esp_err_t matter_temperature_create(esp_matter::node_t *node)
{
    esp_matter::endpoint::temperature_sensor::config_t config;
    config.temperature_measurement.measured_value = nullable<int16_t>();
    config.temperature_measurement.min_measured_value = nullable<int16_t>(kMinCentiC);
    config.temperature_measurement.max_measured_value = nullable<int16_t>(kMaxCentiC);
    auto *ep = esp_matter::endpoint::temperature_sensor::create(
        node, &config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    if (!ep) {
        return ESP_ERR_NO_MEM;
    }
    s_endpoint = esp_matter::endpoint::get_id(ep);
    return ESP_OK;
}

esp_err_t matter_temperature_start(void)
{
    esp_timer_create_args_t timer{};
    timer.callback = sample;
    timer.name = "matter_temp";
    timer.skip_unhandled_events = true;
    esp_err_t err = esp_timer_create(&timer, &s_timer);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_timer_start_periodic(s_timer, kSampleUs);
    if (err != ESP_OK) {
        esp_timer_delete(s_timer);
        s_timer = nullptr;
        return err;
    }
    sample(nullptr);
    ESP_LOGI(kTag, "Temperature Measurement endpoint %u: ESP32-S3 die temperature", s_endpoint);
    return ESP_OK;
}

uint16_t matter_temperature_endpoint(void)
{
    return s_endpoint;
}
