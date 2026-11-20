#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "meshcop_sdk.h"
#include "meshcop.c"

enum scenario {
    NORMAL, HANDSHAKE_STALL, WRITE_STALL, READ_STALL, ACK_STORM,
    SHARED_DEADLINE, LATE_HANDSHAKE, LATE_REPLY, BAD_PASSCODE,
    CRYPTO_FAIL, NONBLOCK_FAIL,
};
static enum scenario scenario;
static int64_t now_us;
static int crypto_calls, resolve_calls, sockets_open, ssl_frees, conf_frees;
static int read_stage, read_calls, handshake_calls, write_calls, ack_count, retry_count;
static int net_recv_calls, rng_calls;
static void *bio_ctx;
static test_send_fn bio_send;
static test_recv_fn bio_recv;
static const unsigned char *retry_buf;
static size_t retry_len;

int64_t esp_timer_get_time(void) { return now_us; }
size_t heap_caps_get_free_size(int cap) { return 100000; }
void vTaskDelay(unsigned ticks) { assert(ticks > 0); now_us += ticks * 10000; }
void mbedtls_net_init(mbedtls_net_context *net) { net->fd = -1; }
void mbedtls_net_free(mbedtls_net_context *net) {
    if (net->fd >= 0) { assert(sockets_open == 1); sockets_open--; net->fd = -1; }
}
int mbedtls_net_set_nonblock(mbedtls_net_context *net) {
    assert(net->fd >= 0); return scenario == NONBLOCK_FAIL ? -1 : 0;
}
int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len) {
    assert(((mbedtls_net_context *)ctx)->fd >= 0); return (int)len;
}
int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len) {
    net_recv_calls++; return MBEDTLS_ERR_SSL_WANT_READ;
}
int getaddrinfo(const char *addr, const char *port, const struct addrinfo *hints,
                struct addrinfo **out) {
    static struct sockaddr address;
    static struct addrinfo result;
    resolve_calls++;
    assert(hints->ai_flags == AI_NUMERICHOST);
    assert(hints->ai_socktype == SOCK_DGRAM && hints->ai_protocol == IPPROTO_UDP);
    assert(strcmp(port, "12345") == 0);
    if (strcmp(addr, "router.local") == 0) { return -1; }
    result = *hints;
    result.ai_family = strchr(addr, ':') ? AF_INET6 : AF_INET;
    result.ai_addr = &address; result.ai_addrlen = sizeof(address); result.ai_next = NULL;
    *out = &result; return 0;
}
void freeaddrinfo(struct addrinfo *addresses) { assert(addresses != NULL); }
int socket(int family, int type, int protocol) {
    assert(family == AF_INET || family == AF_INET6);
    assert(sockets_open == 0); sockets_open++; return 4;
}
int connect(int fd, const struct sockaddr *addr, unsigned len) { assert(fd == 4); return 0; }
void mbedtls_ssl_init(mbedtls_ssl_context *ssl) { }
void mbedtls_ssl_free(mbedtls_ssl_context *ssl) { ssl_frees++; }
void mbedtls_ssl_config_init(mbedtls_ssl_config *conf) { }
void mbedtls_ssl_config_free(mbedtls_ssl_config *conf) { conf_frees++; }
int mbedtls_ssl_config_defaults(mbedtls_ssl_config *conf, int role, int transport, int preset) {
    assert(role == MBEDTLS_SSL_IS_CLIENT && transport == MBEDTLS_SSL_TRANSPORT_DATAGRAM);
    return 0;
}
void mbedtls_ssl_conf_ciphersuites(mbedtls_ssl_config *conf, const int *suites) {
    assert(suites[0] == MBEDTLS_TLS_ECJPAKE_WITH_AES_128_CCM_8 && suites[1] == 0);
}
void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *conf, int mode) { }
void mbedtls_ssl_conf_handshake_timeout(mbedtls_ssl_config *conf, uint32_t min, uint32_t max) {
    assert(min == 1000 && max == 8000);
}
int mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf) {
    assert(crypto_calls == 1); return 0;
}
int mbedtls_ssl_set_hs_ecjpake_password(mbedtls_ssl_context *ssl,
                                      const unsigned char *password, size_t len) {
    assert(len == 9 && memcmp(password, "123456789", 9) == 0); return 0;
}
void mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *ctx, test_send_fn send_fn,
                         test_recv_fn recv_fn, int (*timeout_fn)(void *, unsigned char *, size_t, uint32_t)) {
    assert(timeout_fn == NULL); bio_ctx = ctx; bio_send = send_fn; bio_recv = recv_fn;
}
void mbedtls_ssl_set_timer_cb(mbedtls_ssl_context *ssl, void *ctx,
                            void (*set)(void *, uint32_t, uint32_t), int (*get)(void *)) {
    assert(get(ctx) == -1); set(ctx, 1000, 8000); assert(get(ctx) == 0);
}
int mbedtls_ssl_handshake(mbedtls_ssl_context *ssl) {
    handshake_calls++;
    if (scenario == HANDSHAKE_STALL) {
        unsigned char byte; return bio_recv(bio_ctx, &byte, 1);
    }
    if (scenario == SHARED_DEADLINE) { now_us = 27000000; }
    if (scenario == LATE_HANDSHAKE) { now_us = 30000001; }
    if (scenario == BAD_PASSCODE) { return MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE; }
    return 0;
}
int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len) {
    write_calls++;
    if (scenario == WRITE_STALL || (scenario == NORMAL && retry_count < 2)) {
        if (retry_count > 0) { assert(retry_buf == buf && retry_len == len); }
        retry_buf = buf; retry_len = len; retry_count++;
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return bio_send(bio_ctx, buf, len);
}
int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t cap) {
    static const unsigned char ack[] = {0x60, 0, 0, 1};
    static const unsigned char accepted[] = {0x60, 0x44, 0, 1, 0xff, TLV_STATE, 1, 1};
    static const unsigned char dataset[] = {0x60, 0x44, 0, 2, 0xff, TLV_CHANNEL, 3, 0, 0, 15};
    read_calls++;
    assert(cap >= sizeof(dataset));
    if (scenario == READ_STALL || (scenario == SHARED_DEADLINE &&
        (read_stage > 0 || now_us < 29000000))) { return MBEDTLS_ERR_SSL_WANT_READ; }
    if (scenario == ACK_STORM || (scenario == NORMAL && ack_count < 6)) {
        ack_count++; memcpy(buf, ack, sizeof(ack)); return sizeof(ack);
    }
    if (scenario == LATE_REPLY) { now_us += 5000001; }
    if (read_stage++ == 0) { memcpy(buf, accepted, sizeof(accepted)); return sizeof(accepted); }
    memcpy(buf, dataset, sizeof(dataset)); return sizeof(dataset);
}
const char *mbedtls_ssl_get_ciphersuite(const mbedtls_ssl_context *ssl) { return "ECJPAKE"; }
int mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl) { return 0; }

