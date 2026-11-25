#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int otError;
typedef int esp_err_t;
typedef struct { int unused; } otInstance;
typedef struct { uint8_t mId[16]; } otBorderAgentId;
enum { OT_OPERATIONAL_DATASET_MAX_LENGTH = 254, portMAX_DELAY = -1, THREAD_TOPOLOGY_NEVER = 0 };
enum { OT_DEVICE_ROLE_DISABLED, OT_DEVICE_ROLE_DETACHED, OT_DEVICE_ROLE_CHILD,
       OT_DEVICE_ROLE_ROUTER, OT_DEVICE_ROLE_LEADER };
enum { OT_ERROR_NONE, OT_ERROR_NOT_FOUND, OT_ERROR_FAILED };
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG };
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef struct {
    uint8_t mTlvs[OT_OPERATIONAL_DATASET_MAX_LENGTH], mLength;
} otOperationalDatasetTlvs;

/* Two short TLVs stand in for a validated active dataset. The SDK owns TLV
 * validation and order-independent equality; mock those two contracts. */
static const otOperationalDatasetTlvs original = {{1, 1, 11, 2, 1, 22}, 6};
static bool s_ready = true;
static otInstance instance;
static struct {
    otOperationalDatasetTlvs active;
    int role, erase, apply, reset, cancel, id_writes, ip_calls, thread_calls;
    otError read_error, ip_error, enable_error;
    bool ip6, locked, fail_first_apply;
} radio;

static void esp_openthread_lock_acquire(int timeout)
{
    assert(timeout == portMAX_DELAY && !radio.locked);
    radio.locked = true;
}
static void esp_openthread_lock_release(void) { assert(radio.locked); radio.locked = false; }
static otInstance *esp_openthread_get_instance(void) { assert(radio.locked); return &instance; }
static void check(otInstance *ins) { assert(ins == &instance && radio.locked); }
static bool otDatasetIsValid(const otOperationalDatasetTlvs *d, bool active)
{
    assert(active);
    return d->mLength == 6 && d->mTlvs[1] == 1 && d->mTlvs[4] == 1;
}
static bool otDatasetTlvsCompare(const otOperationalDatasetTlvs *a, const otOperationalDatasetTlvs *b)
{
    assert(a->mLength == 6 && b->mLength == 6);
    return memcmp(a->mTlvs, b->mTlvs, 6) == 0 ||
           (memcmp(a->mTlvs, b->mTlvs + 3, 3) == 0 && memcmp(a->mTlvs + 3, b->mTlvs, 3) == 0);
}
static otError otDatasetGetActiveTlvs(otInstance *ins, otOperationalDatasetTlvs *out)
{
    check(ins);
    *out = radio.active;
    return radio.read_error;
}
static int otThreadGetDeviceRole(otInstance *ins) { check(ins); return radio.role; }
static bool otIp6IsEnabled(otInstance *ins) { check(ins); return radio.ip6; }
static otError otIp6SetEnabled(otInstance *ins, bool on)
{
    check(ins);
    radio.ip_calls++;
    if (radio.ip_error == OT_ERROR_NONE) { radio.ip6 = on; }
    return radio.ip_error;
}
static otError otThreadSetEnabled(otInstance *ins, bool on)
{
    check(ins);
    radio.thread_calls++;
    if (radio.enable_error == OT_ERROR_NONE) {
        radio.role = on ? OT_DEVICE_ROLE_DETACHED : OT_DEVICE_ROLE_DISABLED;
    }
    return radio.enable_error;
}
static void otMeshDiagCancel(otInstance *ins) { check(ins); radio.cancel++; }
static void topology_reset(int state) { assert(state == THREAD_TOPOLOGY_NEVER); radio.reset++; }
static otError otBorderAgentGetId(otInstance *ins, otBorderAgentId *out)
{
    check(ins);
    memset(out, 0xab, sizeof(*out));
    return OT_ERROR_NONE;
}
static otError otBorderAgentSetId(otInstance *ins, const otBorderAgentId *id)
{
    check(ins);
    radio.id_writes++;
    assert(id->mId[0] == (radio.id_writes % 2 ? 0xaa : 0xab));
    return OT_ERROR_NONE;
}
static otError otInstanceErasePersistentInfo(otInstance *ins)
{
    check(ins);
    assert(radio.role == OT_DEVICE_ROLE_DISABLED && !radio.ip6);
    radio.erase++;
    memset(&radio.active, 0, sizeof(radio.active));
    return OT_ERROR_NONE;
}
static esp_err_t esp_openthread_auto_start(otOperationalDatasetTlvs *d)
{
    assert(radio.locked);
    radio.active = *d;
    radio.apply++;
    return (radio.fail_first_apply && radio.apply == 1) ? ESP_FAIL : ESP_OK;
}

