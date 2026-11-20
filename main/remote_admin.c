#include "remote_admin.h"

#include <stdint.h>
#include <string.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define REMOTE_ADMIN_NAMESPACE "cfg"
#define REMOTE_ADMIN_NVS_KEY "remote_admin"
#define KEY_BYTES 32
#define RECORD_VERSION 1
#define RECORD_SIZE 40
#define RECORD_KEY_OFFSET 4
#define RECORD_CRC_OFFSET 36

typedef struct {
    bool ready;
    bool enabled;
    bool configured;
    uint8_t key[KEY_BYTES];
} admin_state_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_mutation_mutex;
static admin_state_t s_state;

static admin_state_t snapshot(void)
{
    admin_state_t state;
    portENTER_CRITICAL(&s_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

static void publish(const admin_state_t *state)
{
    portENTER_CRITICAL(&s_lock);
    s_state = *state;
    portEXIT_CRITICAL(&s_lock);
}

static void close_access(void)
{
    portENTER_CRITICAL(&s_lock);
    s_state.enabled = false;
    portEXIT_CRITICAL(&s_lock);
}

/* Allocation and mutex waits stay outside the short snapshot critical section.
 * A racing first caller may allocate a spare mutex, which it then releases. */
static esp_err_t lock_mutations(SemaphoreHandle_t *out)
{
    portENTER_CRITICAL(&s_lock);
    SemaphoreHandle_t mutex = s_mutation_mutex;
    portEXIT_CRITICAL(&s_lock);
    if (mutex == NULL) {
        SemaphoreHandle_t created = xSemaphoreCreateMutex();
        if (created == NULL) {
            close_access();
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_lock);
        if (s_mutation_mutex == NULL) {
            s_mutation_mutex = created;
            created = NULL;
        }
        mutex = s_mutation_mutex;
        portEXIT_CRITICAL(&s_lock);
        if (created != NULL) {
            vSemaphoreDelete(created);
        }
    }
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        close_access();
        return ESP_ERR_INVALID_STATE;
    }
    *out = mutex;
    return ESP_OK;
}

static bool valid_key(const uint8_t *key)
{
    uint8_t nonzero = 0;
    for (size_t i = 0; i < KEY_BYTES; ++i) {
        nonzero |= key[i];
    }
    return nonzero != 0;
}

/* Detect malformed records independently of NVS's storage CRC. This checksum
 * detects corruption; the random key and Bearer comparison provide access
 * control. The record has an explicit byte layout, without struct padding. */
static uint32_t record_crc(const uint8_t *record)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < RECORD_CRC_OFFSET; ++i) {
        crc ^= record[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void encode_record(const admin_state_t *state, uint8_t record[RECORD_SIZE])
{
    record[0] = 'R';
    record[1] = 'A';
    record[2] = RECORD_VERSION;
    record[3] = state->enabled ? 1 : 0;
    memcpy(record + RECORD_KEY_OFFSET, state->key, KEY_BYTES);
    uint32_t crc = record_crc(record);
    for (unsigned i = 0; i < 4; ++i) {
        record[RECORD_CRC_OFFSET + i] = (uint8_t)(crc >> (8 * i));
    }
}

static bool decode_record(const uint8_t record[RECORD_SIZE], admin_state_t *state)
{
    if (record[0] != 'R' || record[1] != 'A' || record[2] != RECORD_VERSION ||
        record[3] > 1 || !valid_key(record + RECORD_KEY_OFFSET)) {
        return false;
    }
    uint32_t saved_crc = 0;
    for (unsigned i = 0; i < 4; ++i) {
        saved_crc |= (uint32_t)record[RECORD_CRC_OFFSET + i] << (8 * i);
    }
    if (saved_crc != record_crc(record)) {
        return false;
    }
    state->ready = true;
    state->enabled = record[3] != 0;
    state->configured = true;
    memcpy(state->key, record + RECORD_KEY_OFFSET, KEY_BYTES);
    return true;
}

/* The version, enabled flag and key are committed as one NVS value. Nothing
 * from the candidate becomes visible to readers before a successful commit. */
static esp_err_t persist(const admin_state_t *state)
{
    uint8_t record[RECORD_SIZE];
    encode_record(state, record);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(REMOTE_ADMIN_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, REMOTE_ADMIN_NVS_KEY, record, sizeof(record));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        close_access();
    }
    return err;
}

esp_err_t remote_admin_init(void)
{
    SemaphoreHandle_t mutex;
    esp_err_t err = lock_mutations(&mutex);
    if (err != ESP_OK) {
        return err;
    }
    admin_state_t state = {0};
    publish(&state);
    nvs_handle_t handle;
    err = nvs_open(REMOTE_ADMIN_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
        state.ready = true;
    } else if (err == ESP_OK) {
        uint8_t record[RECORD_SIZE];
        size_t size = sizeof(record);
        err = nvs_get_blob(handle, REMOTE_ADMIN_NVS_KEY, record, &size);
        nvs_close(handle);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
            state.ready = true;
        } else if (err == ESP_OK &&
                   (size != sizeof(record) || !decode_record(record, &state))) {
            err = ESP_ERR_INVALID_STATE;
        }
    }
    if (err == ESP_OK) {
        publish(&state);
    }
    xSemaphoreGive(mutex);
    return err;
}

