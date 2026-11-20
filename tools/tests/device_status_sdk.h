#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef int esp_log_level_t;
typedef struct esp_netif esp_netif_t;
#define ESP_OK 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
typedef struct {
    char version[32];
    char idf_ver[32];
    uint8_t app_elf_sha256[32];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
int64_t esp_timer_get_time(void);
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
size_t heap_caps_get_minimum_free_size(unsigned);
void *heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void *);
