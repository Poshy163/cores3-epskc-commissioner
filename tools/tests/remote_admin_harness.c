/* Zig's optimized host mode may define NDEBUG; checks must always execute. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote_admin.c"

/* Independent wire fixture: key 01..20, CRC32 generated with Python zlib. */
static const uint8_t saved_enabled[40] = {
    0x52, 0x41, 0x01, 0x01, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
    0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c,
    0x1d, 0x1e, 0x1f, 0x20, 0xf8, 0x82, 0x1a, 0xee
};
static const char saved_token[] =
    "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";

static bool namespace_exists, blob_exists, pending_exists;
static uint8_t stored_blob[80], pending_blob[80];
static size_t stored_size, pending_size;
static int open_error, get_error, set_error, commit_error;
static int critical_depth, creates, deletes, opens, closes, sets, commits, random_calls;
static bool allocation_fails, take_fails, creation_race;
static uint8_t random_sequence;
static int random_mode;
static bool observe_writes, expected_enabled, expected_configured;
static char expected_token[REMOTE_ADMIN_TOKEN_BUFFER_SIZE];

void test_enter_critical(portMUX_TYPE *lock)
{
    assert(lock == &s_lock && critical_depth == 0);
    ++critical_depth;
}

void test_exit_critical(portMUX_TYPE *lock)
{
    assert(lock == &s_lock && critical_depth == 1);
    --critical_depth;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    assert(critical_depth == 0);
    ++creates;
    if (allocation_fails) {
        return NULL;
    }
    SemaphoreHandle_t mutex = calloc(1, sizeof(*mutex));
    assert(mutex);
    if (creation_race) {
        creation_race = false;
        /* A second caller finishes creation before the first publishes. */
        assert(remote_admin_init() == ESP_OK);
    }
    return mutex;
}

int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned delay)
{
    assert(critical_depth == 0 && mutex && !mutex->held && delay == portMAX_DELAY);
    if (take_fails) {
        return 0;
    }
    mutex->held = true;
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(critical_depth == 0 && mutex && mutex->held);
    mutex->held = false;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    assert(critical_depth == 0 && mutex && !mutex->held);
    ++deletes;
    free(mutex);
}

static void flash_context(void)
{
    assert(critical_depth == 0 && s_mutation_mutex && s_mutation_mutex->held);
}

static void check_unpublished(void)
{
    flash_context();
    if (!observe_writes) {
        return;
    }
    assert(remote_admin_enabled() == expected_enabled);
    assert(remote_admin_key_configured() == expected_configured);
    char token[REMOTE_ADMIN_TOKEN_BUFFER_SIZE] = "sentinel";
    assert(remote_admin_get_token(token, sizeof(token)) == expected_configured);
    assert(strcmp(token, expected_configured ? expected_token : "") == 0);
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle)
{
    flash_context();
    assert(strcmp(ns, "cfg") == 0 && (mode == NVS_READONLY || mode == NVS_READWRITE));
    ++opens;
    if (open_error) {
        return open_error;
    }
    if (!namespace_exists && mode == NVS_READONLY) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    namespace_exists = true;
    *handle = 123;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    flash_context();
    assert(handle == 123 && strcmp(key, "remote_admin") == 0 && out && size);
    if (get_error) {
        return get_error;
    }
    if (!blob_exists) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*size < stored_size) {
        *size = stored_size;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, stored_blob, stored_size);
    *size = stored_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    check_unpublished();
    assert(handle == 123 && strcmp(key, "remote_admin") == 0 && size == 40);
    ++sets;
    if (set_error) {
        return set_error;
    }
    memcpy(pending_blob, data, size);
    pending_size = size;
    pending_exists = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    check_unpublished();
    assert(handle == 123 && pending_exists);
    ++commits;
    if (commit_error) {
        return commit_error;
    }
    memcpy(stored_blob, pending_blob, pending_size);
    stored_size = pending_size;
    blob_exists = true;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    flash_context();
    assert(handle == 123);
    ++closes;
    pending_exists = false;
}

