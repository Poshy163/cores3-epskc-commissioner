/*
 * Thread 1.4 ePSKc external commissioner, ESP-IDF port.
 *
 * DTLS retransmission timers use esp_timer because MBEDTLS_TIMING_C is not
 * available on IDF. Socket I/O and retry loops share a bounded deadline, and
 * secrets are masked by default when printing.
 */
#include "meshcop.h"
#include "meshcop_codec.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/version.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#else
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#endif
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"

static const char *TAG = "meshcop";

#define URI_PETITION   "cp"
#define URI_ACTIVE_GET "ag"

/* An absent peer must not hold the application's single fetch worker forever.
 * The reply limit includes empty ACKs and WANT_READ/WRITE retries. */
#define FETCH_TIMEOUT_MS 30000
#define REPLY_TIMEOUT_MS 5000

typedef struct {
    mbedtls_net_context net;
    int64_t deadline_us;
} meshcop_transport_t;

static bool deadline_expired(int64_t deadline_us)
{
    return esp_timer_get_time() >= deadline_us;
}

static bool io_would_block(int ret)
{
    return ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE;
}

static int transport_send(void *ctx, const unsigned char *buf, size_t len)
{
    meshcop_transport_t *transport = ctx;
    if (deadline_expired(transport->deadline_us)) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    return mbedtls_net_send(&transport->net, buf, len);
}

static int transport_recv(void *ctx, unsigned char *buf, size_t len)
{
    meshcop_transport_t *transport = ctx;
    if (deadline_expired(transport->deadline_us)) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    return mbedtls_net_recv(&transport->net, buf, len);
}

/* Discovery supplies numeric IP addresses. AI_NUMERICHOST also accepts scoped
 * IPv6 literals and prevents a CLI typo from starting an unbounded DNS lookup.
 * UDP connect does not wait for a peer; all subsequent I/O is nonblocking. */
static int transport_connect(meshcop_transport_t *transport, const char *addr,
                             const char *port)
{
    struct addrinfo hints = { 0 };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICHOST;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(addr, port, &hints, &addresses) != 0) {
        return MBEDTLS_ERR_NET_UNKNOWN_HOST;
    }

    int ret = MBEDTLS_ERR_NET_CONNECT_FAILED;
    for (struct addrinfo *a = addresses; a != NULL; a = a->ai_next) {
        if (deadline_expired(transport->deadline_us)) {
            ret = MBEDTLS_ERR_SSL_TIMEOUT;
            break;
        }
        transport->net.fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (transport->net.fd < 0) {
            ret = MBEDTLS_ERR_NET_SOCKET_FAILED;
            continue;
        }
        ret = mbedtls_net_set_nonblock(&transport->net);
        if (ret == 0 && connect(transport->net.fd, a->ai_addr, a->ai_addrlen) == 0) {
            break;
        }
        if (ret == 0) {
            ret = MBEDTLS_ERR_NET_CONNECT_FAILED;
        }
        mbedtls_net_free(&transport->net);
    }
    freeaddrinfo(addresses);
    return ret;
}

static int handshake_until(mbedtls_ssl_context *ssl, int64_t deadline_us)
{
    for (;;) {
        if (deadline_expired(deadline_us)) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        int ret = mbedtls_ssl_handshake(ssl);
        if (!io_would_block(ret)) {
            return deadline_expired(deadline_us) ? MBEDTLS_ERR_SSL_TIMEOUT : ret;
        }
        vTaskDelay(1);
    }
}

static int write_until(mbedtls_ssl_context *ssl, const uint8_t *buf, size_t len,
                       int64_t deadline_us)
{
    for (;;) {
        if (deadline_expired(deadline_us)) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        int ret = mbedtls_ssl_write(ssl, buf, len);
        if (!io_would_block(ret)) {
            return deadline_expired(deadline_us) ? MBEDTLS_ERR_SSL_TIMEOUT : ret;
        }
        /* Mbed TLS requires the same buffer and length after WANT_WRITE. */
        vTaskDelay(1);
    }
}

const char *meshcop_tlv_name(unsigned type)
{
    switch (type) {
    case TLV_CHANNEL: return "Channel";
    case TLV_PANID: return "PAN ID";
    case TLV_EXT_PANID: return "Extended PAN ID";
    case TLV_NETWORK_NAME: return "Network Name";
    case TLV_PSKC: return "PSKc";
    case TLV_NETWORK_KEY: return "Network Key";
    case TLV_COMMISSIONER_ID: return "Commissioner ID";
    case TLV_COMMISSIONER_SESSION_ID: return "Commissioner Session ID";
    case TLV_SECURITY_POLICY: return "Security Policy";
    case TLV_ACTIVE_TIMESTAMP: return "Active Timestamp";
    case TLV_STATE: return "State";
    case TLV_CHANNEL_MASK: return "Channel Mask";
    case 7: return "Network Key Sequence";
    case 8: return "Mesh-Local Prefix";
    default: return "?";
    }
}

