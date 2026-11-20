#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "remote_http_production.inc"

static const char test_token[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char good_header[] =
    "Bearer 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static bool enabled, header_read_fails, allocation_fails, format_fails, queue_full, receive_fails;
static bool in_http, expect_reply_unfinished;
static esp_err_t send_error;
static int header_reads, auth_calls, body_reads, allocations, frees, formats, sends, queue_sends, jobs;
static rest_control_msg_t queued[8];
static unsigned elapsed_ms, wait_reads, finish_reply_at;
static uint64_t boot_id;
static unsigned reboots;

bool remote_admin_enabled(void) { return enabled; }
bool remote_admin_authorize(const char *authorization)
{
    ++auth_calls;
    return strcmp(authorization, good_header) == 0;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *value)
{
    snprintf(r->type, sizeof(r->type), "%s", value); return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *value)
{
    snprintf(r->status, sizeof(r->status), "%s", value); return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *name, const char *value)
{
    if (strcmp(name, "Cache-Control") == 0) {
        snprintf(r->cache, sizeof(r->cache), "%s", value);
    } else {
        assert(strcmp(name, "WWW-Authenticate") == 0);
        snprintf(r->challenge, sizeof(r->challenge), "%s", value);
    }
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *value)
{
    assert(in_http && reboots == 0);
    assert(strstr(value, test_token) == NULL && strstr(value, good_header) == NULL);
    snprintf(r->response, sizeof(r->response), "%s", value);
    ++sends;
    if (expect_reply_unfinished) {
        assert(atomic_load(&s_reboot_pending) && !atomic_load(&s_reboot_reply_done));
    }
    return send_error;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name)
{
    assert(strcmp(name, "Authorization") == 0);
    return r->header_length_override ? r->header_length_override :
           r->authorization ? strlen(r->authorization) : 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t cap)
{
    assert(strcmp(name, "Authorization") == 0);
    ++header_reads;
    if (header_read_fails || !r->authorization || strlen(r->authorization) >= cap) {
        return ESP_FAIL;
    }
    strcpy(out, r->authorization);
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *out, size_t cap)
{
    ++body_reads;
    if (receive_fails) { return -1; }
    size_t n = r->content_len - r->received;
    if (n > cap) { n = cap; }
    if (n > 3) { n = 3; } /* exercise split HTTP body reception */
    memcpy(out, r->body + r->received, n);
    r->received += n;
    return (int)n;
}
void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(size == DEVICE_STATUS_JSON_SIZE && caps == MALLOC_CAP_SPIRAM);
    ++allocations;
    return allocation_fails ? NULL : malloc(size);
}
void heap_caps_free(void *p) { assert(p); ++frees; free(p); }
size_t device_status_json(char *out, size_t cap, bool reboot_pending)
{
    ++formats;
    if (format_fails) { return 0; }
    int n = snprintf(out, cap, "{\"schema\":1,\"boot_id\":\"%016llx\","
                     "\"remote\":{\"enabled\":true,\"reboot_pending\":%s}}",
                     (unsigned long long)boot_id, reboot_pending ? "true" : "false");
    assert(n > 0 && (size_t)n < cap);
    return (size_t)n;
}
int xQueueSend(QueueHandle_t queue, const void *message, unsigned delay)
{
    assert(queue == s_control_queue && queue != NULL && delay == 0);
    ++queue_sends;
    if (queue_full) { return 0; }
    assert(jobs < 8);
    queued[jobs++] = *(const rest_control_msg_t *)message;
    return pdTRUE;
}
void vTaskDelay(unsigned ms)
{
    assert(!in_http);
    if (ms == 10) {
        assert(!atomic_load(&s_reboot_reply_done));
        ++wait_reads;
        if (finish_reply_at && wait_reads == finish_reply_at) {
            atomic_store(&s_reboot_reply_done, true);
        }
    } else {
        assert(ms == 1000 && atomic_load(&s_reboot_reply_done));
    }
    elapsed_ms += ms;
}
void app_reboot(void)
{
    assert(!in_http && atomic_load(&s_reboot_reply_done) && elapsed_ms >= 1000);
    ++reboots;
    ++boot_id;
}

