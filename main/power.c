/*
 * AXP2101 fuel-gauge reader.
 *
 * The BSP already owns an AXP2101 device handle for LDO control but keeps it
 * static, so this adds a second device on the same bus. The new i2c_master
 * driver serialises transactions per bus, so this coexists with the BSP and
 * the touch controller without extra locking.
 *
 * Register map (matches XPowersLib / M5Unified usage on this exact board):
 *   0x00  status1: bit3 battery present, bit5 VBUS good
 *   0x01  status2: bits[6:5] 01 = charging
 *   0x30  ADC enable: bit0 VBAT channel
 *   0x34/0x35  VBAT, high 5 bits / low 8 bits, LSB = 1 mV
 *   0xA4  gauge state-of-charge, 0-100
 */
#include "power.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "power";

#define AXP2101_ADDR         0x34
#define AXP2101_REG_STATUS1  0x00
#define AXP2101_REG_STATUS2  0x01
#define AXP2101_REG_ADC_EN   0x30
#define AXP2101_REG_VBAT_H   0x34
#define AXP2101_REG_VBAT_L   0x35
#define AXP2101_REG_SOC      0xA4

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

    /* The gauge runs regardless, but VBAT only shows up in 0x34/0x35 with its
     * ADC channel switched on. */
    uint8_t adc = 0;
    if (reg_read(AXP2101_REG_ADC_EN, &adc) == ESP_OK && !(adc & 0x01)) {
        reg_write(AXP2101_REG_ADC_EN, adc | 0x01);
    }
    return ESP_OK;
}

esp_err_t power_read(power_status_t *out)
{
    out->percent = -1;
    out->batt_mv = -1;
    out->present = false;
    out->charging = false;
    out->vbus = false;

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
    out->full = (st2 & 0x07) == 0x04;   /* charger detail: charge done */
    out->raw_st1 = st1;
    out->raw_st2 = st2;

    /* VBAT is read unconditionally: a plausible cell voltage with the present
     * bit clear means the detection is wrong, not the wiring. */
    uint8_t vh = 0, vl = 0;
    if (reg_read(AXP2101_REG_VBAT_H, &vh) == ESP_OK &&
        reg_read(AXP2101_REG_VBAT_L, &vl) == ESP_OK) {
        out->batt_mv = ((vh & 0x1F) << 8) | vl;
    }

    if (out->present) {
        uint8_t soc = 0;
        if (reg_read(AXP2101_REG_SOC, &soc) == ESP_OK && soc <= 100) {
            out->percent = soc;
        }
    }
    return ESP_OK;
}
