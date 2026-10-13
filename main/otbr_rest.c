#include "otbr_rest.h"
#include "otbr_rest_wire.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "log_ring.h"
#include "power_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "settings.h"
#include "thread.h"
#include "ui.h"

static const char *TAG = "otbr_rest";
static httpd_handle_t s_server;

typedef enum {
    REST_CONTROL_RUNNING,
    REST_CONTROL_EPSKC,
    REST_CONTROL_LOGS,
} rest_control_type_t;

typedef struct {
    rest_control_type_t type;
    bool on;
} rest_control_msg_t;

static QueueHandle_t s_control_queue;
static TaskHandle_t s_control_task;
static bool s_control_start_failed;

/* A group of routes that exists only while its feature is switched on. */
typedef struct {
    const char *name;
    const httpd_uri_t *uris;
    size_t count;
    bool registered;   /* set on the server task; reset once it has stopped */
} route_set_t;

static route_set_t s_epskc_routes, s_log_routes;

/* ot-br-posix returns bare JSON strings for the scalar endpoints, quotes and
 * all, so HA's client parses them as JSON rather than plain text. */
static esp_err_t send_json(httpd_req_t *r, const char *json)
{
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, json);
}

static esp_err_t send_quoted(httpd_req_t *r, const char *value)
{
    char buf[128];
    if (!otbr_rest_quote_json_string(value, buf, sizeof(buf))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return send_json(r, buf);
}

/* esp_http_server may split even a small request body across several reads.
 * Consume exactly Content-Length bytes, and never let a timeout/error turn
 * into an empty body with default semantics. `cap` includes the trailing NUL. */
static bool recv_body(httpd_req_t *r, char *buf, size_t cap)
{
    size_t expected = r->content_len;
    if (cap == 0 || expected >= cap) {
        return false;
    }

    size_t total = 0;
    while (total < expected) {
        int n = httpd_req_recv(r, buf + total, expected - total);
        if (n <= 0) {
            buf[0] = '\0';
            return false;
        }
        total += (size_t) n;
    }
    return otbr_rest_terminate_body(buf, total, cap);
}

static esp_err_t err_404(httpd_req_t *r, const char *why)
{
    httpd_resp_set_status(r, "404 Not Found");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, why);
}

/* GET /node/ba-id -- HA uses this as the router's unique id. */
static esp_err_t h_ba_id(httpd_req_t *r)
{
    char hex[40];
    if (!thread_border_agent_id_hex(hex, sizeof(hex))) {
        return err_404(r, "\"border agent id unavailable\"");
    }
    return send_quoted(r, hex);
}

static esp_err_t h_ext_address(httpd_req_t *r)
{
    char hex[20];
    if (!thread_ext_address_hex(hex, sizeof(hex))) {
        return err_404(r, "\"unavailable\"");
    }
    return send_quoted(r, hex);
}

static esp_err_t h_ext_panid(httpd_req_t *r)
{
    char hex[20];
    if (!thread_ext_panid_hex(hex, sizeof(hex))) {
        return err_404(r, "\"unavailable\"");
    }
    return send_quoted(r, hex);
}

static esp_err_t h_network_name(httpd_req_t *r)
{
    return send_quoted(r, thread_network_name());
}

static esp_err_t h_rloc16(httpd_req_t *r)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", thread_rloc16());
    return send_json(r, buf);
}

/*
 * OpenThread calls that reach the spinel layer (enabling the stack, applying a
 * dataset) need more stack than esp_http_server gives a handler, and blew it
 * badly enough to corrupt FreeRTOS' lists. They run on the UI worker instead;
 * these little structs carry the arguments and the result across.
 */
struct set_enabled_args {
    bool on;
    esp_err_t result;
};

static void do_set_enabled(void *arg)
{
    struct set_enabled_args *a = arg;
    a->result = thread_set_enabled(a->on);
}

struct set_dataset_args {
    const char *hex;
    esp_err_t result;
};

static void do_set_dataset(void *arg)
{
    struct set_dataset_args *a = arg;
    a->result = thread_set_dataset_hex(a->hex);
}

