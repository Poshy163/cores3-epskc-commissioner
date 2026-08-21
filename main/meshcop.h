#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* MeshCoP TLV types (ot-commissioner src/library/tlv.hpp) */
enum {
    TLV_CHANNEL = 0,
    TLV_PANID = 1,
    TLV_EXT_PANID = 2,
    TLV_NETWORK_NAME = 3,
    TLV_PSKC = 4,
    TLV_NETWORK_KEY = 5,
    TLV_COMMISSIONER_ID = 10,
    TLV_COMMISSIONER_SESSION_ID = 11,
    TLV_SECURITY_POLICY = 12,
    TLV_ACTIVE_TIMESTAMP = 14,
    TLV_STATE = 16,
    TLV_CHANNEL_MASK = 53,
};

const char *meshcop_tlv_name(unsigned type);

/*
 * Run the Thread 1.4 ePSKc external-commissioner exchange against a Border
 * Agent's ephemeral-key endpoint: DTLS/EC-JPAKE, commissioner petition, then
 * MGMT_ACTIVE_GET.
 *
 * On success `dataset` holds the raw Active Operational Dataset TLVs.
 */
esp_err_t meshcop_fetch_dataset(const char *addr, uint16_t port, const char *passcode,
                                uint8_t *dataset, size_t dataset_cap, size_t *dataset_len);

/* Pretty-print raw dataset TLVs. Secrets are masked unless `reveal` is true. */
void meshcop_print_tlvs(const uint8_t *tlvs, size_t len, bool reveal);

/*
 * Pull the non-secret identifying fields out of a dataset, for display.
 * Any field not present is left at its zeroed/empty value.
 */
void meshcop_summarize(const uint8_t *tlvs, size_t len,
                       char *name, size_t name_cap, int *channel, uint16_t *panid);

/* Why the last meshcop_fetch_dataset() failed, for the result screen. */
const char *meshcop_last_error(void);