#if MBEDTLS_VERSION_MAJOR < 4
void mbedtls_entropy_init(mbedtls_entropy_context *ctx) { }
void mbedtls_entropy_free(mbedtls_entropy_context *ctx) { }
void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *ctx) { }
void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *ctx) { }
int mbedtls_entropy_func(void *ctx, unsigned char *buf, size_t len) { return 0; }
int mbedtls_ctr_drbg_random(void *ctx, unsigned char *buf, size_t len) { return 0; }
int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *ctx, test_recv_fn fn, void *arg,
                         const unsigned char *pers, size_t len) {
    crypto_calls++; return scenario == CRYPTO_FAIL ? -1 : 0;
}
void mbedtls_ssl_conf_rng(mbedtls_ssl_config *conf, test_recv_fn fn, void *ctx) { rng_calls++; }
#else
psa_status_t psa_crypto_init(void) { crypto_calls++; return scenario == CRYPTO_FAIL ? -1 : PSA_SUCCESS; }
#endif

static void reset(enum scenario next) {
    assert(sockets_open == 0); scenario = next; now_us = 0;
    crypto_calls = resolve_calls = ssl_frees = conf_frees = 0;
    read_stage = read_calls = write_calls = handshake_calls = ack_count = retry_count = 0;
    net_recv_calls = rng_calls = 0; retry_buf = NULL; retry_len = 0;
}
static esp_err_t fetch(const char *addr, size_t *len) {
    unsigned char dataset[64]; memset(dataset, 0xa5, sizeof(dataset));
    esp_err_t err = meshcop_fetch_dataset(addr, 12345, "123456789", dataset, sizeof(dataset), len);
    assert(sockets_open == 0 && ssl_frees == 1 && conf_frees == 1);
    if (err == ESP_OK) {
        const unsigned char expected[] = {TLV_CHANNEL, 3, 0, 0, 15};
        assert(*len == sizeof(expected) && memcmp(dataset, expected, *len) == 0);
    } else {
        assert(*len == 0 && dataset[0] == 0xa5);
    }
    return err;
}
int main(void) {
    size_t len = 99;
    reset(NORMAL); assert(fetch("192.0.2.1", &len) == ESP_OK);
    assert(ack_count == 6 && retry_count == 2);
#if MBEDTLS_VERSION_MAJOR < 4
    assert(rng_calls == 1);
#else
    assert(rng_calls == 0);
#endif
    reset(NORMAL); assert(fetch("fe80::1%st1", &len) == ESP_OK);
    reset(HANDSHAKE_STALL); assert(fetch("192.0.2.1", &len) == ESP_ERR_TIMEOUT);
    assert(now_us == 30000000 && net_recv_calls > 0 && read_calls == 0);
    reset(WRITE_STALL); assert(fetch("192.0.2.1", &len) == ESP_ERR_TIMEOUT);
    assert(now_us == 30000000 && read_calls == 0);
    reset(READ_STALL); assert(fetch("192.0.2.1", &len) == ESP_ERR_TIMEOUT);
    assert(now_us == 5000000 && strstr(meshcop_last_error(), "petition") != NULL);
    reset(ACK_STORM); assert(fetch("192.0.2.1", &len) == ESP_ERR_TIMEOUT);
    assert(now_us == 5000000 && ack_count > 4);
    reset(SHARED_DEADLINE); assert(fetch("192.0.2.1", &len) == ESP_ERR_TIMEOUT);
    assert(now_us == 30000000 && read_stage == 1);
    reset(LATE_HANDSHAKE); assert(fetch("192.0.2.1", &len) == ESP_ERR_TIMEOUT);
    assert(read_calls == 0);
    reset(LATE_REPLY); assert(fetch("192.0.2.1", &len) == ESP_ERR_TIMEOUT);
    reset(BAD_PASSCODE); assert(fetch("192.0.2.1", &len) == ESP_FAIL);
    assert(strstr(meshcop_last_error(), "wrong passcode") != NULL && read_calls == 0);
    reset(CRYPTO_FAIL); assert(fetch("192.0.2.1", &len) == ESP_FAIL);
    assert(resolve_calls == 0 && handshake_calls == 0);
    assert(strstr(meshcop_last_error(), "secure connection") != NULL);
    reset(NONBLOCK_FAIL); assert(fetch("192.0.2.1", &len) == ESP_FAIL);
    assert(handshake_calls == 0);
    reset(NORMAL); assert(fetch("router.local", &len) == ESP_FAIL);
    assert(resolve_calls == 1 && handshake_calls == 0);
    reset(NORMAL);
    meshcop_transport_t expired = {.net = {.fd = 4}, .deadline_us = 0};
    unsigned char byte;
    assert(transport_recv(&expired, &byte, 1) == MBEDTLS_ERR_SSL_TIMEOUT);
    assert(transport_send(&expired, &byte, 1) == MBEDTLS_ERR_SSL_TIMEOUT);
    assert(net_recv_calls == 0);
    printf("PASS ePSKc Mbed TLS %d: bounded handshake/write/reply/ACK retries, shared deadline, "
           "EC-JPAKE configuration, crypto/transport failure cleanup, numeric IPs\n", MBEDTLS_VERSION_MAJOR);
    return 0;
}
