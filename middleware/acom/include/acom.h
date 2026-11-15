#pragma once

#include <stdint.h>

#include "esp_err.h"

// ACOM: the contact link between two devices. One wire, open-drain UART at
// 4800 baud (PIN_ACOM_OD), pulled up through PIN_ACOM_PU only while an
// exchange runs, so an unpowered peer is not back-powered on contact.
//
// Both sides probe at random intervals with their BLE address; once a side
// holds the peer's address it sets the ACK flag in its frames. Wire format
// as in the old firmware:
//   A5 5A | addr[6] (MSB first) | type (bits 0..6 BLE addr type, bit 7 ACK) | CRC-8
// CRC-8 is esp_rom_crc8_le(0, ...) over addr and type.

typedef enum
{
    ACOM_DONE_ACKED,   // got the peer's address, and the peer has ours
    ACOM_DONE_NO_ACK,  // got the peer's address, never saw its ACK
    ACOM_TIMEOUT,      // no peer within ACOM_TIMEOUT_MS
} acom_result_t;

// Runs in the ACOM task. peer_addr and peer_type are only valid unless
// result is ACOM_TIMEOUT; peer_addr points to driver memory, copy it.
typedef void (*acom_done_cb_t)(acom_result_t result, const uint8_t peer_addr[6],
                               uint8_t peer_type);

// UART, pins and the ACOM task. The pull-up stays off.
esp_err_t acom_init(void);

// Starts one exchange with our own BLE address (MSB first) and type. Ends
// with exactly one call to cb, unless acom_stop() comes first.
// ESP_ERR_INVALID_STATE while an exchange is running.
esp_err_t acom_start(const uint8_t own_addr[6], uint8_t own_type, acom_done_cb_t cb);

// Ends a running exchange without calling cb and drops the pull-up. Blocks
// until the ACOM task has let go of the line.
void acom_stop(void);