/* Return the payload from either bare hex or a JSON string. JSON escapes are
 * deliberately rejected: a dataset is hexadecimal and never needs them. */
struct share_start_args {
    char code[10];
    uint32_t lifetime_ms;
    uint16_t port;
    esp_err_t result;
};

static void do_share_start(void *arg)
{
    struct share_start_args *a = arg;
    a->result = thread_share_start_on(a->code, sizeof(a->code),
                                      a->lifetime_ms, a->port);
}

struct share_stop_args {
    bool done;
};

static void do_share_stop(void *arg)
{
    struct share_stop_args *a = arg;
    thread_share_stop();
    a->done = true;
}

struct epskc_enable_args {
    bool on;
    bool done;
};

static void do_epskc_enable(void *arg)
{
    struct epskc_enable_args *a = arg;
    thread_epskc_set_feature_enabled(a->on);
    a->done = true;
}

/*
 * GET      /node/state -- "disabled" | "detached" | "child" | "router" | "leader"
 * PUT/POST /node/state -- body "enable" / "disable"
 *
 * python-otbr-api uses PUT; ot-br-posix has historically accepted POST too, so
 * both are registered.
 */
static esp_err_t h_state(httpd_req_t *r)
{
    if (r->method == HTTP_PUT || r->method == HTTP_POST) {
        char body[16] = { 0 };
        if (r->content_len >= sizeof(body)) {
            httpd_resp_set_status(r, "413 Content Too Large");
            return httpd_resp_sendstr(r, "\"body too long\"");
        }
        if (!recv_body(r, body, sizeof(body)) || body[0] == '\0') {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"could not read body\"");
        }
        bool on;
        if (!otbr_rest_parse_toggle(body, &on)) {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"expected enable or disable\"");
        }
        struct set_enabled_args a = { .on = on, .result = ESP_FAIL };
        if (!ui_run_on_worker(do_set_enabled, &a, 20000) || a.result != ESP_OK) {
            httpd_resp_set_status(r, "409 Conflict");
            return httpd_resp_sendstr(r, "\"could not change state\"");
        }
        return send_quoted(r, on ? "enabled" : "disabled");
    }
    return send_quoted(r, thread_role());
}

/*
 * GET /node/dataset/active
 *   Accept: text/plain      -> hex TLVs (what HA asks for)
 *   otherwise               -> a small JSON view
 * PUT /node/dataset/active  -> hex TLVs in the body; applies and attaches
 */
static esp_err_t h_dataset_active(httpd_req_t *r)
{
    if (r->method == HTTP_PUT) {
        /* A full dataset is 254 bytes -> 508 hex chars. */
        static char body[540];
        if (r->content_len >= sizeof(body)) {
            httpd_resp_set_status(r, "413 Content Too Large");
            return httpd_resp_sendstr(r, "\"dataset body too long\"");
        }
        if (!recv_body(r, body, sizeof(body)) || body[0] == '\0') {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"could not read dataset body\"");
        }
        /* Tolerate a quoted JSON string as well as bare hex. */
        char *p = otbr_rest_parse_dataset_hex_body(body);
        if (p == NULL || *p == '\0') {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"malformed dataset string\"");
        }
        /* A JSON body means create_active_dataset(), which builds a network
         * from named fields rather than TLVs. Not supported here; say so
         * rather than failing on hex parsing. */
        if (*p == '{') {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"send dataset TLVs as hex, not JSON\"");
        }
        struct set_dataset_args a = { .hex = p, .result = ESP_FAIL };
        if (!ui_run_on_worker(do_set_dataset, &a, 30000) || a.result != ESP_OK) {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"expected hex TLVs\"");
        }
        /* 200, not 202: python-otbr-api accepts only 200 or 201 and reports
         * anything else as "Failed to call OTBR API". */
        return send_quoted(r, "applied");
    }

    /* Active Dataset TLVs contain the Thread Network Key. */
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    static char hex[540];
    if (!thread_dataset_hex(hex, sizeof(hex))) {
        return err_404(r, "\"no active dataset\"");
    }

    char accept[32] = { 0 };
    httpd_req_get_hdr_value_str(r, "Accept", accept, sizeof(accept));
    if (strstr(accept, "text/plain") != NULL) {
        httpd_resp_set_type(r, "text/plain");
        return httpd_resp_sendstr(r, hex);
    }

    char pan[20] = "", name[20], quoted_name[128];
    thread_ext_panid_hex(pan, sizeof(pan));
    snprintf(name, sizeof(name), "%s", thread_network_name());
    if (!otbr_rest_quote_json_string(name, quoted_name, sizeof(quoted_name))) {
        return ESP_ERR_INVALID_SIZE;
    }
    static char json[220];
    snprintf(json, sizeof(json),
             "{\"NetworkName\":%s,\"ExtPanId\":\"%s\",\"ActiveTimestamp\":{\"Seconds\":0}}",
             quoted_name, pan);
    return send_json(r, json);
}