void esp_fill_random(void *out, size_t size)
{
    flash_context();
    assert(size == 32);
    ++random_calls;
    if (random_mode == 1) {
        memset(out, 0, size);
    } else if (random_mode == 2) {
        memcpy(out, s_state.key, size);
    } else {
        for (size_t i = 0; i < size; ++i) {
            ((uint8_t *)out)[i] = ++random_sequence;
        }
    }
}

static void reboot(void)
{
    if (s_mutation_mutex) {
        vSemaphoreDelete(s_mutation_mutex);
    }
    s_mutation_mutex = NULL;
    memset(&s_state, 0, sizeof(s_state));
    assert(critical_depth == 0);
    open_error = get_error = set_error = commit_error = 0;
    allocation_fails = take_fails = creation_race = observe_writes = false;
    pending_exists = false;
    random_mode = 0;
}

static void reset(void)
{
    reboot();
    namespace_exists = blob_exists = false;
    stored_size = 0;
    random_sequence = 0;
    creates = deletes = opens = closes = sets = commits = random_calls = 0;
}

static void fixture(void)
{
    reset();
    namespace_exists = blob_exists = true;
    memcpy(stored_blob, saved_enabled, sizeof(saved_enabled));
    stored_size = sizeof(saved_enabled);
}

static void header_for(const char *token, char *header)
{
    memcpy(header, "Bearer ", 7);
    memcpy(header + 7, token, REMOTE_ADMIN_TOKEN_BUFFER_SIZE);
}

static void assert_closed(bool configured)
{
    assert(!remote_admin_enabled());
    assert(remote_admin_key_configured() == configured);
    char header[72];
    header_for(saved_token, header);
    assert(!remote_admin_authorize(header));
    char token[65] = "sentinel";
    assert(remote_admin_get_token(token, sizeof(token)) == configured);
    if (!configured) {
        assert(token[0] == '\0');
    }
}

static void watch_current(void)
{
    expected_enabled = remote_admin_enabled();
    expected_configured = remote_admin_key_configured();
    (void)remote_admin_get_token(expected_token, sizeof(expected_token));
    observe_writes = true;
}

static void test_defaults_and_persistence(void)
{
    reset();
    assert_closed(false);
    assert(remote_admin_set_enabled(true) == ESP_ERR_INVALID_STATE);
    assert(remote_admin_rotate_key() == ESP_ERR_INVALID_STATE);
    assert(random_calls == 0 && opens == 0);
    assert(remote_admin_init() == ESP_OK);
    assert_closed(false);
    assert(!namespace_exists && random_calls == 0 && sets == 0);
    assert(remote_admin_set_enabled(false) == ESP_OK && sets == 0);
    assert(remote_admin_rotate_key() == ESP_ERR_INVALID_STATE);
    watch_current();
    assert(remote_admin_set_enabled(true) == ESP_OK);
    observe_writes = false;
    assert(remote_admin_enabled() && remote_admin_key_configured());
    assert(random_calls == 1 && sets == 1 && commits == 1);
    assert(stored_size == sizeof(saved_enabled));
    assert(memcmp(stored_blob, saved_enabled, stored_size) == 0);
    char token[65], header[72];
    assert(remote_admin_get_token(token, sizeof(token)) && strcmp(token, saved_token) == 0);
    header_for(token, header);
    assert(remote_admin_authorize(header));
    assert(remote_admin_set_enabled(true) == ESP_OK && random_calls == 1);
    reboot();
    assert(remote_admin_init() == ESP_OK && remote_admin_enabled());
    assert(random_calls == 1 && remote_admin_authorize(header));
    watch_current();
    assert(remote_admin_set_enabled(false) == ESP_OK);
    observe_writes = false;
    assert_closed(true);
    reboot();
    assert(remote_admin_init() == ESP_OK);
    assert_closed(true);
    assert(remote_admin_get_token(token, sizeof(token)) && strcmp(token, saved_token) == 0);
    assert(remote_admin_set_enabled(true) == ESP_OK && remote_admin_authorize(header));
    /* settings_erase erases cfg; no replacement key is silently generated. */
    blob_exists = false;
    reboot();
    assert(remote_admin_init() == ESP_OK);
    assert_closed(false);
    puts("PASS remote admin: explicit enable, atomic persistence, disabled/reboot/reset defaults");
}

