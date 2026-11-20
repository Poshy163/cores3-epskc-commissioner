#pragma once
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_TIMEOUT 0x107
#define MALLOC_CAP_INTERNAL 1
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
int64_t esp_timer_get_time(void);
size_t heap_caps_get_free_size(int cap);
void vTaskDelay(unsigned ticks);

#define AF_UNSPEC 0
#define AF_INET 2
#define AF_INET6 10
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
#define AI_NUMERICHOST 4
struct sockaddr { int family; };
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    unsigned ai_addrlen;
    struct sockaddr *ai_addr;
    struct addrinfo *ai_next;
};
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
void freeaddrinfo(struct addrinfo *);
int socket(int, int, int);
int connect(int, const struct sockaddr *, unsigned);

#define MBEDTLS_ERR_SSL_WANT_READ -0x6900
#define MBEDTLS_ERR_SSL_WANT_WRITE -0x6880
#define MBEDTLS_ERR_SSL_TIMEOUT -0x6800
#define MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE -0x6980
#define MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE -0x7780
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004e
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004c
#define MBEDTLS_ERR_NET_CONN_RESET -0x0050
#define MBEDTLS_ERR_NET_UNKNOWN_HOST -0x0052
#define MBEDTLS_ERR_NET_CONNECT_FAILED -0x0044
#define MBEDTLS_ERR_NET_SOCKET_FAILED -0x0042
#define MBEDTLS_SSL_IS_CLIENT 0
#define MBEDTLS_SSL_TRANSPORT_DATAGRAM 1
#define MBEDTLS_SSL_PRESET_DEFAULT 0
#define MBEDTLS_SSL_VERIFY_NONE 0
#define MBEDTLS_TLS_ECJPAKE_WITH_AES_128_CCM_8 0xc0ff
typedef struct { int fd; } mbedtls_net_context;
typedef struct { int unused; } mbedtls_ssl_context;
typedef struct { int unused; } mbedtls_ssl_config;
typedef int (*test_send_fn)(void *, const unsigned char *, size_t);
typedef int (*test_recv_fn)(void *, unsigned char *, size_t);
void mbedtls_net_init(mbedtls_net_context *);
void mbedtls_net_free(mbedtls_net_context *);
int mbedtls_net_set_nonblock(mbedtls_net_context *);
int mbedtls_net_send(void *, const unsigned char *, size_t);
int mbedtls_net_recv(void *, unsigned char *, size_t);
void mbedtls_ssl_init(mbedtls_ssl_context *);
void mbedtls_ssl_free(mbedtls_ssl_context *);
void mbedtls_ssl_config_init(mbedtls_ssl_config *);
void mbedtls_ssl_config_free(mbedtls_ssl_config *);
int mbedtls_ssl_config_defaults(mbedtls_ssl_config *, int, int, int);
void mbedtls_ssl_conf_ciphersuites(mbedtls_ssl_config *, const int *);
void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *, int);
void mbedtls_ssl_conf_handshake_timeout(mbedtls_ssl_config *, uint32_t, uint32_t);
int mbedtls_ssl_setup(mbedtls_ssl_context *, const mbedtls_ssl_config *);
int mbedtls_ssl_set_hs_ecjpake_password(mbedtls_ssl_context *, const unsigned char *, size_t);
void mbedtls_ssl_set_bio(mbedtls_ssl_context *, void *, test_send_fn, test_recv_fn,
                       int (*)(void *, unsigned char *, size_t, uint32_t));
void mbedtls_ssl_set_timer_cb(mbedtls_ssl_context *, void *,
                            void (*)(void *, uint32_t, uint32_t), int (*)(void *));
int mbedtls_ssl_handshake(mbedtls_ssl_context *);
int mbedtls_ssl_write(mbedtls_ssl_context *, const unsigned char *, size_t);
int mbedtls_ssl_read(mbedtls_ssl_context *, unsigned char *, size_t);
const char *mbedtls_ssl_get_ciphersuite(const mbedtls_ssl_context *);
int mbedtls_ssl_close_notify(mbedtls_ssl_context *);

/* Deliberately do not declare the removed API on the Mbed TLS 4 surface. */
#if MBEDTLS_VERSION_MAJOR < 4
typedef struct { int unused; } mbedtls_entropy_context;
typedef struct { int unused; } mbedtls_ctr_drbg_context;
void mbedtls_entropy_init(mbedtls_entropy_context *);
void mbedtls_entropy_free(mbedtls_entropy_context *);
void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *);
void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *);
int mbedtls_entropy_func(void *, unsigned char *, size_t);
int mbedtls_ctr_drbg_random(void *, unsigned char *, size_t);
int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *, test_recv_fn, void *,
                        const unsigned char *, size_t);
void mbedtls_ssl_conf_rng(mbedtls_ssl_config *, test_recv_fn, void *);
#else
typedef int psa_status_t;
#define PSA_SUCCESS 0
psa_status_t psa_crypto_init(void);
#endif
