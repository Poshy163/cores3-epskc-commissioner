/*
 * Mask the reserved PHR bit of received ACK frames before OpenThread sees them.
 *
 * An 802.15.4 PHR is a 7-bit frame length plus one reserved bit, and the
 * ESP32-H2 radio stores the raw byte in front of the frame. ESP-IDF clears the
 * reserved bit for received data frames (ieee802154_receive_done(), "ignore
 * bit8 for the frame length"), but hands ACK frames to
 * ieee802154_inner_transmit_done() as received. OpenThread's radio port copies
 * that byte into the ACK's mLength, so an ACK with the reserved bit set reaches
 * the spinel host with a length of 128-255. The host's ParseRadioFrame()
 * rejects anything longer than a 127-byte PSDU, treats the RCP as misbehaving
 * and resets it ("Handle radio frame failed: Parse", then "RCP failure
 * detected").
 *
 * main/CMakeLists.txt links with -Wl,--wrap=ieee802154_inner_transmit_done,
 * so the driver's call from esp_ieee802154_dev.c lands here first. The driver
 * already keeps that function in IRAM; so does this wrapper.
 */
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_ieee802154_types.h"

#define PHR_LENGTH_MASK 0x7f

void __real_ieee802154_inner_transmit_done(const uint8_t *frame, const uint8_t *ack,
                                           esp_ieee802154_frame_info_t *ack_frame_info);

void IRAM_ATTR __wrap_ieee802154_inner_transmit_done(const uint8_t *frame, const uint8_t *ack,
                                                     esp_ieee802154_frame_info_t *ack_frame_info)
{
    if (ack != NULL) {
        /* `ack` points into the driver's own receive buffer, which is writable;
         * the const only describes the callback interface. */
        ((uint8_t *) ack)[0] &= PHR_LENGTH_MASK;
    }
    __real_ieee802154_inner_transmit_done(frame, ack, ack_frame_info);
}