static void test_authorization(void)
{
    fixture();
    assert(remote_admin_init() == ESP_OK);
    char token[70], header[160], candidate[160];
    memset(token, '!', sizeof(token));
    assert(!remote_admin_get_token(NULL, 65));
    assert(!remote_admin_get_token(token, 0) && token[0] == '!');
    for (size_t cap = 1; cap < 65; ++cap) {
        token[0] = '!';
        assert(!remote_admin_get_token(token, cap) && token[0] == '\0');
    }
    assert(remote_admin_get_token(token, sizeof(token)) && token[65] == '!');
    assert(strcmp(token, saved_token) == 0);
    header_for(token, header);
    assert(remote_admin_authorize(header) && !remote_admin_authorize(NULL));
    for (size_t length = 0; length < 71; ++length) {
        memcpy(candidate, header, 72);
        candidate[length] = '\0';
        assert(!remote_admin_authorize(candidate));
    }
    for (size_t i = 0; i < 64; ++i) {
        memcpy(candidate, header, 72);
        candidate[7 + i] = candidate[7 + i] == '0' ? '1' : '0';
        assert(!remote_admin_authorize(candidate));
    }
    const char *bad[] = {"", "Bearer", "Bearer ", "Basic test", saved_token,
                         "?token=test", "token=test", "Cookie: token=test"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!remote_admin_authorize(bad[i]));
    }
    memcpy(candidate, header, 72); candidate[0] = 'b';
    assert(!remote_admin_authorize(candidate));
    memcpy(candidate, header, 72); candidate[6] = '\t';
    assert(!remote_admin_authorize(candidate));
    memcpy(candidate, header, 72); candidate[7 + 19] = 'A';
    assert(!remote_admin_authorize(candidate));
    memcpy(candidate, header, 71); strcpy(candidate + 71, " ");
    assert(!remote_admin_authorize(candidate));
    strcpy(candidate + 71, "\r\n");
    assert(!remote_admin_authorize(candidate));
    memset(candidate, 'a', sizeof(candidate));
    assert(!remote_admin_authorize(candidate)); /* bounded, no terminator */
    assert(remote_admin_set_enabled(false) == ESP_OK && !remote_admin_authorize(header));
    puts("PASS remote admin: exact bounded Bearer syntax, every wrong token position, output bounds");
}

static void test_rotation(void)
{
    fixture();
    assert(remote_admin_init() == ESP_OK);
    random_sequence = 32;
    char old_header[72], new_header[72], token[65];
    header_for(saved_token, old_header);
    watch_current();
    assert(remote_admin_rotate_key() == ESP_OK);
    observe_writes = false;
    assert(remote_admin_get_token(token, sizeof(token)) && strcmp(token, saved_token) != 0);
    header_for(token, new_header);
    assert(remote_admin_enabled() && remote_admin_authorize(new_header));
    assert(!remote_admin_authorize(old_header));
    reboot();
    assert(remote_admin_init() == ESP_OK && remote_admin_authorize(new_header));
    assert(!remote_admin_authorize(old_header));
    assert(remote_admin_set_enabled(false) == ESP_OK);
    watch_current();
    assert(remote_admin_rotate_key() == ESP_OK);
    observe_writes = false;
    assert_closed(true);
    assert(remote_admin_get_token(token, sizeof(token)));
    assert(remote_admin_set_enabled(true) == ESP_OK);
    assert(!remote_admin_authorize(new_header));
    header_for(token, new_header);
    assert(remote_admin_authorize(new_header));
    int writes = sets;
    random_mode = 1;
    assert(remote_admin_rotate_key() == ESP_FAIL && sets == writes);
    assert(remote_admin_authorize(new_header));
    random_mode = 2;
    assert(remote_admin_rotate_key() == ESP_FAIL && sets == writes);
    assert(remote_admin_authorize(new_header));
    reset();
    assert(remote_admin_init() == ESP_OK);
    random_mode = 1;
    assert(remote_admin_set_enabled(true) == ESP_FAIL && sets == 0);
    assert_closed(false);
    puts("PASS remote admin: rotation revokes old tokens, retains disabled state, rejects invalid randomness");
}