/*
 * DELETE /node is factory_reset(). Wiping credentials from an unauthenticated
 * HTTP call is not something this device should do, and the client treats 405
 * specifically as "this router cannot factory reset" rather than an error.
 */
static esp_err_t h_node_delete(httpd_req_t *r)
{
    httpd_resp_set_status(r, "405 Method Not Allowed");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "\"factory reset is not supported over REST\"");
}

/* Pending dataset is not maintained here; report empty rather than 500. */
static esp_err_t h_dataset_pending(httpd_req_t *r)
{
    return err_404(r, "\"no pending dataset\"");
}

/*
 * The static buffers below are safe because esp_http_server dispatches
 * requests from a single task: handlers never run concurrently.
 *
 * ePSKc endpoints, the shape python-otbr-api PR #267 expects:
 *   GET    /node/ba-epskc/state -> "enabled" | "disabled"
 *   PUT    /node/ba-epskc/state <- "enable" | "disable"
 *   GET    /node/ba-epskc/key   -> {"state": <s>, "port": <p>}
 *   POST   /node/ba-epskc/key   <- {"lifetime": ms, "port": p}  (both optional)
 *                               -> {"tap": "123456789", "port": <p>}
 *   DELETE /node/ba-epskc/key
 * 404 everywhere when the feature is switched off, which is what the client
 * treats as "this router does not support ePSKc".
 */
/* Parse the intentionally tiny ePSKc options object without accepting
 * substring matches, malformed values, unknown fields or trailing garbage. */
static esp_err_t h_epskc_state(httpd_req_t *r)
{
    if (r->method == HTTP_PUT) {
        char body[24] = { 0 };
        if (r->content_len >= sizeof(body)) {
            httpd_resp_set_status(r, "413 Content Too Large");
            return httpd_resp_sendstr(r, "\"body too long\"");
        }
        if (!recv_body(r, body, sizeof(body)) || body[0] == '\0') {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"could not read body\"");
        }
        bool on;
        if (!otbr_rest_parse_toggle(body, &on)) {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"expected enable or disable\"");
        }
        struct epskc_enable_args a = { .on = on };
        if (!ui_run_on_worker(do_epskc_enable, &a, 20000) || !a.done) {
            httpd_resp_set_status(r, "409 Conflict");
            return httpd_resp_sendstr(r, "\"could not change ePSKc state\"");
        }
        return send_quoted(r, on ? "enabled" : "disabled");
    }
    return send_quoted(r, thread_epskc_feature_enabled() ? "enabled" : "disabled");
}