bool remote_admin_enabled(void)
{
    admin_state_t state = snapshot();
    return state.ready && state.configured && state.enabled;
}

bool remote_admin_key_configured(void)
{
    admin_state_t state = snapshot();
    return state.ready && state.configured;
}

esp_err_t remote_admin_set_enabled(bool enabled)
{
    SemaphoreHandle_t mutex;
    esp_err_t err = lock_mutations(&mutex);
    if (err != ESP_OK) {
        return err;
    }
    admin_state_t state = snapshot();
    if (!state.ready) {
        err = ESP_ERR_INVALID_STATE;
    } else if (state.configured || enabled) {
        /* Also persist repeated requests: a prior failed write closed access
         * in RAM, so a subsequent explicit disable must still reach NVS. */
        if (enabled && !state.configured) {
            esp_fill_random(state.key, sizeof(state.key));
            state.configured = valid_key(state.key);
        }
        if (!state.configured) {
            err = ESP_FAIL;
        } else {
            state.enabled = enabled;
            err = persist(&state);
            if (err == ESP_OK) {
                publish(&state);
            }
        }
    }
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t remote_admin_rotate_key(void)
{
    SemaphoreHandle_t mutex;
    esp_err_t err = lock_mutations(&mutex);
    if (err != ESP_OK) {
        return err;
    }
    admin_state_t state = snapshot();
    if (!state.ready || !state.configured) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        uint8_t previous[KEY_BYTES];
        memcpy(previous, state.key, sizeof(previous));
        esp_fill_random(state.key, sizeof(state.key));
        if (!valid_key(state.key) || memcmp(previous, state.key, sizeof(previous)) == 0) {
            err = ESP_FAIL;
        } else {
            err = persist(&state);
            if (err == ESP_OK) {
                publish(&state);
            }
        }
    }
    xSemaphoreGive(mutex);
    return err;
}

static void encode_token(const uint8_t key[KEY_BYTES], char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < KEY_BYTES; ++i) {
        out[2 * i] = hex[key[i] >> 4];
        out[2 * i + 1] = hex[key[i] & 0xf];
    }
    out[REMOTE_ADMIN_TOKEN_LENGTH] = '\0';
}

bool remote_admin_get_token(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (cap < REMOTE_ADMIN_TOKEN_BUFFER_SIZE) {
        return false;
    }
    admin_state_t state = snapshot();
    if (!state.ready || !state.configured) {
        return false;
    }
    encode_token(state.key, out);
    return true;
}

bool remote_admin_authorize(const char *authorization)
{
    admin_state_t state = snapshot();
    if (!state.ready || !state.configured || !state.enabled || authorization == NULL) {
        return false;
    }
    enum { PREFIX_SIZE = 7, HEADER_SIZE = PREFIX_SIZE + REMOTE_ADMIN_TOKEN_LENGTH };
    /* Bound inspection to the one accepted header length. */
    for (size_t i = 0; i < HEADER_SIZE; ++i) {
        if (authorization[i] == '\0') {
            return false;
        }
    }
    if (authorization[HEADER_SIZE] != '\0' || memcmp(authorization, "Bearer ", PREFIX_SIZE) != 0) {
        return false;
    }
    char expected[REMOTE_ADMIN_TOKEN_BUFFER_SIZE];
    encode_token(state.key, expected);
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < REMOTE_ADMIN_TOKEN_LENGTH; ++i) {
        difference |= (uint8_t)authorization[PREFIX_SIZE + i] ^ (uint8_t)expected[i];
    }
    return difference == 0;
}
