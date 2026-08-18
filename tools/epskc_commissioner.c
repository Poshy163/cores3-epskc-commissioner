/*
 * Thread 1.4 ePSKc external commissioner - proof of concept.
 *
 * Connects to a Border Agent's ephemeral-key endpoint over DTLS 1.2 using
 * EC-JPAKE (password = the 9-digit Thread Administration Passcode), then runs
 * the MeshCoP exchange to retrieve the Active Operational Dataset.
 *
 *   usage: epskc_commissioner <addr> <port> <passcode>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/timing.h"

/* MeshCoP CoAP URIs (ot-commissioner src/library/uri.hpp) */
#define URI_PETITION   "cp"
#define URI_ACTIVE_GET "ag"

/* MeshCoP TLV types (ot-commissioner src/library/tlv.hpp) */
enum {
    TLV_CHANNEL = 0,
    TLV_PANID = 1,
    TLV_EXT_PANID = 2,
    TLV_NETWORK_NAME = 3,
    TLV_NETWORK_KEY = 5,
    TLV_COMMISSIONER_ID = 10,
    TLV_COMMISSIONER_SESSION_ID = 11,
    TLV_ACTIVE_TIMESTAMP = 14,
    TLV_STATE = 16,
};

static const char *tlv_name(unsigned t)
{
    switch (t) {
    case TLV_CHANNEL: return "Channel";
    case TLV_PANID: return "PAN ID";
    case TLV_EXT_PANID: return "Extended PAN ID";
    case TLV_NETWORK_NAME: return "Network Name";
    case TLV_NETWORK_KEY: return "Network Key";
    case TLV_COMMISSIONER_ID: return "Commissioner ID";
    case TLV_COMMISSIONER_SESSION_ID: return "Commissioner Session ID";
    case TLV_ACTIVE_TIMESTAMP: return "Active Timestamp";
    case TLV_STATE: return "State";
    case 4: return "PSKc";
    case 7: return "Network Key Seq";
    case 8: return "Mesh-Local ULA";
    case 12: return "Security Policy";
    case 13: return "Get";
    case 51: return "Channel Mask";
    default: return "?";
    }
}

static uint16_t g_msgid = 0x1000;

/*
 * Build a CoAP CON POST to /c/<uri> with an optional payload.
 * Uri-Path is option 11, emitted twice: "c" then <uri>.
 */
static size_t coap_post(uint8_t *buf, const char *uri,
                        const uint8_t *payload, size_t payload_len)
{
    size_t n = 0;
    uint16_t mid = g_msgid++;

    buf[n++] = 0x40;                 /* ver=1, type=CON, TKL=0 */
    buf[n++] = 0x02;                 /* code 0.02 POST */
    buf[n++] = (uint8_t) (mid >> 8);
    buf[n++] = (uint8_t) (mid & 0xff);

    buf[n++] = 0xB1;                 /* opt delta 11 (Uri-Path), len 1 */
    buf[n++] = 'c';

    size_t ulen = strlen(uri);
    buf[n++] = (uint8_t) (0x00 | ulen); /* delta 0, len ulen */
    memcpy(buf + n, uri, ulen);
    n += ulen;

    if (payload && payload_len) {
        buf[n++] = 0xFF;             /* payload marker */
        memcpy(buf + n, payload, payload_len);
        n += payload_len;
    }
    return n;
}

static void hexdump(const char *label, const uint8_t *p, size_t len)
{
    printf("  %s (%zu bytes): ", label, len);
    for (size_t i = 0; i < len; i++) {
        printf("%02x", p[i]);
    }
    printf("\n");
}

/*
 * ot-br-posix answers with a CoAP empty ACK (code 0.00) first, then sends the
 * real response as a separate message. The ESP border agent piggybacks its
 * response instead, so a naive read-once looks correct against ESP and
 * silently shifts every reply by one against ot-br-posix -- the petition
 * response gets mistaken for the dataset. Skip empty ACKs until a real
 * message arrives. (Ported from epskc-fw/main/meshcop.c.)
 */
static int coap_is_empty_ack(const uint8_t *buf, int len)
{
    return len == 4 && buf[1] == 0x00;
}

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
        printf("  (empty ACK, waiting for separate response)\n");
    }
    return -1;
}