static esp_err_t h_epskc_key(httpd_req_t *r)
{
    char json[96];
    /* POST returns a one-time TAP; keep every response on this endpoint out of
     * browser and intermediary caches. */
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");

    if (r->method == HTTP_POST) {
        char body[96] = { 0 };
        if (r->content_len >= sizeof(body)) {
            httpd_resp_set_status(r, "413 Content Too Large");
            return httpd_resp_sendstr(r, "\"body too long\"");
        }
        if (!recv_body(r, body, sizeof(body))) {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"could not read body\"");
        }
        /* OpenThread caps lifetime at 10 min; default to the UI's setting. */
        long lifetime = settings_get()->share_minutes * 60000L;
        long port = 0;
        if (!otbr_rest_parse_epskc_options(body, &lifetime, &port)) {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"expected JSON lifetime and/or port\"");
        }
        if (lifetime <= 0 || lifetime > 600000L) {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"lifetime must be 1..600000 ms\"");
        }
        if (port < 0 || port > UINT16_MAX) {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"port must be between 0 and 65535\"");
        }

        struct share_start_args a = {
            .lifetime_ms = (uint32_t) lifetime,
            .port = (uint16_t) port,
            .result = ESP_FAIL,
        };
        if (!ui_run_on_worker(do_share_start, &a, 30000) || a.result != ESP_OK) {
            /* Already running, or the border router is not up yet. */
            httpd_resp_set_status(r, "409 Conflict");
            return httpd_resp_sendstr(r, "\"could not activate ephemeral key\"");
        }
        snprintf(json, sizeof(json), "{\"tap\":\"%s\",\"port\":%u}",
                 a.code, thread_share_port());
        return send_json(r, json);
    }

    if (r->method == HTTP_DELETE) {
        struct share_stop_args a = { 0 };
        if (!ui_run_on_worker(do_share_stop, &a, 20000) || !a.done) {
            httpd_resp_set_status(r, "409 Conflict");
            return httpd_resp_sendstr(r, "\"could not stop ephemeral key\"");
        }
        return send_quoted(r, "stopped");
    }

    snprintf(json, sizeof(json), "{\"state\":\"%s\",\"port\":%u}",
             thread_share_state_rest(), thread_share_port());
    return send_json(r, json);
}

/* GET /node -- summary object ot-br-posix serves at the root. */
static esp_err_t h_node(httpd_req_t *r)
{
    char ba[40] = "", ext[20] = "", pan[20] = "";
    thread_border_agent_id_hex(ba, sizeof(ba));
    thread_ext_address_hex(ext, sizeof(ext));
    thread_ext_panid_hex(pan, sizeof(pan));
    char quoted_name[128];
    if (!otbr_rest_quote_json_string(thread_network_name(), quoted_name, sizeof(quoted_name))) {
        return ESP_ERR_INVALID_SIZE;
    }
    static char json[320];
    snprintf(json, sizeof(json),
             "{\"State\":\"%s\",\"NetworkName\":%s,\"ExtAddress\":\"%s\","
             "\"ExtPanId\":\"%s\",\"Rloc16\":%u,\"BorderAgentId\":\"%s\"}",
             thread_role(), quoted_name, ext, pan, thread_rloc16(), ba);
    return send_json(r, json);
}

/* Read-only, paged battery history. No PMIC reads or flash writes occur in
 * HTTPD; it only copies the UI's samples from the PSRAM ring. */