static bool tlv_is_secret(unsigned type)
{
    return type == TLV_NETWORK_KEY || type == TLV_PSKC;
}

/* ---- DTLS retransmission timers (MBEDTLS_TIMING_C is not built on IDF) ---- */

typedef struct {
    int64_t start_us;
    uint32_t int_ms;
    uint32_t fin_ms;
} dtls_timer_t;

static void timer_set_delay(void *ctx, uint32_t int_ms, uint32_t fin_ms)
{
    dtls_timer_t *t = (dtls_timer_t *) ctx;
    t->int_ms = int_ms;
    t->fin_ms = fin_ms;
    t->start_us = esp_timer_get_time();
}

static int timer_get_delay(void *ctx)
{
    const dtls_timer_t *t = (const dtls_timer_t *) ctx;
    if (t->fin_ms == 0) {
        return -1; /* cancelled */
    }
    int64_t elapsed_ms = (esp_timer_get_time() - t->start_us) / 1000;
    if (elapsed_ms >= t->fin_ms) {
        return 2;
    }
    if (elapsed_ms >= t->int_ms) {
        return 1;
    }
    return 0;
}

/* ---- CoAP ---- */

static uint16_t s_msgid = 0x2000;

static size_t coap_post(uint8_t *buf, const char *uri,
                        const uint8_t *payload, size_t payload_len)
{
    size_t n = 0;
    uint16_t mid = s_msgid++;

    buf[n++] = 0x40;                    /* ver 1, CON, TKL 0 */
    buf[n++] = 0x02;                    /* 0.02 POST */
    buf[n++] = (uint8_t) (mid >> 8);
    buf[n++] = (uint8_t) (mid & 0xff);

    buf[n++] = 0xB1;                    /* Uri-Path (11), len 1 */
    buf[n++] = 'c';

    size_t ulen = strlen(uri);
    buf[n++] = (uint8_t) ulen;          /* delta 0, len ulen */
    memcpy(buf + n, uri, ulen);
    n += ulen;

    if (payload && payload_len) {
        buf[n++] = 0xFF;
        memcpy(buf + n, payload, payload_len);
        n += payload_len;
    }
    return n;
}

/*
 * True for a CoAP empty ACK (code 0.00, no payload). ot-br-posix answers with
 * one of these and then sends the real response as a separate message, so a
 * naive read-once treats the next request's reply as belonging to this one.
 */
/* Read until a message with an actual payload arrives, skipping empty ACKs. */
static int read_real_response(mbedtls_ssl_context *ssl, uint8_t *buf, size_t cap,
                              int64_t fetch_deadline_us)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t) REPLY_TIMEOUT_MS * 1000;
    if (deadline_us > fetch_deadline_us) {
        deadline_us = fetch_deadline_us;
    }
    for (;;) {
        if (deadline_expired(deadline_us)) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        int ret = mbedtls_ssl_read(ssl, buf, cap);
        if (io_would_block(ret)) {
            vTaskDelay(1);
            continue;
        }
        if (ret <= 0) {
            return ret;
        }
        if (deadline_expired(deadline_us)) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        if (!meshcop_coap_is_empty_ack(buf, ret)) {
            return ret;
        }
        ESP_LOGD(TAG, "empty ACK, waiting for separate response");
        vTaskDelay(1);
    }
}

void meshcop_print_tlvs(const uint8_t *p, size_t len, bool reveal)
{
    size_t i = 0;
    while (i + 2 <= len) {
        unsigned t = p[i];
        size_t l = p[i + 1];
        i += 2;
        if (l == 255 && i + 2 <= len) {
            l = ((size_t) p[i] << 8) | p[i + 1];
            i += 2;
        }
        if (i + l > len) {
            printf("  !! truncated TLV %u\n", t);
            return;
        }
        printf("  [%2u] %-24s len=%-3u ", t, meshcop_tlv_name(t), (unsigned) l);
        if (t == TLV_NETWORK_NAME) {
            printf("\"%.*s\"", (int) l, p + i);
        } else if (tlv_is_secret(t) && !reveal) {
            printf("<hidden - use 'reveal' to show>");
        } else {
            for (size_t k = 0; k < l; k++) {
                printf("%02x", p[i + k]);
            }
        }
        printf("\n");
        i += l;
    }
}