static void test_invalid_saved_state(void)
{
    for (int test = 0; test < 11; ++test) {
        fixture();
        switch (test) {
        case 0: stored_size = 0; break;
        case 1: stored_size = 39; break;
        case 2: stored_size = 41; break;
        case 3: stored_blob[0] ^= 1; break;
        case 4: stored_blob[2] = 0; break;
        case 5: stored_blob[2] = 2; break;
        case 6: stored_blob[3] = 2; break;
        case 7: memset(stored_blob + 4, 0, 32); break;
        case 8: stored_blob[13] ^= 1; break;
        case 9: stored_blob[36] ^= 1; break;
        case 10: memset(stored_blob, 0xff, 40); break;
        }
        assert(remote_admin_init() != ESP_OK);
        assert_closed(false);
        assert(remote_admin_set_enabled(true) == ESP_ERR_INVALID_STATE);
        assert(remote_admin_rotate_key() == ESP_ERR_INVALID_STATE);
        assert(sets == 0 && random_calls == 0);
    }
    fixture();
    get_error = ESP_FAIL;
    assert(remote_admin_init() == ESP_FAIL);
    assert_closed(false);
    assert(opens == closes);
    get_error = 0;
    assert(remote_admin_init() == ESP_OK && remote_admin_enabled());
    open_error = ESP_FAIL;
    assert(remote_admin_init() == ESP_FAIL);
    assert_closed(false);
    puts("PASS remote admin: malformed/unsupported/corrupt records and NVS read errors fail closed");
}

static void test_mutation_errors(void)
{
    for (int phase = 0; phase < 3; ++phase) {
        reset();
        assert(remote_admin_init() == ESP_OK);
        int *error = phase == 0 ? &open_error : phase == 1 ? &set_error : &commit_error;
        *error = ESP_FAIL;
        watch_current();
        assert(remote_admin_set_enabled(true) == ESP_FAIL);
        observe_writes = false;
        assert_closed(false);
        assert(!blob_exists && !pending_exists);
        *error = 0;
        assert(remote_admin_set_enabled(true) == ESP_OK);
        assert(remote_admin_enabled());

        fixture();
        assert(remote_admin_init() == ESP_OK);
        random_sequence = 32;
        error = phase == 0 ? &open_error : phase == 1 ? &set_error : &commit_error;
        *error = ESP_FAIL;
        watch_current();
        assert(remote_admin_rotate_key() == ESP_FAIL);
        observe_writes = false;
        assert_closed(true);
        char token[65];
        assert(remote_admin_get_token(token, sizeof(token)) && strcmp(token, saved_token) == 0);
        assert(memcmp(stored_blob, saved_enabled, sizeof(saved_enabled)) == 0);
        *error = 0;
        int before = sets;
        assert(remote_admin_set_enabled(false) == ESP_OK && sets == before + 1);
        reboot();
        assert(remote_admin_init() == ESP_OK);
        assert_closed(true);

        assert(remote_admin_set_enabled(true) == ESP_OK);
        error = phase == 0 ? &open_error : phase == 1 ? &set_error : &commit_error;
        *error = ESP_FAIL;
        assert(remote_admin_set_enabled(false) == ESP_FAIL);
        assert_closed(true);
        *error = 0;
        before = sets;
        assert(remote_admin_set_enabled(false) == ESP_OK && sets == before + 1);
    }
    puts("PASS remote admin: failed open/set/commit closes access without publishing candidate keys");
}

static void test_mutex_failures(void)
{
    reset();
    allocation_fails = true;
    assert(remote_admin_init() == ESP_ERR_NO_MEM);
    assert_closed(false);
    allocation_fails = false;
    creation_race = true;
    assert(remote_admin_init() == ESP_OK);
    assert(creates == 3 && deletes == 1);
    assert(remote_admin_set_enabled(true) == ESP_OK);
    take_fails = true;
    assert(remote_admin_set_enabled(false) == ESP_ERR_INVALID_STATE);
    assert_closed(true);
    take_fails = false;
    assert(remote_admin_set_enabled(false) == ESP_OK);
    puts("PASS remote admin: mutex creation race, allocation/wait failures, flash outside critical sections");
}

int main(void)
{
    test_defaults_and_persistence();
    test_authorization();
    test_rotation();
    test_invalid_saved_state();
    test_mutation_errors();
    test_mutex_failures();
    reset();
    return 0;
}