static esp_err_t h_power_history(httpd_req_t *r)
{
    uint32_t after = 0, limit = 120;
    char query[80], value[24];
    if (httpd_req_get_url_query_len(r)) {
        if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK) {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "query too long");
        }
        esp_err_t err = httpd_query_key_value(query, "after", value, sizeof(value));
        if ((err != ESP_OK && err != ESP_ERR_NOT_FOUND) ||
            (err == ESP_OK && !power_log_parse_uint(value, UINT32_MAX, &after))) {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "invalid after");
        }
        err = httpd_query_key_value(query, "limit", value, sizeof(value));
        if ((err != ESP_OK && err != ESP_ERR_NOT_FOUND) ||
            (err == ESP_OK && (!power_log_parse_uint(value, 120, &limit) || limit == 0))) {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "limit must be 1..120");
        }
    }
    power_log_info_t info;
    if (!power_log_info(&info)) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        return send_json(r, "{\"error\":\"battery history unavailable\"}");
    }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"schema\":1,\"boot_id\":\"%016"PRIx64"\",\"device_uptime_ms\":%"PRId64","
        "\"capacity\":%u,\"oldest_seq\":%"PRIu32",\"latest_seq\":%"PRIu32",\"samples\":[",
        info.boot_id, esp_timer_get_time() / 1000, POWER_LOG_CAPACITY,
        info.oldest_seq, info.latest_seq);
    esp_err_t err = httpd_resp_send_chunk(r, buf, n);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t start = after >= info.latest_seq ? 0 : after + 1;
    if (start && start < info.oldest_seq) {
        start = info.oldest_seq;
    }
    unsigned sent = 0;
    for (uint32_t seq = start; seq && seq <= info.latest_seq && sent < limit; ++seq) {
        power_log_sample_t row;
        if (!power_log_get(seq, &row)) {
            continue; /* overwritten while a slow client was reading */
        }
        size_t len = power_log_json(&row, buf, sizeof(buf));
        if (!len) {
            return ESP_FAIL;
        }
        if (sent && httpd_resp_send_chunk(r, ",", 1) != ESP_OK) {
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(r, buf, len) != ESP_OK) {
            return ESP_FAIL;
        }
        ++sent;
    }
    if (httpd_resp_send_chunk(r, "]}", 2) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* Parse key=a|b. Absent keeps *is_second; anything else is rejected. */
static bool query_choice(const char *query, const char *key, const char *first,
                         const char *second, bool *is_second)
{
    char value[16];
    esp_err_t err = httpd_query_key_value(query, key, value, sizeof(value));
    if (err == ESP_ERR_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK || (strcmp(value, first) != 0 && strcmp(value, second) != 0)) {
        return false;
    }
    *is_second = strcmp(value, second) == 0;
    return true;
}

typedef struct {
    char batch[1536];
    size_t used;
    log_ring_line_t line;
    char json[448];
} logs_scratch_t;

/* Coalesce small writes; one chunk per line would mean one TCP send each. */
static esp_err_t logs_send(httpd_req_t *r, logs_scratch_t *s, const char *data, size_t len)
{
    if (s->used + len > sizeof(s->batch)) {
        if (s->used && httpd_resp_send_chunk(r, s->batch, s->used) != ESP_OK) {
            return ESP_FAIL;
        }
        s->used = 0;
        if (len > sizeof(s->batch)) {
            return httpd_resp_send_chunk(r, data, len);
        }
    }
    memcpy(s->batch + s->used, data, len);
    s->used += len;
    return ESP_OK;
}

/*
 * Recent log lines from the PSRAM ring: this boot, or with boot=previous the
 * boot that ended in the last panic, watchdog or software reset. Paged by
 * sequence number like /power/history, as JSON or format=text. Short paged
 * requests rather than a held-open stream, so Home Assistant's REST calls on
 * this single-task server are never starved. No flash access.
 */
static esp_err_t h_logs(httpd_req_t *r)
{
    uint32_t after = 0, limit = 200;
    bool previous = false, text = false;
    char query[96], value[24];
    if (httpd_req_get_url_query_len(r)) {
        if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK) {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "query too long");
        }
        esp_err_t err = httpd_query_key_value(query, "after", value, sizeof(value));
        if ((err != ESP_OK && err != ESP_ERR_NOT_FOUND) ||
            (err == ESP_OK && !power_log_parse_uint(value, UINT32_MAX, &after))) {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "invalid after");
        }
        err = httpd_query_key_value(query, "limit", value, sizeof(value));
        if ((err != ESP_OK && err != ESP_ERR_NOT_FOUND) ||
            (err == ESP_OK && (!power_log_parse_uint(value, 200, &limit) || limit == 0))) {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "limit must be 1..200");
        }
        if (!query_choice(query, "boot", "current", "previous", &previous)) {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "boot must be current or previous");
        }
        if (!query_choice(query, "format", "json", "text", &text)) {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "format must be json or text");
        }
    }
    const log_ring_boot_t boot = previous ? LOG_RING_PREVIOUS : LOG_RING_CURRENT;
    log_ring_info_t info;
    if (!log_ring_info(boot, &info)) {
        httpd_resp_set_status(r, HTTPD_404);
        return send_json(r, previous
            ? "{\"error\":\"no previous boot log: last start was a power-on or new firmware\"}"
            : "{\"error\":\"log capture unavailable\"}");
    }
    /* Keep the ~2 KB of buffers off the 4 KB internal-RAM server stack. */
    logs_scratch_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    if (s == NULL) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        return send_json(r, "{\"error\":\"out of memory\"}");
    }
    s->used = 0;
    httpd_resp_set_type(r, text ? "text/plain; charset=utf-8" : "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");

    char head[400];
    int n = text
        ? snprintf(head, sizeof(head), "# %s boot %" PRIu32 ", lines %" PRIu32 "..%" PRIu32
                   ", this boot started by: %s\n", previous ? "previous" : "current",
                   info.boot_count, info.oldest_seq, info.latest_seq, ui_reset_reason_str())
        : snprintf(head, sizeof(head),
                   "{\"schema\":1,\"boot\":\"%s\",\"boot_id\":\"%016" PRIx64 "\",\"boot_count\":%" PRIu32
                   ",\"reset_reason\":\"%s\",\"device_uptime_ms\":%" PRId64 ",\"capacity\":%u"
                   ",\"oldest_seq\":%" PRIu32 ",\"latest_seq\":%" PRIu32 ",\"last_uptime_ms\":%" PRIu32
                   ",\"lines\":[", previous ? "previous" : "current", info.boot_id, info.boot_count,
                   ui_reset_reason_str(), esp_timer_get_time() / 1000, LOG_RING_LINES,
                   info.oldest_seq, info.latest_seq, info.last_uptime_ms);
    esp_err_t err = n > 0 && (size_t) n < sizeof(head) ? logs_send(r, s, head, (size_t) n) : ESP_FAIL;

    uint32_t start = after >= info.latest_seq ? 0 : after + 1;
    if (start && start < info.oldest_seq) {
        start = info.oldest_seq;
    }
    unsigned sent = 0;
    for (uint32_t seq = start; err == ESP_OK && seq && seq <= info.latest_seq && sent < limit; ++seq) {
        if (!log_ring_get(boot, seq, &s->line)) {
            continue;   /* overwritten while a slow client paged, or torn by a crash */
        }
        if (text) {
            size_t len = strlen(s->line.text);
            s->line.text[len] = '\n';
            err = logs_send(r, s, s->line.text, len + 1);
        } else {
            size_t len = log_ring_json(&s->line, s->json, sizeof(s->json));
            err = len == 0 ? ESP_FAIL : sent ? logs_send(r, s, ",", 1) : ESP_OK;
            if (err == ESP_OK) {
                err = logs_send(r, s, s->json, len);
            }
        }
        ++sent;
    }
    if (err == ESP_OK && !text) {
        err = logs_send(r, s, "]}", 2);
    }
    if (err == ESP_OK && s->used) {
        err = httpd_resp_send_chunk(r, s->batch, s->used);
    }
    heap_caps_free(s);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(r, NULL, 0);
}

