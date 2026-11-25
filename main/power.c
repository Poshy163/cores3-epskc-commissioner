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
#include <math.h>
#include <stdatomic.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "matter.h"
#include "power_log.h"
#include "wifi_ctl.h"

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
static bool s_adc_ready;

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
    uint8_t data[2];
    if (i2c_master_transmit_receive(s_axp, &reg_h, 1, data, sizeof(data),
                                  I2C_TIMEOUT_MS) != ESP_OK) {
        return -1;
    }
    return ((data[0] & ((1 << hbits) - 1)) << 8) | data[1];
}

/* Battery telemetry is a nice-to-have: never let it take the app down. */
static esp_err_t ensure_init(void)
{
    if (s_axp && s_adc_ready) {
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
    if (!s_axp) {
        err = i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_axp);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AXP2101 not reachable: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* The gauge runs regardless, but the voltage and temperature results only
     * populate with their ADC channels switched on. */
    uint8_t adc = 0;
    const uint8_t want = ADC_EN_VBAT | ADC_EN_VBUS | ADC_EN_VSYS | ADC_EN_TDIE;
    err = reg_read(AXP2101_REG_ADC_EN, &adc);
    if (err != ESP_OK) {
        return err;
    }
    if ((adc & want) != want) {
        err = reg_write(AXP2101_REG_ADC_EN, adc | want);
        if (err != ESP_OK) {
            return err;
        }
    }
    s_adc_ready = true;
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

/*
 * Open-circuit curve for a single Li-ion cell. Coarse by design: it only has
 * to stand in while the gauge re-converges. Load affects this estimate;
 * never apply an open-circuit curve to a cell being charged.
 */
static int percent_from_mv(int mv)
{
    static const struct { int16_t mv; int8_t pct; } curve[] = {
        { 4200, 100 }, { 4060, 90 }, { 3980, 80 }, { 3920, 70 },
        { 3870, 60 },  { 3820, 50 }, { 3790, 40 }, { 3770, 30 },
        { 3740, 20 },  { 3680, 10 }, { 3450, 5 },  { 3000, 0 },
    };
    const size_t n = sizeof(curve) / sizeof(curve[0]);
    if (mv >= curve[0].mv) {
        return 100;
    }
    for (size_t i = 1; i < n; i++) {
        if (mv >= curve[i].mv) {
            int span_mv = curve[i - 1].mv - curve[i].mv;
            int span_pct = curve[i - 1].pct - curve[i].pct;
            return curve[i].pct + ((mv - curve[i].mv) * span_pct + span_mv / 2) / span_mv;
        }
    }
    return 0;
}

static esp_err_t read_pmic(power_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->percent = -1;
    out->batt_mv = -1;
    out->vbus_mv = -1;
    out->vsys_mv = -1;
    out->pmic_temp_c = INT_MIN;
    out->present = false;
    out->percent_estimated = false;
    out->charging = false;
    out->full = false;
    out->vbus = false;
    out->charge_detail = "unknown";
    out->raw_st1 = 0;
    out->raw_st2 = 0;

    esp_err_t err = ensure_init();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t st1 = 0, st2 = 0;
    err = reg_read(AXP2101_REG_STATUS1, &st1);
    if (err != ESP_OK) {
        return err;
    }
    /* Status 2 carries the charge phase. Do not turn a failed read into a
     * plausible-looking "trickle" status in diagnostics or the UI. */
    err = reg_read(AXP2101_REG_STATUS2, &st2);
    if (err != ESP_OK) {
        return err;
    }

    out->present = st1 & (1 << 3);
    out->vbus = st1 & (1 << 5);
    out->charging = out->present && out->vbus && ((st2 >> 5) & 0x03) == 0x01;
    out->full = out->present && out->vbus && !out->charging && (st2 & 0x07) == 0x04;
    out->charge_detail = !out->present ? "not connected" :
                         !out->vbus ? "discharging" : charge_detail_str(st2);
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
        bool gauge_ok = reg_read(AXP2101_REG_SOC, &soc) == ESP_OK && soc <= 100;
        if (gauge_ok && (soc > 0 || (out->batt_mv >= 2500 && out->batt_mv < 3800))) {
            out->percent = soc;
        } else if (!out->vbus && out->batt_mv >= 2500 && out->batt_mv <= 4350) {
            /*
             * Switching the Base DIN battery out and back in restarts the
             * AXP2101's gauge, which then reports 0 % for several minutes
             * while it re-converges. A cell sitting at 4 V is plainly not
             * empty, so read the voltage curve instead of showing 0 %.
             */
            out->percent = percent_from_mv(out->batt_mv);
            out->percent_estimated = true;
        }
    }
    return ESP_OK;
}

/* ---------------- ESP32-S3 die temperature ---------------- */

/* The console, the Power page and Matter's temperature sampler all read the
 * sensor, so the lazy install and every read run under one mutex: two first
 * calls racing the install would otherwise mark the sensor unavailable. */
static temperature_sensor_handle_t s_tsens;
static bool s_tsens_failed;
static StaticSemaphore_t s_tsens_lock_buf;
static SemaphoreHandle_t s_tsens_lock;
static atomic_int s_tsens_lock_state;   /* 0 none, 1 being created, 2 ready */

static void tsens_lock(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_tsens_lock_state, &expected, 1)) {
        s_tsens_lock = xSemaphoreCreateMutexStatic(&s_tsens_lock_buf);
        atomic_store(&s_tsens_lock_state, 2);
    }
    while (atomic_load(&s_tsens_lock_state) != 2) {
        vTaskDelay(1);
    }
    xSemaphoreTake(s_tsens_lock, portMAX_DELAY);
}

