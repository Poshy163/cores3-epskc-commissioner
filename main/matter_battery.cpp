#include "matter_battery.h"
#include "matter.h"
#include "matter_battery_model.h"

#include <atomic>
#include <cstdio>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <app/clusters/power-source-server/power-source-server.h>
#include <platform/CHIPDeviceLayer.h>

static_assert(CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT >= 3,
              "Root, border router and battery require three Matter endpoints");

namespace {
namespace ps = chip::app::Clusters::PowerSource;
constexpr char kTag[] = "matter_battery";
static_assert(static_cast<int>(ps::PowerSourceStatusEnum::kStandby) == 2);
static_assert(static_cast<int>(ps::PowerSourceStatusEnum::kUnavailable) == 3);
static_assert(static_cast<int>(ps::BatChargeStateEnum::kIsNotCharging) == 3);
static_assert(static_cast<int>(ps::BatChargeStateEnum::kIsAtFullCharge) == 2);
static_assert(static_cast<int>(ps::BatChargeLevelEnum::kCritical) == 2);

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
matter_battery_sample_t s_sample{};
std::atomic<bool> s_ready{false}, s_pending{false};
uint16_t s_endpoint;
esp_timer_handle_t s_watchdog;
/* Written only by the CHIP event loop. Failed updates leave caches untouched. */
matter_battery_values_t s_sent{-2, -2, -2, -2, -2};
uint64_t s_voltage_at;

bool update(uint32_t id, esp_matter_attr_val_t value)
{
    esp_err_t err = esp_matter::attribute::update(s_endpoint, ps::Id, id, &value);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "attribute 0x%lx update failed: %s",
                 static_cast<unsigned long>(id), esp_err_to_name(err));
    }
    return err == ESP_OK;
}

void publish(intptr_t)
{
    /* Release first: a sample arriving during publication queues one follow-up. */
    s_pending.store(false, std::memory_order_release);
    matter_battery_sample_t sample;
    portENTER_CRITICAL(&s_mux);
    sample = s_sample;
    portEXIT_CRITICAL(&s_mux);
    const uint64_t now = esp_timer_get_time() / 1000;
    const auto next = matter_battery_convert(&sample, now);
    if (next.percent_half != s_sent.percent_half &&
        update(ps::Attributes::BatPercentRemaining::Id,
               esp_matter_nullable_uint8(next.percent_half < 0 ? nullable<uint8_t>() :
                                         nullable<uint8_t>(next.percent_half)))) {
        s_sent.percent_half = next.percent_half;
    }
    if (matter_battery_voltage_due(s_sent.millivolts, next.millivolts, s_voltage_at, now) &&
        update(ps::Attributes::BatVoltage::Id,
               esp_matter_nullable_uint32(next.millivolts < 0 ? nullable<uint32_t>() :
                                          nullable<uint32_t>(next.millivolts)))) {
        s_sent.millivolts = next.millivolts;
        s_voltage_at = now;
    }
    if (next.status != s_sent.status &&
        update(ps::Attributes::Status::Id, esp_matter_enum8(next.status))) {
        s_sent.status = next.status;
    }
    if (next.charge != s_sent.charge &&
        update(ps::Attributes::BatChargeState::Id, esp_matter_enum8(next.charge))) {
        s_sent.charge = next.charge;
    }
    /* ChargeLevel is non-nullable. Preserve its last known value on bad data;
     * Status and the nullable percentage describe the current validity. */
    if (next.level >= 0 && next.level != s_sent.level &&
        update(ps::Attributes::BatChargeLevel::Id, esp_matter_enum8(next.level))) {
        s_sent.level = next.level;
    }
}

void request_publish(void *)
{
    if (!s_ready.load(std::memory_order_acquire) || s_pending.exchange(true)) {
        return;
    }
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(publish, 0) != CHIP_NO_ERROR) {
        s_pending.store(false, std::memory_order_release);
        /* Latest snapshot is retained; the watchdog or next sample retries. */
    }
}
} // namespace

extern "C" void matter_note_power(const power_status_t *sample, esp_err_t error)
{
    matter_battery_sample_t next{};
    next.at_ms = esp_timer_get_time() / 1000;
    if (sample && error == ESP_OK) {
        next.valid = true;
        next.percent = sample->percent;
        next.millivolts = sample->batt_mv;
        next.present = sample->present;
        next.estimated = sample->percent_estimated;
        next.usb = sample->vbus;
        next.charging = sample->charging;
        next.full = sample->full;
    }
    portENTER_CRITICAL(&s_mux);
    s_sample = next;
    portEXIT_CRITICAL(&s_mux);
    request_publish(nullptr);
}

esp_err_t matter_battery_create(esp_matter::node_t *node)
{
    namespace power = esp_matter::cluster::power_source;
    esp_matter::endpoint::power_source::config_t config{};
    config.power_source.feature_flags = power::feature::battery::get_id() |
                                        power::feature::rechargeable::get_id();
    snprintf(config.power_source.description, sizeof(config.power_source.description),
             "CoreS3 battery");
    config.power_source.features.rechargeable.bat_functional_while_charging = true;
    auto *ep = esp_matter::endpoint::power_source::create(
        node, &config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    if (!ep) return ESP_ERR_NO_MEM;
    s_endpoint = esp_matter::endpoint::get_id(ep);
    auto *cluster = esp_matter::cluster::get(ep, ps::Id);
    if (!cluster ||
        !power::attribute::create_bat_percent_remaining(cluster, nullable<uint8_t>(), 0, 200) ||
        !power::attribute::create_bat_voltage(cluster, nullable<uint32_t>(), 0, 0xFFFFFFFE)) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t matter_battery_start(uint16_t tbr_endpoint)
{
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        chip::EndpointId powered[] = {0, tbr_endpoint, s_endpoint};
        if (chip::app::Clusters::PowerSourceServer::Instance().SetEndpointList(
                s_endpoint, chip::Span<chip::EndpointId>(powered)) != CHIP_NO_ERROR) {
            return ESP_FAIL;
        }
    }
    esp_timer_create_args_t timer{};
    timer.callback = request_publish;
    timer.name = "matter_battery";
    timer.skip_unhandled_events = true;
    esp_err_t err = esp_timer_create(&timer, &s_watchdog);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(s_watchdog, 10000000);
    if (err != ESP_OK) {
        esp_timer_delete(s_watchdog);
        s_watchdog = nullptr;
        return err;
    }
    s_ready.store(true, std::memory_order_release);
    request_publish(nullptr);
    ESP_LOGI(kTag, "Power Source endpoint %u: battery, voltage, charge state", s_endpoint);
    return ESP_OK;
}
