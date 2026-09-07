#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool meshcop_coap_is_empty_ack(const uint8_t *buf, int len)
{
    return buf != NULL && len == 4 && buf[0] == 0x60 && buf[1] == 0x00;
}

static inline bool meshcop_coap_is_success(const uint8_t *buf, int len)
{
    if (buf == NULL || len < 4 || (buf[0] >> 6) != 1) {
        return false;
    }
    unsigned token_len = buf[0] & 0x0f;
    return token_len <= 8 && (size_t) len >= 4 + token_len && (buf[1] >> 5) == 2;
}

static inline const uint8_t *meshcop_coap_payload(const uint8_t *buf, size_t len,
                                                   size_t *out_len)
{
    if (buf == NULL || out_len == NULL || len < 4 || (buf[0] >> 6) != 1 ||
        (buf[0] & 0x0f) > 8) {
        return NULL;
    }
    size_t i = 4 + (buf[0] & 0x0f);
    if (i > len) {
        return NULL;
    }
    while (i < len && buf[i] != 0xFF) {
        uint8_t delta = (buf[i] >> 4) & 0x0f;
        uint8_t length = buf[i] & 0x0f;
        i++;
        if (delta == 15 || length == 15) {
            return NULL;
        }
        size_t delta_ext = delta == 13 ? 1 : delta == 14 ? 2 : 0;
        size_t length_ext = length == 13 ? 1 : length == 14 ? 2 : 0;
        if (delta_ext + length_ext > len - i) {
            return NULL;
        }
        i += delta_ext;
        size_t option_len = length;
        if (length == 13) {
            option_len = (size_t) buf[i] + 13;
        } else if (length == 14) {
            option_len = ((size_t) buf[i] << 8) | buf[i + 1];
            option_len += 269;
        }
        i += length_ext;
        if (option_len > len - i) {
            return NULL;
        }
        i += option_len;
    }
    if (i >= len || buf[i] != 0xFF) {
        return NULL;
    }
    i++;
    *out_len = len - i;
    return buf + i;
}

static inline bool meshcop_petition_was_accepted(const uint8_t *buf, size_t len)
{
    enum { kMeshcopStateTlv = 16, kMeshcopStateAccept = 1 };
    size_t payload_len = 0;
    const uint8_t *payload = meshcop_coap_payload(buf, len, &payload_len);
    size_t i = 0;
    while (payload != NULL && i + 2 <= payload_len) {
        unsigned type = payload[i++];
        size_t tlv_len = payload[i++];
        if (tlv_len == 255) {
            if (payload_len - i < 2) {
                return false;
            }
            tlv_len = ((size_t) payload[i] << 8) | payload[i + 1];
            i += 2;
        }
        if (tlv_len > payload_len - i) {
            return false;
        }
        if (type == kMeshcopStateTlv) {
            return tlv_len == 1 && payload[i] == kMeshcopStateAccept;
        }
        i += tlv_len;
    }
    return false;
}