bool power_esp_temp(float *celsius)
{
    if (celsius == NULL) {
        return false;
    }
    tsens_lock();
    if (s_tsens == NULL && !s_tsens_failed) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK ||
            temperature_sensor_enable(s_tsens) != ESP_OK) {
            s_tsens_failed = true;
            ESP_LOGW(TAG, "internal temperature sensor unavailable");
        }
    }
    const bool ok = s_tsens != NULL && !s_tsens_failed &&
                    temperature_sensor_get_celsius(s_tsens, celsius) == ESP_OK;
    xSemaphoreGive(s_tsens_lock);
    return ok;
}

/* ---------------- display-independent telemetry ---------------- */

static StaticSemaphore_t s_pmic_mutex_storage;
static SemaphoreHandle_t s_pmic_mutex;
static atomic_uint s_screen_percent; /* Zero on a headless boot. */
static portMUX_TYPE s_sample_lock = portMUX_INITIALIZER_UNLOCKED;
static power_status_t s_sample;
static float s_die_temp = NAN;
static int64_t s_sample_at;
static esp_err_t s_sample_result = ESP_ERR_INVALID_STATE;
static TaskHandle_t s_power_task;

esp_err_t power_read(power_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_pmic_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_pmic_mutex, portMAX_DELAY);
    esp_err_t result = read_pmic(out);
    xSemaphoreGive(s_pmic_mutex);
    return result;
}

void power_set_screen_percent(unsigned percent)
{
    atomic_store(&s_screen_percent, percent);
}

esp_err_t power_cached(power_status_t *out, float *esp_temp_c)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_sample_lock);
    *out = s_sample;
    float temp = s_die_temp;
    int64_t at = s_sample_at;
    esp_err_t result = s_sample_result;
    portEXIT_CRITICAL(&s_sample_lock);
    if (esp_timer_get_time() - at > 30000000 && result == ESP_OK) {
        result = ESP_ERR_TIMEOUT;
    }
    if (esp_temp_c) {
        *esp_temp_c = result == ESP_OK ? temp : NAN;
    }
    return result;
}

static void sample_power(bool first)
{
    power_status_t ps = {0};
    esp_err_t result = power_read(&ps);
    float die_temp;
    if (!power_esp_temp(&die_temp)) {
        die_temp = NAN;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_sample_lock);
    s_sample = ps;
    s_die_temp = die_temp;
    s_sample_at = now;
    s_sample_result = result;
    portEXIT_CRITICAL(&s_sample_lock);

    power_log_note(&ps, result, atomic_load(&s_screen_percent));
    matter_note_power(&ps, result);
    power_note_sample(result == ESP_OK ? ps.percent : -1,
                      result == ESP_OK && ps.present && !ps.vbus && !ps.percent_estimated);
    /* Assume mains if the boot read fails. Later failures retain the last
     * known profile until a valid reading arrives. */
    if (first || result == ESP_OK) {
        app_wifi_set_power_profile(result != ESP_OK || ps.vbus);
    }
}