void meshcop_summarize(const uint8_t *p, size_t len,
                       char *name, size_t name_cap, int *channel, uint16_t *panid)
{
    if (name && name_cap) {
        name[0] = '\0';
    }
    if (channel) {
        *channel = 0;
    }
    if (panid) {
        *panid = 0;
    }

    size_t i = 0;
    while (i + 2 <= len) {
        unsigned t = p[i];
        size_t l = p[i + 1];
        i += 2;
        if (l == 255 && i + 2 <= len) {
            l = ((size_t) p[i] << 8) | p[i + 1];
            i += 2;
        }
        if (i + l > len) {
            return;
        }
        if (t == TLV_NETWORK_NAME && name && name_cap) {
            size_t n = l < name_cap - 1 ? l : name_cap - 1;
            memcpy(name, p + i, n);
            name[n] = '\0';
        } else if (t == TLV_CHANNEL && l >= 3 && channel) {
            /* Channel TLV is {page, channel_hi, channel_lo} */
            *channel = ((int) p[i + 1] << 8) | p[i + 2];
        } else if (t == TLV_PANID && l >= 2 && panid) {
            *panid = ((uint16_t) p[i] << 8) | p[i + 1];
        }
        i += l;
    }
}

/* Last failure, in words the result screen can show. */
static char s_last_error[64] = "";

static void set_err(const char *fmt, int code)
{
    snprintf(s_last_error, sizeof(s_last_error), fmt, (unsigned) -code);
}

const char *meshcop_last_error(void)
{
    return s_last_error;
}

esp_err_t meshcop_fetch_dataset(const char *addr, uint16_t port, const char *passcode,
                                uint8_t *dataset, size_t dataset_cap, size_t *dataset_len)
{
    s_last_error[0] = '\0';
    if (addr == NULL || addr[0] == '\0' || port == 0 || passcode == NULL ||
        passcode[0] == '\0' || dataset == NULL || dataset_len == NULL || dataset_cap == 0) {
        snprintf(s_last_error, sizeof(s_last_error), "Invalid connection details");
        return ESP_ERR_INVALID_ARG;
    }
    int ret = 0;
    *dataset_len = 0;
    esp_err_t err = ESP_FAIL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    meshcop_transport_t transport = {
        .deadline_us = esp_timer_get_time() + (int64_t) FETCH_TIMEOUT_MS * 1000,
    };
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
#endif
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    dtls_timer_t timer = { 0 };

    mbedtls_net_init(&transport.net);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
#endif
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

#if MBEDTLS_VERSION_MAJOR >= 4
    /* Mbed TLS 4 uses the shared PSA RNG; the application no longer provides a
     * DRBG callback. Initialization is idempotent and failures must stop TLS. */
    if ((ret = psa_crypto_init()) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init: %d", ret);
        snprintf(s_last_error, sizeof(s_last_error), "Could not initialize secure connection");
        goto done;
    }
#else
    const char *pers = "epskc";
    if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char *) pers, strlen(pers))) != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed: -0x%04x", (unsigned) -ret);
        snprintf(s_last_error, sizeof(s_last_error), "Could not initialize secure connection");
        goto done;
    }
#endif

    ESP_LOGI(TAG, "connecting UDP %s:%s", addr, port_str);
    if ((ret = transport_connect(&transport, addr, port_str)) != 0) {
        ESP_LOGE(TAG, "net_connect: -0x%04x", (unsigned) -ret);
        set_err("Could not reach the router (-0x%04x)", ret);
        goto done;
    }

    if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        ESP_LOGE(TAG, "config_defaults: -0x%04x", (unsigned) -ret);
        goto done;
    }

    static const int ciphers[] = { MBEDTLS_TLS_ECJPAKE_WITH_AES_128_CCM_8, 0 };
    mbedtls_ssl_conf_ciphersuites(&conf, ciphers);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
