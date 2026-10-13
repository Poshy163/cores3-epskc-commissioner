/*
 * Recent log lines, kept in PSRAM and readable over the REST API (/logs).
 *
 * The ring lives in no-init external RAM, so a panic, watchdog or software
 * reset leaves the previous boot's lines in place for the next boot to serve.
 * A power cut or brownout loses them. Each committed line is written back from
 * the data cache, because the ESP32-S3 reset paths do not flush it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_RING_LINES 1024   /* per boot; the previous boot keeps its own set */
#define LOG_RING_TEXT 180     /* bytes including NUL; longer lines are cut */

typedef enum {
    LOG_RING_CURRENT = 0,
    LOG_RING_PREVIOUS = 1,
} log_ring_boot_t;

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    char text[LOG_RING_TEXT];
} log_ring_line_t;

typedef struct {
    uint64_t boot_id;          /* random per boot */
    uint32_t boot_count;       /* consecutive boots without a power cut */
    uint32_t oldest_seq, latest_seq;
    uint32_t last_uptime_ms;   /* uptime of the newest line */
} log_ring_info_t;

/* Install the esp_log hook and recover the previous boot's lines. Call once,
 * early in app_main; console output is unchanged. */
esp_err_t log_ring_init(void);

/* Append one complete line that did not pass through esp_log. */
void log_ring_line(const char *text, size_t len);

bool log_ring_info(log_ring_boot_t boot, log_ring_info_t *out);
bool log_ring_get(log_ring_boot_t boot, uint32_t seq, log_ring_line_t *out);

/* {"seq":..,"uptime_ms":..,"text":".."}. Always fits a cap of 64 or more,
 * shortening the text when it must. Returns zero only for smaller caps. */
size_t log_ring_json(const log_ring_line_t *line, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
