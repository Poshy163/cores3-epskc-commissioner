#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_ADMIN_TOKEN_LENGTH 64
#define REMOTE_ADMIN_TOKEN_BUFFER_SIZE (REMOTE_ADMIN_TOKEN_LENGTH + 1)

/* Load the single versioned record from NVS namespace "cfg". No record means
 * disabled with no key. Invalid records and read failures leave access closed
 * and return an error; a successful init is required before mutations. */
esp_err_t remote_admin_init(void);
bool remote_admin_enabled(void);
bool remote_admin_key_configured(void);

/* First explicit enable creates the key. Disable preserves it. Persistence
 * errors close access in RAM; they never publish an uncommitted new key. */
esp_err_t remote_admin_set_enabled(bool enabled);

/* Requires a configured key, and preserves the current enabled state. */
esp_err_t remote_admin_rotate_key(void);

/* Local use only: 64 lowercase hex characters plus NUL. On failure, clears
 * out[0] when possible. Neither this module nor its errors log the token. */
bool remote_admin_get_token(char *out, size_t cap);

/* Accepts only exact "Bearer " followed by the current 64-character token.
 * Disabled access always rejects. Token comparison takes constant time. */
bool remote_admin_authorize(const char *authorization);

#ifdef __cplusplus
}
#endif
