#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef unsigned nvs_handle_t;
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
void esp_fill_random(void *, size_t);

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void test_enter_critical(portMUX_TYPE *);
void test_exit_critical(portMUX_TYPE *);
#define portENTER_CRITICAL(p) test_enter_critical(p)
#define portEXIT_CRITICAL(p) test_exit_critical(p)
typedef struct test_mutex { bool held; } *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