/* Walk past the CoAP header/options and return the payload, or NULL. */
static const uint8_t *coap_payload(const uint8_t *buf, size_t len, size_t *out_len)
{
    if (len < 4) {
        return NULL;
    }
    printf("  CoAP code %u.%02u\n", (buf[1] >> 5) & 0x7, buf[1] & 0x1f);

    size_t i = 4 + (buf[0] & 0x0f); /* skip token */
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

static void parse_tlvs(const uint8_t *p, size_t len)
{
    size_t i = 0;
    while (i + 2 <= len) {
        unsigned t = p[i];
        size_t l = p[i + 1];
        i += 2;
        if (l == 255 && i + 2 <= len) { /* extended length */
            l = ((size_t) p[i] << 8) | p[i + 1];
            i += 2;
        }
        if (i + l > len) {
            printf("  !! truncated TLV %u (want %zu, have %zu)\n", t, l, len - i);
            break;
        }
        printf("  [%2u] %-24s len=%-3zu ", t, tlv_name(t), l);
        if (t == TLV_NETWORK_NAME) {
            printf("\"%.*s\"", (int) l, p + i);
        } else {
            for (size_t k = 0; k < l; k++) {
                printf("%02x", p[i + k]);
            }
        }
        printf("\n");
        i += l;
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <addr> <port> <passcode>\n", argv[0]);
        return 2;
    }
    const char *addr = argv[1], *port = argv[2], *pw = argv[3];

    int ret;
    mbedtls_net_context fd;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_timing_delay_context timer;

    mbedtls_net_init(&fd);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    const char *pers = "epskc";
    if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char *) pers, strlen(pers))) != 0) {
        printf("!! ctr_drbg_seed: -0x%04x\n", (unsigned) -ret);
        return 1;
    }

    printf("== connecting UDP %s:%s ==\n", addr, port);
    if ((ret = mbedtls_net_connect(&fd, addr, port, MBEDTLS_NET_PROTO_UDP)) != 0) {
        printf("!! net_connect: -0x%04x\n", (unsigned) -ret);
        return 1;
    }

    if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        printf("!! config_defaults: -0x%04x\n", (unsigned) -ret);
        return 1;
    }

    static const int ciphers[] = { MBEDTLS_TLS_ECJPAKE_WITH_AES_128_CCM_8, 0 };
    mbedtls_ssl_conf_ciphersuites(&conf, ciphers);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
        printf("!! ssl_setup: -0x%04x\n", (unsigned) -ret);
        return 1;
    }
    if ((ret = mbedtls_ssl_set_hs_ecjpake_password(
             &ssl, (const unsigned char *) pw, strlen(pw))) != 0) {
        printf("!! set_hs_ecjpake_password: -0x%04x\n", (unsigned) -ret);
        return 1;
    }

    mbedtls_ssl_set_bio(&ssl, &fd, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);
    mbedtls_ssl_set_timer_cb(&ssl, &timer, mbedtls_timing_set_delay,
                             mbedtls_timing_get_delay);

    printf("== DTLS handshake (EC-JPAKE) ==\n");
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            printf("!! handshake FAILED: -0x%04x\n", (unsigned) -ret);
            return 1;
        }
    }
    printf("   OK  ciphersuite=%s\n\n", mbedtls_ssl_get_ciphersuite(&ssl));

    uint8_t out[512], in[1024];

    /* --- 1. Commissioner Petition: POST /c/cp { Commissioner ID } --- */
    const char *cid = "claude-epskc-poc";
    uint8_t pl[64];
    size_t pn = 0;
    pl[pn++] = TLV_COMMISSIONER_ID;
    pl[pn++] = (uint8_t) strlen(cid);
    memcpy(pl + pn, cid, strlen(cid));
    pn += strlen(cid);

    size_t n = coap_post(out, URI_PETITION, pl, pn);
    printf("== POST /c/%s (commissioner petition) ==\n", URI_PETITION);
    hexdump("tx", out, n);
    if ((ret = mbedtls_ssl_write(&ssl, out, n)) <= 0) {
        printf("!! write: -0x%04x\n", (unsigned) -ret);
        return 1;
    }

    ret = read_real_response(&ssl, in, sizeof(in));
    if (ret <= 0) {
        printf("!! read: -0x%04x\n\n", (unsigned) -ret);
    } else {
        hexdump("rx", in, (size_t) ret);
        size_t plen = 0;
        const uint8_t *p = coap_payload(in, (size_t) ret, &plen);
        if (p) {
            parse_tlvs(p, plen);
        } else {
            printf("  (no payload)\n");
        }
        printf("\n");
    }

    /* --- 2. MGMT_ACTIVE_GET: POST /c/ag, empty payload = all TLVs --- */
    n = coap_post(out, URI_ACTIVE_GET, NULL, 0);
    printf("== POST /c/%s (MGMT_ACTIVE_GET) ==\n", URI_ACTIVE_GET);
    hexdump("tx", out, n);
    if ((ret = mbedtls_ssl_write(&ssl, out, n)) <= 0) {
        printf("!! write: -0x%04x\n", (unsigned) -ret);
        return 1;
    }

    ret = read_real_response(&ssl, in, sizeof(in));
    if (ret <= 0) {
        printf("!! read: -0x%04x\n", (unsigned) -ret);
    } else {
        hexdump("rx", in, (size_t) ret);
        size_t plen = 0;
        const uint8_t *p = coap_payload(in, (size_t) ret, &plen);
        if (p) {
            printf("  --- ACTIVE OPERATIONAL DATASET ---\n");
            parse_tlvs(p, plen);
        } else {
            printf("  (no payload)\n");
        }
    }

    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return 0;
}