static void power_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        xTaskDelayUntil(&next, pdMS_TO_TICKS(10000));
        sample_power(false);
    }
}

esp_err_t power_sampling_start(void)
{
    /* Reserve the task and its stack so sampling does not depend on internal
     * heap availability after display/Wi-Fi initialization. IDF takes bytes.
     * main/linker.lf moves this component's .bss to PSRAM; FreeRTOS requires
     * an internal TCB, and the stack stays internal because this task drives
     * I2C and esp_pm_configure(). Measured on the bench, its deepest path (a
     * power-source change switching the CPU and Wi-Fi profile both ways) uses
     * about 1.6 KB; 3 KB leaves room for error paths. `status` reports the
     * headroom as "power st". */
    static DRAM_ATTR StaticTask_t task_storage;
    static DRAM_ATTR StackType_t stack[3072 / sizeof(StackType_t)];
    s_pmic_mutex = xSemaphoreCreateMutexStatic(&s_pmic_mutex_storage);
    sample_power(true);
    s_power_task = xTaskCreateStatic(power_task, "power", sizeof(stack), NULL, 3, stack,
                                     &task_storage);
    return s_power_task ? ESP_OK : ESP_ERR_NO_MEM;
}

size_t power_task_stack_free(void)
{
    return s_power_task ? (size_t) uxTaskGetStackHighWaterMark(s_power_task) : 0;
}

/* ---------------- discharge rate ---------------- */

/*
 * One sample a minute, twelve kept: the estimate uses the oldest sample still
 * in the window against the newest. Gauge resolution is 1 %, so under ten
 * minutes the rate would be dominated by quantisation and is withheld.
 */
#define HIST_N 12
static portMUX_TYPE s_hist_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int64_t t_us;
    int pct;
} s_hist[HIST_N];
static int s_hist_n;      /* valid entries */
static int s_hist_head;   /* next write slot */

void power_note_sample(int percent, bool on_battery)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_hist_lock);
    if (!on_battery || percent < 0 || percent > 100) {
        s_hist_n = 0;
        s_hist_head = 0;
        portEXIT_CRITICAL(&s_hist_lock);
        return;
    }
    if (s_hist_n) {
        int newest = (s_hist_head + HIST_N - 1) % HIST_N;
        int64_t elapsed = now - s_hist[newest].t_us;
        /* A gap or gauge correction invalidates a continuous discharge run. */
        if (elapsed > 90000000 || percent > s_hist[newest].pct || elapsed < 0) {
            s_hist_n = 0;
            s_hist_head = 0;
        } else if (elapsed < 60000000) {
            portEXIT_CRITICAL(&s_hist_lock);
            return;
        }
    }
    s_hist[s_hist_head].t_us = now;
    s_hist[s_hist_head].pct = percent;
    s_hist_head = (s_hist_head + 1) % HIST_N;
    if (s_hist_n < HIST_N) {
        s_hist_n++;
    }
    portEXIT_CRITICAL(&s_hist_lock);
}

bool power_discharge_rate(float *pct_per_hour, int *minutes_left)
{
    portENTER_CRITICAL(&s_hist_lock);
    if (s_hist_n < 2) {
        portEXIT_CRITICAL(&s_hist_lock);
        return false;
    }
    int newest = (s_hist_head + HIST_N - 1) % HIST_N;
    int oldest = (s_hist_n == HIST_N) ? s_hist_head : 0;

    int64_t elapsed = s_hist[newest].t_us - s_hist[oldest].t_us;
    int drop = s_hist[oldest].pct - s_hist[newest].pct;
    int remaining = s_hist[newest].pct;
    int64_t age = esp_timer_get_time() - s_hist[newest].t_us;
    portEXIT_CRITICAL(&s_hist_lock);
    double dt_h = elapsed / 3.6e9;
    if (dt_h < (10.0 / 60.0) || drop < 2 || age > 90000000) {
        return false;
    }
    float rate = (float) (drop / dt_h);
    if (pct_per_hour) {
        *pct_per_hour = rate;
    }
    if (minutes_left) {
        *minutes_left = (int) (remaining / rate * 60.0f);
    }
    return true;
}