#include "thread_join_production.inc"

static void start_in(int role)
{
    memset(&radio, 0, sizeof(radio));
    radio.active = original;
    radio.role = role;
    radio.ip6 = role != OT_DEVICE_ROLE_DISABLED;
}
static void assert_preserved(void)
{
    assert(!radio.locked && radio.erase == 0 && radio.apply == 0 && radio.id_writes == 0);
    assert(radio.cancel == 0 && radio.reset == 0);
    assert(memcmp(&radio.active, &original, sizeof(original)) == 0);
}

int main(void)
{
    const char *same = "01010b020116";
    const char *reordered = "02011601010b";
    const char *different = "01010b020117";
    for (int role = OT_DEVICE_ROLE_DETACHED; role <= OT_DEVICE_ROLE_LEADER; ++role) {
        start_in(role);
        assert(thread_set_dataset_hex(same) == ESP_OK);
        assert(thread_set_dataset_hex(reordered) == ESP_OK);
        assert(radio.role == role && radio.ip_calls == 0 && radio.thread_calls == 0);
        assert_preserved();
    }

    start_in(OT_DEVICE_ROLE_DISABLED);
    assert(thread_set_dataset_hex(same) == ESP_OK && radio.ip6 && radio.role == OT_DEVICE_ROLE_DETACHED);
    assert(radio.ip_calls == 1 && radio.thread_calls == 1);
    assert_preserved();
    start_in(OT_DEVICE_ROLE_DISABLED);
    radio.ip_error = OT_ERROR_FAILED;
    assert(thread_set_dataset_hex(same) == ESP_FAIL && !radio.ip6 && radio.thread_calls == 0);
    assert_preserved();
    start_in(OT_DEVICE_ROLE_DISABLED);
    radio.enable_error = OT_ERROR_FAILED;
    assert(thread_set_dataset_hex(same) == ESP_FAIL && !radio.ip6 && radio.ip_calls == 2);
    radio.ip6 = true;
    assert(thread_set_dataset_hex(same) == ESP_FAIL && radio.ip6 && radio.ip_calls == 2);
    assert_preserved();

    start_in(OT_DEVICE_ROLE_ROUTER);
    assert(thread_join(NULL, 6) == ESP_ERR_INVALID_ARG);
    assert(thread_set_dataset_hex(NULL) == ESP_ERR_INVALID_ARG);
    assert(thread_set_dataset_hex("0x") == ESP_ERR_INVALID_ARG);
    assert(thread_set_dataset_hex("01") == ESP_ERR_INVALID_ARG);
    radio.read_error = OT_ERROR_FAILED;
    assert(thread_set_dataset_hex(different) == ESP_FAIL);
    assert_preserved();

    start_in(OT_DEVICE_ROLE_ROUTER);
    assert(thread_set_dataset_hex(different) == ESP_OK);
    assert(radio.erase == 1 && radio.apply == 1 && radio.cancel == 1 && radio.reset == 1);
    assert(!radio.locked && radio.id_writes == 2 && radio.active.mTlvs[5] == 23);
    start_in(OT_DEVICE_ROLE_ROUTER);
    radio.fail_first_apply = true;
    assert(thread_set_dataset_hex(different) == ESP_FAIL);
    assert(radio.erase == 1 && radio.apply == 2 && !radio.locked);
    assert(memcmp(&radio.active, &original, sizeof(original)) == 0);
    start_in(OT_DEVICE_ROLE_DISABLED);
    radio.read_error = OT_ERROR_NOT_FOUND;
    radio.fail_first_apply = true;
    assert(thread_set_dataset_hex(same) == ESP_FAIL);
    assert(radio.erase == 2 && radio.apply == 1 && radio.active.mLength == 0 && !radio.locked);
    puts("PASS Thread dataset reapply, reordering, role recovery, replacement and rollback");
    return 0;
}
