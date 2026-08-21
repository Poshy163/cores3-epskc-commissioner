#include "otbr_rest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "settings.h"
#include "thread.h"

static const char *TAG = "otbr_rest";
static httpd_handle_t s_server;

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
 * GET  /node/state -- "disabled" | "detached" | "child" | "router" | "leader"
 * POST /node/state -- body "enable" / "disable"
 */
static esp_err_t h_state(httpd_req_t *r)
{
    if (r->method == HTTP_POST) {
        char body[16] = { 0 };
        int n = httpd_req_recv(r, body, sizeof(body) - 1);
        if (n <= 0) {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"empty body\"");
        }
        bool on = strstr(body, "enable") != NULL && strstr(body, "disable") == NULL;
        if (thread_set_enabled(on) != ESP_OK) {
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
        int total = 0;
        while (total < (int) sizeof(body) - 1) {
            int n = httpd_req_recv(r, body + total, sizeof(body) - 1 - total);
            if (n <= 0) {
                break;
            }
            total += n;
        }
        body[total > 0 ? total : 0] = '\0';
        /* Tolerate a quoted JSON string as well as bare hex. */
        char *p = body;
        if (*p == '"') {
            p++;
            char *q = strchr(p, '"');
            if (q) {
                *q = '\0';
            }
        }
        if (thread_set_dataset_hex(p) != ESP_OK) {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"expected hex TLVs\"");
        }
        httpd_resp_set_status(r, "202 Accepted");
        return httpd_resp_sendstr(r, "\"applied\"");
    }

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
/* Pulls one integer field out of a small JSON body without a parser. */
static long json_int(const char *body, const char *key, long fallback)
{
    const char *p = strstr(body, key);
    if (p == NULL) {
        return fallback;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return fallback;
    }
    return strtol(p + 1, NULL, 10);
}

static esp_err_t h_epskc_state(httpd_req_t *r)
{
    if (r->method == HTTP_PUT) {
        char body[24] = { 0 };
        int n = httpd_req_recv(r, body, sizeof(body) - 1);
        if (n <= 0) {
            httpd_resp_set_status(r, "400 Bad Request");
            return httpd_resp_sendstr(r, "\"empty body\"");
        }
        bool on = strstr(body, "disable") == NULL && strstr(body, "enable") != NULL;
        thread_epskc_set_feature_enabled(on);
        return send_quoted(r, on ? "enabled" : "disabled");
    }
    return send_quoted(r, thread_epskc_feature_enabled() ? "enabled" : "disabled");
}

static esp_err_t h_epskc_key(httpd_req_t *r)
{
    char json[96];

    if (r->method == HTTP_POST) {
        char body[96] = { 0 };
        int n = httpd_req_recv(r, body, sizeof(body) - 1);
        if (n < 0) {
            n = 0;
        }
        body[n] = '\0';
        /* OpenThread caps lifetime at 10 min; default to the UI's setting. */
        long lifetime = json_int(body, "lifetime", settings_get()->share_minutes * 60000L);
        long port = json_int(body, "port", 0);
        if (lifetime <= 0 || lifetime > 600000L) {
            lifetime = 300000L;
        }

        char code[10];
        esp_err_t err = thread_share_start_on(code, sizeof(code), (uint32_t) lifetime,
                                              (uint16_t) port);
        if (err != ESP_OK) {
            /* Already running, or the border router is not up yet. */
            httpd_resp_set_status(r, "409 Conflict");
            return httpd_resp_sendstr(r, "\"could not activate ephemeral key\"");
        }
        snprintf(json, sizeof(json), "{\"tap\":\"%s\",\"port\":%u}",
                 code, thread_share_port());
        return send_json(r, json);
    }

    if (r->method == HTTP_DELETE) {
        thread_share_stop();
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
    /*
     * 4 KB rather than the 4096+ default profile: the task stack must come from
     * internal RAM, and on this board the largest free block hovers around
     * 7-8 KB once Thread, LVGL and the console are up.
     */
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
        { .uri = "/node",                 .method = HTTP_GET,  .handler = h_node },
        { .uri = "/node/ba-id",           .method = HTTP_GET,  .handler = h_ba_id },
        { .uri = "/node/ext-address",     .method = HTTP_GET,  .handler = h_ext_address },
        { .uri = "/node/ext-panid",       .method = HTTP_GET,  .handler = h_ext_panid },
        { .uri = "/node/network-name",    .method = HTTP_GET,  .handler = h_network_name },
        { .uri = "/node/rloc16",          .method = HTTP_GET,  .handler = h_rloc16 },
        { .uri = "/node/state",           .method = HTTP_GET,  .handler = h_state },
        { .uri = "/node/state",           .method = HTTP_POST, .handler = h_state },
        { .uri = "/node/dataset/active",  .method = HTTP_GET,  .handler = h_dataset_active },
        { .uri = "/node/dataset/active",  .method = HTTP_PUT,  .handler = h_dataset_active },
        { .uri = "/node/dataset/pending", .method = HTTP_GET,  .handler = h_dataset_pending },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t rerr = httpd_register_uri_handler(s_server, &uris[i]);
        if (rerr != ESP_OK) {
            /* Silently missing routes look like an unsupported endpoint to
             * the client, which is a confusing way to run out of handlers. */
            ESP_LOGE(TAG, "could not register %s: %s", uris[i].uri, esp_err_to_name(rerr));
        }
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

void otbr_rest_set_epskc(bool on)
{
    if (s_server == NULL) {
        return;   /* picked up when the server next starts */
    }
    for (size_t i = 0; i < sizeof(epskc_uris) / sizeof(epskc_uris[0]); i++) {
        if (on) {
            esp_err_t rerr = httpd_register_uri_handler(s_server, &epskc_uris[i]);
            if (rerr != ESP_OK) {
                ESP_LOGE(TAG, "could not register %s: %s",
                         epskc_uris[i].uri, esp_err_to_name(rerr));
            }
        } else {
            httpd_unregister_uri_handler(s_server, epskc_uris[i].uri, epskc_uris[i].method);
        }
    }
    ESP_LOGI(TAG, "ba-epskc endpoints %s", on ? "registered" : "removed");
}

void otbr_rest_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "REST API stopped");
    }
}

bool otbr_rest_running(void)
{
    return s_server != NULL;
}
