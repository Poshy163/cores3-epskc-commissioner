#pragma once

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Small, allocation-free wire helpers kept separate so their edge cases can
 * be exercised by a host compiler as well as the firmware build. */
static inline bool otbr_rest_terminate_body(char *buf, size_t length, size_t capacity)
{
    if (buf == NULL || length >= capacity || memchr(buf, '\0', length) != NULL) {
        return false;
    }
    buf[length] = '\0';
    return true;
}

static inline bool otbr_rest_quote_json_string(const char *value, char *buf, size_t size)
{
    if (value == NULL || buf == NULL || size < 3) {
        return false;
    }
    char *out = buf;
    const char *const end = buf + size - 1;
    *out++ = '"';
    for (const unsigned char *p = (const unsigned char *) value; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            if (end - out < 2) {
                return false;
            }
            *out++ = '\\';
            *out++ = (char) *p;
        } else if (*p < 0x20) {
            if (end - out < 6) {
                return false;
            }
            static const char hex[] = "0123456789abcdef";
            *out++ = '\\';
            *out++ = 'u';
            *out++ = '0';
            *out++ = '0';
            *out++ = hex[*p >> 4];
            *out++ = hex[*p & 0x0f];
        } else {
            if (out == end) {
                return false;
            }
            *out++ = (char) *p;
        }
    }
    if (out == end) {
        return false;
    }
    *out++ = '"';
    *out = '\0';
    return true;
}

static inline char *otbr_rest_parse_dataset_hex_body(char *body)
{
    unsigned char *start = (unsigned char *) body;
    while (isspace(*start)) {
        start++;
    }
    if (*start != '"') {
        return (char *) start;
    }

    char *payload = (char *) ++start;
    char *closing_quote = strchr(payload, '"');
    if (closing_quote == NULL) {
        return NULL;
    }
    char *tail = closing_quote + 1;
    while (isspace((unsigned char) *tail)) {
        tail++;
    }
    char *escape = strchr(payload, '\\');
    if (*tail != '\0' || (escape != NULL && escape < closing_quote)) {
        return NULL;
    }
    *closing_quote = '\0';
    return payload;
}

static inline bool otbr_rest_parse_toggle(const char *body, bool *on)
{
    const unsigned char *p = (const unsigned char *) body;
    while (isspace(*p)) {
        p++;
    }
    const bool quoted = *p == '"';
    if (quoted) {
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
    if (quoted && *p++ != '"') {
        return false;
    }
    while (isspace(*p)) {
        p++;
    }
    return *p == '\0';
}

static inline bool otbr_rest_parse_epskc_options(const char *body, long *lifetime, long *port)
{
    const char *p = body;
    while (isspace((unsigned char) *p)) {
        p++;
    }
    if (*p == '\0') {
        return true;
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

        const char *number_end = p;
        if (*number_end == '-') {
            number_end++;
        }
        if (*number_end == '0') {
            number_end++;
            if (isdigit((unsigned char) *number_end)) {
                return false;
            }
        } else if (isdigit((unsigned char) *number_end)) {
            do {
                number_end++;
            } while (isdigit((unsigned char) *number_end));
        } else {
            return false;
        }

        errno = 0;
        char *end = NULL;
        long value = strtol(p, &end, 10);
        if (end != number_end || errno == ERANGE) {
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
