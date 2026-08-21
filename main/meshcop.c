/*
 * Thread 1.4 ePSKc external commissioner, ESP-IDF port.
 *
 * Ported from the PC proof-of-concept. The mbedTLS calls are identical; the two
 * things that differ on IDF are that MBEDTLS_TIMING_C is unavailable (so the
 * DTLS retransmission timers are implemented here on esp_timer) and that
 * secrets are masked by default when printing.
 */
#include "meshcop.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"

static const char *TAG = "meshcop";

#define URI_PETITION   "cp"
#define URI_ACTIVE_GET "ag"

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
static bool coap_is_empty_ack(const uint8_t *buf, int len)
{
    return len == 4 && buf[1] == 0x00;
}

/* Read until a message with an actual payload arrives, skipping empty ACKs. */
static int read_real_response(mbedtls_ssl_context *ssl, uint8_t *buf, size_t cap)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        int ret = mbedtls_ssl_read(ssl, buf, cap);
        if (ret <= 0) {
            return ret;
        }
        if (!coap_is_empty_ack(buf, ret)) {
            return ret;
        }
        ESP_LOGD(TAG, "empty ACK, waiting for separate response");
    }
    return -1;
}

static const uint8_t *coap_payload(const uint8_t *buf, size_t len, size_t *out_len)
{
    if (len < 4) {
        return NULL;
    }
    size_t i = 4 + (buf[0] & 0x0f);
    while (i < len && buf[i] != 0xFF) {
        uint8_t d = (buf[i] >> 4) & 0x0f, l = buf[i] & 0x0f;
        i++;
        if (d == 13) { i += 1; } else if (d == 14) { i += 2; }
        if (l == 13) { l = buf[i] + 13; i += 1; } else if (l == 14) { i += 2; }
        i += l;
    }
    if (i >= len || buf[i] != 0xFF) {
        return NULL;
    }
    i++;
    *out_len = len - i;
    return buf + i;
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
    int ret;
    s_last_error[0] = '\0';
    esp_err_t err = ESP_FAIL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    mbedtls_net_context fd;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    dtls_timer_t timer = { 0 };

    mbedtls_net_init(&fd);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    const char *pers = "epskc";
    if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char *) pers, strlen(pers))) != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed: -0x%04x", (unsigned) -ret);
        goto done;
    }

    ESP_LOGI(TAG, "connecting UDP %s:%s", addr, port_str);
    if ((ret = mbedtls_net_connect(&fd, addr, port_str, MBEDTLS_NET_PROTO_UDP)) != 0) {
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
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
        ESP_LOGE(TAG, "ssl_setup: -0x%04x", (unsigned) -ret);
        goto done;
    }
    if ((ret = mbedtls_ssl_set_hs_ecjpake_password(
             &ssl, (const unsigned char *) passcode, strlen(passcode))) != 0) {
        ESP_LOGE(TAG, "set_hs_ecjpake_password: -0x%04x", (unsigned) -ret);
        goto done;
    }

    mbedtls_ssl_set_bio(&ssl, &fd, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);
    mbedtls_ssl_set_timer_cb(&ssl, &timer, timer_set_delay, timer_get_delay);

    ESP_LOGI(TAG, "DTLS handshake (EC-JPAKE), internal heap free %u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
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
    }
    ESP_LOGI(TAG, "handshake OK: %s", mbedtls_ssl_get_ciphersuite(&ssl));

    {
        /* static: 1.25 KB of buffers off the console task's stack */
        static uint8_t out[256], in[1024];
        const char *cid = "esp32-epskc";
        uint8_t pl[48];
        size_t pn = 0;
        pl[pn++] = TLV_COMMISSIONER_ID;
        pl[pn++] = (uint8_t) strlen(cid);
        memcpy(pl + pn, cid, strlen(cid));
        pn += strlen(cid);

        size_t n = coap_post(out, URI_PETITION, pl, pn);
        if ((ret = mbedtls_ssl_write(&ssl, out, n)) <= 0) {
            ESP_LOGE(TAG, "petition write: -0x%04x", (unsigned) -ret);
            set_err("Petition send failed (-0x%04x)", ret);
            goto done;
        }
        ret = read_real_response(&ssl, in, sizeof(in));
        if (ret <= 0) {
            ESP_LOGE(TAG, "petition read failed: -0x%04x", (unsigned) -ret);
            set_err("No reply to petition (-0x%04x)", ret);
            goto done;
        }
        ESP_LOGI(TAG, "petition accepted (%d bytes)", ret);

        n = coap_post(out, URI_ACTIVE_GET, NULL, 0);
        if ((ret = mbedtls_ssl_write(&ssl, out, n)) <= 0) {
            ESP_LOGE(TAG, "active_get write: -0x%04x", (unsigned) -ret);
            set_err("Dataset request failed (-0x%04x)", ret);
            goto done;
        }
        ret = read_real_response(&ssl, in, sizeof(in));
        if (ret <= 0) {
            ESP_LOGE(TAG, "active_get read: -0x%04x", (unsigned) -ret);
            set_err("No dataset reply (-0x%04x)", ret);
            goto done;
        }

        size_t plen = 0;
        const uint8_t *p = coap_payload(in, (size_t) ret, &plen);
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
    mbedtls_net_free(&fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return err;
}
