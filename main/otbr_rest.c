#include "otbr_rest.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "settings.h"
#include "thread.h"
#include "ui.h"

static const char *TAG = "otbr_rest";
static httpd_handle_t s_server;
static bool s_epskc_routes_registered;

typedef enum {
    REST_CONTROL_RUNNING,
    REST_CONTROL_EPSKC,
} rest_control_type_t;

typedef struct {
    rest_control_type_t type;
    bool on;
} rest_control_msg_t;

static QueueHandle_t s_control_queue;
static TaskHandle_t s_control_task;
static bool s_control_start_failed;

/* Defined below; called from otbr_rest_start(). */
void otbr_rest_set_epskc(bool on);

/* ot-br-posix returns bare JSON strings for the scalar endpoints, quotes and
 * all, so HA's client parses them as JSON rather than plain text. */
static esp_err_t send_json(httpd_req_t *r, const char *json)
{
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, json);
}

static esp_err_t send_quoted(httpd_req_t *r, const char *value)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "\"%s\"", value);
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
    buf[total] = '\0';
    return true;
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

static bool parse_toggle(const char *body, bool *on)
{
    const unsigned char *p = (const unsigned char *) body;
    while (isspace(*p) || *p == '"') {
        p++;
    }
    size_t len;
    if (strncmp((const char *) p, "enable", 6) == 0) {
        len = 6;
        *on = true;
    } else if (strncmp((const char *) p, "disable", 7) == 0) {
        len = 7;
        *on = false;
    } else {
        return false;
    }
    p += len;
    while (isspace(*p) || *p == '"') {
        p++;
    }
    return *p == '\0';
}

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
        if (!parse_toggle(body, &on)) {
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
        char *p = body;
        if (*p == '"') {
            p++;
            char *q = strchr(p, '"');
            if (q) {
                *q = '\0';
            }
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

    char pan[20] = "", name[20];
    thread_ext_panid_hex(pan, sizeof(pan));
    snprintf(name, sizeof(name), "%s", thread_network_name());
    static char json[220];
    snprintf(json, sizeof(json),
             "{\"NetworkName\":\"%s\",\"ExtPanId\":\"%s\",\"ActiveTimestamp\":{\"Seconds\":0}}",
             name, pan);
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
static bool parse_epskc_options(const char *body, long *lifetime, long *port)
{
    const char *p = body;
    while (isspace((unsigned char) *p)) {
        p++;
    }
    if (*p == '\0') {
        return true; /* POST with no body uses both defaults. */
    }
    if (*p++ != '{') {
        return false;
    }

    bool saw_lifetime = false;
    bool saw_port = false;
    for (;;) {
        while (isspace((unsigned char) *p)) {
            p++;
        }
        if (*p == '}') {
            p++;
            break;
        }
        if (*p++ != '"') {
            return false;
        }
        const char *key = p;
        while (*p && *p != '"') {
            p++;
        }
        if (*p != '"') {
            return false;
        }
        size_t key_len = (size_t) (p - key);
        p++;
        while (isspace((unsigned char) *p)) {
            p++;
        }
        if (*p++ != ':') {
            return false;
        }
        while (isspace((unsigned char) *p)) {
            p++;
        }

        errno = 0;
        char *end = NULL;
        long value = strtol(p, &end, 10);
        if (end == p || errno == ERANGE) {
            return false;
        }
        if (key_len == 8 && strncmp(key, "lifetime", key_len) == 0 && !saw_lifetime) {
            *lifetime = value;
            saw_lifetime = true;
        } else if (key_len == 4 && strncmp(key, "port", key_len) == 0 && !saw_port) {
            *port = value;
            saw_port = true;
        } else {
            return false;
        }
        p = end;
        while (isspace((unsigned char) *p)) {
            p++;
        }
        if (*p == ',') {
            p++;
            const char *next = p;
            while (isspace((unsigned char) *next)) {
                next++;
            }
            if (*next == '}') {
                return false;
            }
            continue;
        }
        if (*p == '}') {
            p++;
            break;
        }
        return false;
    }
    while (isspace((unsigned char) *p)) {
        p++;
    }
    return *p == '\0';
}

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
        if (!parse_toggle(body, &on)) {
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
        if (!parse_epskc_options(body, &lifetime, &port)) {
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
    static char json[320];
    snprintf(json, sizeof(json),
             "{\"State\":\"%s\",\"NetworkName\":\"%s\",\"ExtAddress\":\"%s\","
             "\"ExtPanId\":\"%s\",\"Rloc16\":%u,\"BorderAgentId\":\"%s\"}",
             thread_role(), thread_network_name(), ext, pan, thread_rloc16(), ba);
    return send_json(r, json);
}

esp_err_t otbr_rest_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = OTBR_REST_PORT;
    cfg.ctrl_port = 32768 + OTBR_REST_PORT;   /* keep clear of other servers */
    cfg.max_uri_handlers = 18;
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
        s_epskc_routes_registered = false;
        ESP_LOGE(TAG, "REST API stopped after incomplete route registration");
        return route_err;
    }
    otbr_rest_set_epskc(settings_get()->rest_epskc);

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

/* URI registration mutates the HTTP server's handler array and is not
 * thread-safe. This callback always runs on the server task, after any current
 * handler has returned, because it is submitted with httpd_queue_work(). */
static void apply_epskc_routes(void *arg)
{
    bool on = (uintptr_t) arg != 0;
    httpd_handle_t server = s_server;
    if (server == NULL || on == s_epskc_routes_registered) {
        return;
    }

    bool ok = true;
    for (size_t i = 0; i < sizeof(epskc_uris) / sizeof(epskc_uris[0]); i++) {
        if (on) {
            esp_err_t err = httpd_register_uri_handler(server, &epskc_uris[i]);
            if (err != ESP_OK && err != ESP_ERR_HTTPD_HANDLER_EXISTS) {
                ESP_LOGE(TAG, "could not register %s: %s",
                         epskc_uris[i].uri, esp_err_to_name(err));
                ok = false;
            }
        } else {
            httpd_unregister_uri_handler(server, epskc_uris[i].uri, epskc_uris[i].method);
        }
    }

    if (on && !ok) {
        /* Do not expose a partial feature surface. */
        for (size_t i = 0; i < sizeof(epskc_uris) / sizeof(epskc_uris[0]); i++) {
            httpd_unregister_uri_handler(server, epskc_uris[i].uri, epskc_uris[i].method);
        }
        s_epskc_routes_registered = false;
        ESP_LOGE(TAG, "ba-epskc route update failed closed");
        return;
    }

    s_epskc_routes_registered = on;
    ESP_LOGI(TAG, "ba-epskc endpoints %s", on ? "registered" : "removed");
}

void otbr_rest_set_epskc(bool on)
{
    if (s_server == NULL) {
        return;   /* picked up when the server next starts */
    }
    esp_err_t err = httpd_queue_work(s_server, apply_epskc_routes,
                                     (void *) (uintptr_t) (on ? 1 : 0));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not queue ba-epskc route update: %s", esp_err_to_name(err));
    }
}

void otbr_rest_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        s_epskc_routes_registered = false;
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