#endif
    mbedtls_ssl_conf_handshake_timeout(&conf, 1000, 8000);

    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
        ESP_LOGE(TAG, "ssl_setup: -0x%04x", (unsigned) -ret);
        goto done;
    }
    if ((ret = mbedtls_ssl_set_hs_ecjpake_password(
             &ssl, (const unsigned char *) passcode, strlen(passcode))) != 0) {
        ESP_LOGE(TAG, "set_hs_ecjpake_password: -0x%04x", (unsigned) -ret);
        goto done;
    }

    mbedtls_ssl_set_bio(&ssl, &transport, transport_send, transport_recv, NULL);
    mbedtls_ssl_set_timer_cb(&ssl, &timer, timer_set_delay, timer_get_delay);

    ESP_LOGI(TAG, "DTLS handshake (EC-JPAKE), internal heap free %u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if ((ret = handshake_until(&ssl, transport.deadline_us)) != 0) {
        /* Distinguish "we could not send" from "the peer rejected us" --
         * reporting a transport failure as a bad passcode sends debugging
         * in entirely the wrong direction. */
        const char *why;
        switch (ret) {
        case MBEDTLS_ERR_NET_SEND_FAILED:
        case MBEDTLS_ERR_NET_RECV_FAILED:
        case MBEDTLS_ERR_NET_CONN_RESET:
            why = "router unreachable or out of buffers";
            break;
        case MBEDTLS_ERR_SSL_TIMEOUT:
            why = "timed out, router not responding";
            break;
        case MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE:
        case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE:
            why = "wrong passcode or key already used";
            break;
        default:
            why = "unexpected";
            break;
        }
        ESP_LOGE(TAG, "handshake FAILED: -0x%04x (%s); internal heap free %u",
                 (unsigned) -ret, why,
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        snprintf(s_last_error, sizeof(s_last_error), "Handshake failed: %s", why);
        goto done;
    }
    ESP_LOGI(TAG, "handshake OK: %s", mbedtls_ssl_get_ciphersuite(&ssl));

    {
        /* static: 1.25 KB of buffers off the console task's stack. Fetches are
         * serialized through the application's single worker task. */
        static uint8_t out[256], in[1024];
        const char *cid = "esp32-epskc";
        uint8_t pl[48];
        size_t pn = 0;
        pl[pn++] = TLV_COMMISSIONER_ID;
        pl[pn++] = (uint8_t) strlen(cid);
        memcpy(pl + pn, cid, strlen(cid));
        pn += strlen(cid);

        size_t n = coap_post(out, URI_PETITION, pl, pn);
        if ((ret = write_until(&ssl, out, n, transport.deadline_us)) <= 0) {
            ESP_LOGE(TAG, "petition write: -0x%04x", (unsigned) -ret);
            set_err("Petition send failed (-0x%04x)", ret);
            goto done;
        }
        ret = read_real_response(&ssl, in, sizeof(in), transport.deadline_us);
        if (ret <= 0) {
            ESP_LOGE(TAG, "petition read failed: -0x%04x", (unsigned) -ret);
            set_err("No reply to petition (-0x%04x)", ret);
            goto done;
        }
        if (!meshcop_coap_is_success(in, ret)) {
            ESP_LOGE(TAG, "petition rejected or malformed");
            snprintf(s_last_error, sizeof(s_last_error), "Router rejected petition");
            goto done;
        }
        /* CoAP success can still carry MeshCoP State Reject when another
         * commissioner is active. */
        if (!meshcop_petition_was_accepted(in, (size_t) ret)) {
            ESP_LOGE(TAG, "petition declined by border agent");
            snprintf(s_last_error, sizeof(s_last_error), "Router declined petition");
            goto done;
        }
        ESP_LOGI(TAG, "petition response accepted (%d bytes)", ret);

        n = coap_post(out, URI_ACTIVE_GET, NULL, 0);
        if ((ret = write_until(&ssl, out, n, transport.deadline_us)) <= 0) {
            ESP_LOGE(TAG, "active_get write: -0x%04x", (unsigned) -ret);
            set_err("Dataset request failed (-0x%04x)", ret);
            goto done;
        }
        ret = read_real_response(&ssl, in, sizeof(in), transport.deadline_us);
        if (ret <= 0) {
            ESP_LOGE(TAG, "active_get read: -0x%04x", (unsigned) -ret);
            set_err("No dataset reply (-0x%04x)", ret);
            goto done;
        }

        if (!meshcop_coap_is_success(in, ret)) {
            ESP_LOGE(TAG, "dataset request rejected or malformed");
            snprintf(s_last_error, sizeof(s_last_error), "Router rejected dataset request");
            goto done;
        }

        size_t plen = 0;
        const uint8_t *p = meshcop_coap_payload(in, (size_t) ret, &plen);
        if (!p || plen == 0) {
            ESP_LOGE(TAG, "no dataset payload in response");
            snprintf(s_last_error, sizeof(s_last_error), "Router sent no dataset");
            goto done;
        }
        if (plen > dataset_cap) {
            ESP_LOGE(TAG, "dataset too large (%u > %u)", (unsigned) plen, (unsigned) dataset_cap);
            snprintf(s_last_error, sizeof(s_last_error), "Dataset too large");
            goto done;
        }
        memcpy(dataset, p, plen);
        *dataset_len = plen;
        err = ESP_OK;
    }

    mbedtls_ssl_close_notify(&ssl);

done:
    if (ret == MBEDTLS_ERR_SSL_TIMEOUT) {
        err = ESP_ERR_TIMEOUT;
    }
    mbedtls_net_free(&transport.net);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
#endif
    return err;
}