esp_err_t otbr_rest_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = OTBR_REST_PORT;
    cfg.ctrl_port = 32768 + OTBR_REST_PORT;   /* keep clear of other servers */
    cfg.max_uri_handlers = 22;
    cfg.max_open_sockets = 3;
    /* Keep HTTPD's default internal-RAM stack. Active-dataset reads reach the
     * OpenThread NVS backend, and ESP-IDF cannot disable the flash cache while
     * the current task stack lives in PSRAM. The low-bandwidth Wi-Fi buffer
     * profile leaves the camera's contiguous DMA headroom instead. */
    cfg.stack_size = 4096;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        /* Non-fatal by design: the UI and Thread stack matter more than this. */
        ESP_LOGE(TAG, "could not start REST API: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/power/history",        .method = HTTP_GET,    .handler = h_power_history },
        { .uri = "/node",                 .method = HTTP_GET,    .handler = h_node },
        { .uri = "/node",                 .method = HTTP_DELETE, .handler = h_node_delete },
        { .uri = "/node/ba-id",           .method = HTTP_GET,  .handler = h_ba_id },
        { .uri = "/node/ext-address",     .method = HTTP_GET,  .handler = h_ext_address },
        { .uri = "/node/ext-panid",       .method = HTTP_GET,  .handler = h_ext_panid },
        { .uri = "/node/network-name",    .method = HTTP_GET,  .handler = h_network_name },
        { .uri = "/node/rloc16",          .method = HTTP_GET,  .handler = h_rloc16 },
        { .uri = "/node/state",           .method = HTTP_GET,  .handler = h_state },
        { .uri = "/node/state",           .method = HTTP_POST, .handler = h_state },
        { .uri = "/node/state",           .method = HTTP_PUT,  .handler = h_state },
        { .uri = "/node/dataset/active",  .method = HTTP_GET,  .handler = h_dataset_active },
        { .uri = "/node/dataset/active",  .method = HTTP_PUT,  .handler = h_dataset_active },
        { .uri = "/node/dataset/pending", .method = HTTP_GET,  .handler = h_dataset_pending },
    };
    esp_err_t route_err = ESP_OK;
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t rerr = httpd_register_uri_handler(s_server, &uris[i]);
        if (rerr != ESP_OK) {
            /* Silently missing routes look like an unsupported endpoint to
             * the client, which is a confusing way to run out of handlers. */
            ESP_LOGE(TAG, "could not register %s: %s", uris[i].uri, esp_err_to_name(rerr));
            route_err = rerr;
            break;
        }
    }
    if (route_err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        s_epskc_routes.registered = s_log_routes.registered = false;
        ESP_LOGE(TAG, "REST API stopped after incomplete route registration");
        return route_err;
    }
    otbr_rest_set_epskc(settings_get()->rest_epskc);
    otbr_rest_set_logs(settings_get()->rest_logs);

    ESP_LOGI(TAG, "REST API on port %d", OTBR_REST_PORT);
    return ESP_OK;
}

