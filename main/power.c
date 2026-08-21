/*
 * AXP2101 reader.
 *
 * The BSP already owns an AXP2101 device handle for LDO control but keeps it
 * static, so this adds a second device on the same bus. The new i2c_master
 * driver serialises transactions per bus, so this coexists with the BSP and
 * the touch controller without extra locking.
 *
 * Register map (matches XPowersLib / M5Unified usage on this exact board):
 *   0x00  status1: bit3 battery present, bit5 VBUS good
 *   0x01  status2: bits[6:5] 01 = charging; bits[2:0] charger detail
 *   0x30  ADC enable: bit0 VBAT, bit2 VBUS, bit3 VSYS, bit4 die temp
 *   0x34/0x35  VBAT  H5L8, 1 mV
 *   0x38/0x39  VBUS  H6L8, 1 mV
 *   0x3A/0x3B  VSYS  H6L8, 1 mV
 *   0x3C/0x3D  TDIE  H6L8, degC = 22 + (7274 - raw) / 20
 *   0xA4  gauge state-of-charge, 0-100
 */
#include "power.h"

#include <limits.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "power";

#define AXP2101_ADDR         0x34
#define AXP2101_REG_STATUS1  0x00
#define AXP2101_REG_STATUS2  0x01
#define AXP2101_REG_ADC_EN   0x30
#define AXP2101_REG_VBAT_H   0x34
#define AXP2101_REG_VBUS_H   0x38
#define AXP2101_REG_VSYS_H   0x3A
#define AXP2101_REG_TDIE_H   0x3C
#define AXP2101_REG_SOC      0xA4

#define ADC_EN_VBAT (1 << 0)
#define ADC_EN_VBUS (1 << 2)
#define ADC_EN_VSYS (1 << 3)
#define ADC_EN_TDIE (1 << 4)

#define I2C_TIMEOUT_MS 200

static i2c_master_dev_handle_t s_axp;

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = { reg, val };
    return i2c_master_transmit(s_axp, data, sizeof(data), I2C_TIMEOUT_MS);
}

/* Two-register ADC result: `hbits` significant bits in the high byte. */
static int read_adc(uint8_t reg_h, int hbits)
{
    uint8_t h = 0, l = 0;
    if (reg_read(reg_h, &h) != ESP_OK || reg_read(reg_h + 1, &l) != ESP_OK) {
        return -1;
    }
    return ((h & ((1 << hbits) - 1)) << 8) | l;
}

/* Battery telemetry is a nice-to-have: never let it take the app down. */
static esp_err_t ensure_init(void)
{
    if (s_axp) {
        return ESP_OK;
    }
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        return err;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_axp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not reachable: %s", esp_err_to_name(err));
        return err;
    }

    /* The gauge runs regardless, but the voltage and temperature results only
     * populate with their ADC channels switched on. */
    uint8_t adc = 0;
    const uint8_t want = ADC_EN_VBAT | ADC_EN_VBUS | ADC_EN_VSYS | ADC_EN_TDIE;
    if (reg_read(AXP2101_REG_ADC_EN, &adc) == ESP_OK && (adc & want) != want) {
        reg_write(AXP2101_REG_ADC_EN, adc | want);
    }
    return ESP_OK;
}

static const char *charge_detail_str(uint8_t st2)
{
    switch (st2 & 0x07) {
    case 0:  return "trickle";
    case 1:  return "pre-charge";
    case 2:  return "constant current";
    case 3:  return "constant voltage";
    case 4:  return "done";
    case 5:  return "not charging";
    default: return "unknown";
    }
}

esp_err_t power_read(power_status_t *out)
{
    out->percent = -1;
    out->batt_mv = -1;
    out->vbus_mv = -1;
    out->vsys_mv = -1;
    out->pmic_temp_c = INT_MIN;
    out->present = false;
    out->charging = false;
    out->full = false;
    out->vbus = false;
    out->charge_detail = "unknown";

    esp_err_t err = ensure_init();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t st1 = 0, st2 = 0;
    err = reg_read(AXP2101_REG_STATUS1, &st1);
    if (err != ESP_OK) {
        return err;
    }
    reg_read(AXP2101_REG_STATUS2, &st2);

    out->present = st1 & (1 << 3);
    out->vbus = st1 & (1 << 5);
    out->charging = ((st2 >> 5) & 0x03) == 0x01;
    out->full = (st2 & 0x07) == 0x04;
    out->charge_detail = charge_detail_str(st2);
    out->raw_st1 = st1;
    out->raw_st2 = st2;

    /* VBAT is read unconditionally: a plausible cell voltage with the present
     * bit clear means the detection is wrong, not the wiring. */
    out->batt_mv = read_adc(AXP2101_REG_VBAT_H, 5);
    out->vbus_mv = read_adc(AXP2101_REG_VBUS_H, 6);
    out->vsys_mv = read_adc(AXP2101_REG_VSYS_H, 6);

    int raw = read_adc(AXP2101_REG_TDIE_H, 6);
    if (raw >= 0) {
        out->pmic_temp_c = 22 + (7274 - raw) / 20;
    }

    if (out->present) {
        uint8_t soc = 0;
        if (reg_read(AXP2101_REG_SOC, &soc) == ESP_OK && soc <= 100) {
            out->percent = soc;
        }
    }
    return ESP_OK;
}

/* ---------------- ESP32-S3 die temperature ---------------- */

static temperature_sensor_handle_t s_tsens;
static bool s_tsens_failed;

bool power_esp_temp(float *celsius)
{
    if (s_tsens == NULL && !s_tsens_failed) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK ||
            temperature_sensor_enable(s_tsens) != ESP_OK) {
            s_tsens_failed = true;
            ESP_LOGW(TAG, "internal temperature sensor unavailable");
            return false;
        }
    }
    if (s_tsens == NULL) {
        return false;
    }
    return temperature_sensor_get_celsius(s_tsens, celsius) == ESP_OK;
}

/* ---------------- discharge rate ---------------- */

/*
 * One sample a minute, twelve kept: the estimate uses the oldest sample still
 * in the window against the newest. Gauge resolution is 1 %, so under ten
 * minutes the rate would be dominated by quantisation and is withheld.
 */
#define HIST_N 12
static struct {
    int64_t t_us;
    int pct;
} s_hist[HIST_N];
static int s_hist_n;      /* valid entries */
static int s_hist_head;   /* next write slot */

void power_note_sample(int percent, bool on_battery)
{
    if (!on_battery || percent < 0) {
        s_hist_n = 0;
        s_hist_head = 0;
        return;
    }
    s_hist[s_hist_head].t_us = esp_timer_get_time();
    s_hist[s_hist_head].pct = percent;
    s_hist_head = (s_hist_head + 1) % HIST_N;
    if (s_hist_n < HIST_N) {
        s_hist_n++;
    }
}

bool power_discharge_rate(float *pct_per_hour, int *minutes_left)
{
    if (s_hist_n < 2) {
        return false;
    }
    int newest = (s_hist_head + HIST_N - 1) % HIST_N;
    int oldest = (s_hist_n == HIST_N) ? s_hist_head : 0;

    double dt_h = (s_hist[newest].t_us - s_hist[oldest].t_us) / 3.6e9;
    int drop = s_hist[oldest].pct - s_hist[newest].pct;
    if (dt_h < (10.0 / 60.0) || drop < 1) {
        return false;
    }
    float rate = (float) (drop / dt_h);
    if (pct_per_hour) {
        *pct_per_hour = rate;
    }
    if (minutes_left) {
        *minutes_left = (int) (s_hist[newest].pct / rate * 60.0f);
    }
    return true;
}