static void reset(void)
{
    enabled = header_read_fails = allocation_fails = format_fails = queue_full = receive_fails = false;
    in_http = expect_reply_unfinished = false;
    send_error = ESP_OK;
    header_reads = auth_calls = body_reads = allocations = frees = formats = sends = 0;
    queue_sends = jobs = 0;
    elapsed_ms = wait_reads = finish_reply_at = reboots = 0;
    boot_id = 1;
    s_control_queue = (void *)1;
    s_control_task = (void *)2;
    s_control_start_failed = false;
    atomic_store(&s_reboot_pending, false);
    atomic_store(&s_reboot_reply_done, false);
}
static httpd_req_t request(const char *body)
{
    httpd_req_t r = { .authorization = good_header, .body = body,
                      .content_len = body ? strlen(body) : 0 };
    strcpy(r.status, "200 OK");
    return r;
}
static esp_err_t call(esp_err_t (*handler)(httpd_req_t *), httpd_req_t *r)
{
    in_http = true;
    esp_err_t err = handler(r);
    in_http = false;
    assert(strcmp(r->type, "application/json") == 0 && strcmp(r->cache, "no-store") == 0);
    return err;
}
static void test_authorization(void)
{
    reset();
    httpd_req_t r = request("\"reboot\"");
    assert(call(h_device_status, &r) == ESP_OK && strncmp(r.status, "503", 3) == 0);
    r = request("\"reboot\"");
    assert(call(h_device_reboot, &r) == ESP_OK && strncmp(r.status, "503", 3) == 0);
    assert(header_reads == 0 && auth_calls == 0 && allocations == 0 && body_reads == 0 && jobs == 0);
    for (int variant = 0; variant < 5; ++variant) {
        reset(); enabled = true;
        r = request("\"reboot\"");
        char wrong[72]; strcpy(wrong, good_header); wrong[70] = '0';
        if (variant == 0) { r.authorization = NULL; }
        if (variant == 1) { r.authorization = wrong; }
        if (variant == 2) { r.header_length_override = 4096; }
        if (variant == 3) { r.authorization = "Bearer short"; }
        if (variant == 4) { header_read_fails = true; }
        assert(call(h_device_reboot, &r) == ESP_OK && strncmp(r.status, "401", 3) == 0);
        assert(strcmp(r.challenge, "Bearer realm=\"cores3\"") == 0);
        assert(allocations == 0 && body_reads == 0 && jobs == 0 && !atomic_load(&s_reboot_pending));
        if (variant == 0 || variant == 2 || variant == 3) { assert(header_reads == 0 && auth_calls == 0); }
        r.received = 0;
        assert(call(h_device_status, &r) == ESP_OK && strncmp(r.status, "401", 3) == 0);
        assert(allocations == 0 && formats == 0 && jobs == 0);
    }
    puts("PASS production HTTP: disabled 503, unauthorized 401, bounded header reads, no side effects or secrets");
}
static void test_status(void)
{
    reset(); enabled = true;
    httpd_req_t r = request(NULL);
    assert(call(h_device_status, &r) == ESP_OK && strcmp(r.status, "200 OK") == 0);
    assert(strstr(r.response, "\"boot_id\":\"0000000000000001\"") != NULL);
    assert(strstr(r.response, "\"reboot_pending\":false") != NULL);
    assert(allocations == 1 && formats == 1 && frees == 1);
    atomic_store(&s_reboot_pending, true);
    r = request(NULL);
    assert(call(h_device_status, &r) == ESP_OK && strstr(r.response, "\"reboot_pending\":true"));
    allocation_fails = true;
    r = request(NULL);
    assert(call(h_device_status, &r) == ESP_OK && strncmp(r.status, "503", 3) == 0);
    assert(formats == 2 && frees == 2);
    allocation_fails = false; format_fails = true;
    r = request(NULL);
    assert(call(h_device_status, &r) == ESP_OK && strncmp(r.status, "500", 3) == 0);
    assert(frees == 3);
    format_fails = false; send_error = ESP_FAIL;
    r = request(NULL);
    assert(call(h_device_status, &r) == ESP_FAIL && frees == 4);
    assert(jobs == 0 && reboots == 0);
    puts("PASS production HTTP: status snapshot, pending flag, allocation/format/send failure cleanup");
}
static void test_reboot_bodies(void)
{
    const char *bad[] = { "", "reboot", "\"Reboot\"", "\"reboot\"junk", "{\"reboot\":true}",
        "true", "null", "[]", "\"reboot\\n\"", "\"reboot", "\v\"reboot\"", "\"reboot\"\f" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        reset(); enabled = true;
        httpd_req_t r = request(bad[i]);
        assert(call(h_device_reboot, &r) == ESP_OK && strncmp(r.status, "400", 3) == 0);
        assert(jobs == 0 && !atomic_load(&s_reboot_pending));
    }
    reset(); enabled = true;
    httpd_req_t r = request("\"reboot\"\0junk");
    r.content_len = 13;
    assert(call(h_device_reboot, &r) == ESP_OK && strncmp(r.status, "400", 3) == 0 && jobs == 0);
    r = request("\"reboot\""); r.content_len = 24;
    assert(call(h_device_reboot, &r) == ESP_OK && strncmp(r.status, "400", 3) == 0 && jobs == 0);
    r = request("\"reboot\""); receive_fails = true;
    assert(call(h_device_reboot, &r) == ESP_OK && strncmp(r.status, "400", 3) == 0 && jobs == 0);
    puts("PASS production HTTP: reboot accepts only a strict JSON string and complete bounded body");
}
static void test_queue_and_reboot(void)
{
    for (int failure = 0; failure < 4; ++failure) {
        reset(); enabled = true;
        if (failure == 0) { s_control_task = NULL; }
        if (failure == 1) { s_control_queue = NULL; }
        if (failure == 2) { s_control_start_failed = true; }
        if (failure == 3) { queue_full = true; }
        httpd_req_t r = request("\"reboot\"");
        assert(call(h_device_reboot, &r) == ESP_OK && strncmp(r.status, "503", 3) == 0);
        assert(!atomic_load(&s_reboot_pending) && jobs == 0 && reboots == 0);
    }
    reset(); enabled = true; expect_reply_unfinished = true;
    httpd_req_t r = request(" \t\r\n\"reboot\"\t \r\n");
    assert(call(h_device_reboot, &r) == ESP_OK && strncmp(r.status, "202", 3) == 0);
    expect_reply_unfinished = false;
    assert(atomic_load(&s_reboot_pending) && atomic_load(&s_reboot_reply_done));
    assert(jobs == 1 && queue_sends == 1 && queued[0].type == REST_CONTROL_REBOOT && !queued[0].on);
    assert(reboots == 0 && elapsed_ms == 0);
    r = request("\"reboot\"");
    assert(call(h_device_reboot, &r) == ESP_OK && strncmp(r.status, "202", 3) == 0);
    assert(jobs == 1 && queue_sends == 1 && reboots == 0);
    perform_remote_reboot();
    assert(reboots == 1 && elapsed_ms == 1000 && !atomic_load(&s_reboot_pending));

    reset(); enabled = true; send_error = ESP_FAIL; expect_reply_unfinished = true;
    r = request("\"reboot\"");
    assert(call(h_device_reboot, &r) == ESP_FAIL && strncmp(r.status, "202", 3) == 0);
    expect_reply_unfinished = false;
    assert(jobs == 1 && atomic_load(&s_reboot_pending) && atomic_load(&s_reboot_reply_done));
    perform_remote_reboot();
    assert(reboots == 1 && boot_id == 2);
    /* A later authenticated status poll distinguishes lost reply from failure. */
    reboots = 0; send_error = ESP_OK;
    r = request(NULL);
    assert(call(h_device_status, &r) == ESP_OK);
    assert(strstr(r.response, "\"boot_id\":\"0000000000000002\"") != NULL);
    assert(strstr(r.response, "\"reboot_pending\":false") != NULL);
    puts("PASS production HTTP: unavailable/full queue rollback, duplicate 202, queued reboot, lost reply boot proof");
}
static void test_worker_wait(void)
{
    reset(); atomic_store(&s_reboot_pending, true); finish_reply_at = 3;
    perform_remote_reboot();
    assert(wait_reads == 3 && elapsed_ms == 1030 && reboots == 1 && !atomic_load(&s_reboot_pending));
    reset(); atomic_store(&s_reboot_pending, true);
    perform_remote_reboot();
    assert(wait_reads == 600 && elapsed_ms == 6000 && reboots == 0 && !atomic_load(&s_reboot_pending));
    reset();
    assert(request_control(REST_CONTROL_RUNNING, true));
    assert(jobs == 1 && queued[0].type == REST_CONTROL_RUNNING && queued[0].on);
    puts("PASS production HTTP: worker waits for reply, adds 1000 ms grace, cancels after 6000 ms without reply");
}
int main(void)
{
    test_authorization();
    test_status();
    test_reboot_bodies();
    test_queue_and_reboot();
    test_worker_wait();
    return 0;
}