/*
 * The ba-epskc routes exist only while the feature is on. Switched off they
 * are unregistered rather than answering 404 with an explanation, so the API
 * is byte-for-byte what a firmware without ePSKc support would serve.
 */
static const httpd_uri_t epskc_uris[] = {
    { .uri = "/node/ba-epskc/state", .method = HTTP_GET,    .handler = h_epskc_state },
    { .uri = "/node/ba-epskc/state", .method = HTTP_PUT,    .handler = h_epskc_state },
    { .uri = "/node/ba-epskc/key",   .method = HTTP_GET,    .handler = h_epskc_key },
    { .uri = "/node/ba-epskc/key",   .method = HTTP_POST,   .handler = h_epskc_key },
    { .uri = "/node/ba-epskc/key",   .method = HTTP_DELETE, .handler = h_epskc_key },
};

static route_set_t s_epskc_routes = {
    "ba-epskc", epskc_uris, sizeof(epskc_uris) / sizeof(epskc_uris[0]), false,
};

static const httpd_uri_t log_uris[] = {
    { .uri = "/logs", .method = HTTP_GET, .handler = h_logs },
};

static route_set_t s_log_routes = {
    "/logs", log_uris, sizeof(log_uris) / sizeof(log_uris[0]), false,
};

/* URI registration mutates the HTTP server's handler array and is not
 * thread-safe. These callbacks always run on the server task, after any
 * current handler has returned, because they are submitted with
 * httpd_queue_work(). */
static void apply_routes(route_set_t *set, bool on)
{
    httpd_handle_t server = s_server;
    if (server == NULL || on == set->registered) {
        return;
    }

    bool ok = true;
    for (size_t i = 0; i < set->count; i++) {
        if (on) {
            esp_err_t err = httpd_register_uri_handler(server, &set->uris[i]);
            if (err != ESP_OK && err != ESP_ERR_HTTPD_HANDLER_EXISTS) {
                ESP_LOGE(TAG, "could not register %s: %s",
                         set->uris[i].uri, esp_err_to_name(err));
                ok = false;
            }
        } else {
            httpd_unregister_uri_handler(server, set->uris[i].uri, set->uris[i].method);
        }
    }

    if (on && !ok) {
        /* Do not expose a partial feature surface. */
        for (size_t i = 0; i < set->count; i++) {
            httpd_unregister_uri_handler(server, set->uris[i].uri, set->uris[i].method);
        }
        set->registered = false;
        ESP_LOGE(TAG, "%s route update failed closed", set->name);
        return;
    }

    set->registered = on;
    ESP_LOGI(TAG, "%s endpoints %s", set->name, on ? "registered" : "removed");
}

