#include <assert.h>
#include <string.h>

#include "../main/meshcop_codec.h"

int main(void)
{
    const uint8_t ack[] = {0x60, 0x00, 0x12, 0x34};
    const uint8_t con_empty[] = {0x40, 0x00, 0x12, 0x34};
    assert(meshcop_coap_is_empty_ack(ack, sizeof(ack)));
    assert(!meshcop_coap_is_empty_ack(con_empty, sizeof(con_empty)));

    const uint8_t success[] = {0x61, 0x44, 0, 1, 0xaa, 0xff, 1, 2};
    assert(meshcop_coap_is_success(success, sizeof(success)));
    size_t len = 0;
    const uint8_t *payload = meshcop_coap_payload(success, sizeof(success), &len);
    assert(payload == success + 6 && len == 2 && !memcmp(payload, "\x01\x02", 2));

    const uint8_t excessive_token[] = {0x49, 0x44, 0, 1};
    const uint8_t truncated_token[] = {0x48, 0x44, 0, 1};
    const uint8_t truncated_delta[] = {0x40, 0x44, 0, 1, 0xe0, 0};
    const uint8_t truncated_length[] = {0x40, 0x44, 0, 1, 0x0e, 0};
    const uint8_t reserved_option[] = {0x40, 0x44, 0, 1, 0xf0};
    assert(!meshcop_coap_is_success(excessive_token, sizeof(excessive_token)));
    assert(!meshcop_coap_is_success(truncated_token, sizeof(truncated_token)));
    assert(meshcop_coap_payload(truncated_delta, sizeof(truncated_delta), &len) == NULL);
    assert(meshcop_coap_payload(truncated_length, sizeof(truncated_length), &len) == NULL);
    assert(meshcop_coap_payload(reserved_option, sizeof(reserved_option), &len) == NULL);
    uint8_t extended[278] = {0x40, 0x44, 0, 1, 0x0e, 0, 0};
    extended[276] = 0xff; /* 269-byte option followed by one payload byte */
    extended[277] = 7;
    assert(meshcop_coap_payload(extended, sizeof(extended), &len) == extended + 277 && len == 1);

    const uint8_t petition_accept[] = {0x60, 0x44, 0, 1, 0xff, 16, 1, 1};
    const uint8_t petition_reject[] = {0x60, 0x44, 0, 1, 0xff, 16, 1, 0xff};
    const uint8_t petition_missing_state[] = {0x60, 0x44, 0, 1, 0xff, 11, 2, 0, 1};
    const uint8_t petition_truncated_state[] = {0x60, 0x44, 0, 1, 0xff, 16, 2, 1};
    assert(meshcop_petition_was_accepted(petition_accept, sizeof(petition_accept)));
    assert(!meshcop_petition_was_accepted(petition_reject, sizeof(petition_reject)));
    assert(!meshcop_petition_was_accepted(petition_missing_state,
                                          sizeof(petition_missing_state)));
    assert(!meshcop_petition_was_accepted(petition_truncated_state,
                                          sizeof(petition_truncated_state)));
    return 0;
}
