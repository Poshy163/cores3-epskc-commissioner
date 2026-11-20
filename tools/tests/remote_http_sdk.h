#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define MALLOC_CAP_SPIRAM 2
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef struct {
    const char *authorization;
    size_t header_length_override;
    const char *body;
    size_t content_len, received;
    char status[40], type[40], cache[32], challenge[80], response[4096];
} httpd_req_t;
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_sendstr(httpd_req_t *, const char *);
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
int httpd_req_recv(httpd_req_t *, char *, size_t);
void *heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void *);
int xQueueSend(QueueHandle_t, const void *, unsigned);
void vTaskDelay(unsigned);
void app_reboot(void);