static void apply_epskc_routes(void *arg)
{
    apply_routes(&s_epskc_routes, (uintptr_t) arg != 0);
}

static void apply_log_routes(void *arg)
{
    apply_routes(&s_log_routes, (uintptr_t) arg != 0);
}

static void queue_routes(httpd_work_fn_t apply, const char *name, bool on)
{
    if (s_server == NULL) {
        return;   /* picked up when the server next starts */
    }
    esp_err_t err = httpd_queue_work(s_server, apply, (void *) (uintptr_t) (on ? 1 : 0));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not queue %s route update: %s", name, esp_err_to_name(err));
    }
}

void otbr_rest_set_epskc(bool on)
{
    queue_routes(apply_epskc_routes, s_epskc_routes.name, on);
}

void otbr_rest_set_logs(bool on)
{
    queue_routes(apply_log_routes, s_log_routes.name, on);
}

void otbr_rest_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        s_epskc_routes.registered = s_log_routes.registered = false;
        ESP_LOGI(TAG, "REST API stopped");
    }
}

bool otbr_rest_running(void)
{
    return s_server != NULL;
}

static void rest_control_task(void *arg)
{
    (void) arg;
    rest_control_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_control_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.type == REST_CONTROL_RUNNING) {
            if (msg.on) {
                if (otbr_rest_start() != ESP_OK) {
                    ESP_LOGE(TAG, "asynchronous REST start failed");
                }
            } else {
                otbr_rest_stop();
            }
        } else if (msg.type == REST_CONTROL_EPSKC) {
            thread_epskc_set_feature_enabled(msg.on);
            otbr_rest_set_epskc(msg.on);
        } else if (msg.type == REST_CONTROL_LOGS) {
            otbr_rest_set_logs(msg.on);
        }
    }
}

static bool request_control(rest_control_type_t type, bool on)
{
    if (s_control_queue == NULL || s_control_start_failed) {
        ESP_LOGE(TAG, "REST control task is not available");
        return false;
    }
    rest_control_msg_t msg = { .type = type, .on = on };
    if (xQueueSend(s_control_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "REST control queue full");
        return false;
    }
    return true;
}

esp_err_t otbr_rest_control_init(void)
{
    if (s_control_queue != NULL) {
        return ESP_OK;
    }
    s_control_queue = xQueueCreate(4, sizeof(rest_control_msg_t));
    if (s_control_queue == NULL) {
        ESP_LOGE(TAG, "could not allocate REST control queue");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t otbr_rest_control_start(void)
{
    if (s_control_task != NULL) {
        return ESP_OK;
    }
    if (s_control_queue == NULL) {
        esp_err_t err = otbr_rest_control_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    /* Lifecycle calls can start/stop HTTPD and evolve to touch flash-backed
     * settings, so keep this stack internal for the same cache-safety rule as
     * the server task. */
    if (xTaskCreate(rest_control_task, "rest_control", 4096, NULL, 4,
                    &s_control_task) != pdPASS) {
        s_control_start_failed = true;
        xQueueReset(s_control_queue);
        ESP_LOGE(TAG, "could not start REST control task");
        return ESP_ERR_NO_MEM;
    }
    s_control_start_failed = false;
    return ESP_OK;
}

bool otbr_rest_request_running(bool on)
{
    return request_control(REST_CONTROL_RUNNING, on);
}

bool otbr_rest_request_epskc(bool on)
{
    return request_control(REST_CONTROL_EPSKC, on);
}

bool otbr_rest_request_logs(bool on)
{
    return request_control(REST_CONTROL_LOGS, on);
}
