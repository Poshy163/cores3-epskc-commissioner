#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef int gpio_num_t;
typedef int uart_port_t;
typedef struct esp_netif esp_netif_t;
typedef int nvs_handle_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_ARG, ESP_ERR_NVS_NOT_FOUND };
enum { RCP_TYPE_UART, ESP32H2_CHIP, NVS_READONLY, NVS_READWRITE, ESP_RST_SW, ESP_RST_POWERON };
#define RTC_NOINIT_ATTR
void test_log(const char *tag, const char *fmt, ...);
#define ESP_LOGI test_log
#define ESP_LOGW test_log
#define ESP_LOGE test_log
typedef struct {
    int rcp_type, uart_rx_pin, uart_tx_pin, uart_port, uart_baudrate;
    int reset_pin, boot_pin, update_baudrate, target_chip;
    const char *firmware_dir;
} esp_rcp_update_config_t;
typedef struct {
    const char *base_path, *partition_label;
    size_t max_files;
    bool format_if_mount_failed;
} esp_vfs_spiffs_conf_t;
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *);
esp_err_t esp_vfs_spiffs_unregister(const char *);
esp_err_t esp_rcp_update_init(const esp_rcp_update_config_t *);
esp_err_t esp_rcp_update(void);
void esp_rcp_mark_image_verified(bool);
esp_err_t esp_rcp_load_version_in_storage(char *, size_t);
void esp_openthread_set_compatibility_error_callback(void (*cb)(void));
void esp_openthread_set_coprocessor_reset_failure_callback(void (*cb)(void));
const char *esp_err_to_name(esp_err_t);
int64_t esp_timer_get_time(void);
int esp_reset_reason(void);
_Noreturn void esp_restart(void);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
