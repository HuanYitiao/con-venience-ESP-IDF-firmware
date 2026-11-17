#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Profile transfer between two devices over BLE, wire-compatible with the
// old firmware (NimBLE-Arduino, "read model + size header"):
//
//   Service 4fafc201-1fb5-459e-8fcc-c5c9c331914b
//     TX ...26a8 (read):  the chunk the client asked for
//     RX ...26a9 (write): size header, chunk requests, the client's chunks
//
//   RX writes, told apart by length and first byte:
//     [AA][len_hi][len_lo]          size header, client's data length
//     [BB][idx_hi][idx_lo]          chunk request, then the client reads TX
//     [idx_hi][idx_lo][tot_hi][tot_lo][data]   client's chunk (>= 4 bytes)
//   TX reads: [idx_hi][idx_lo][tot_hi][tot_lo][up to 240 data bytes]
//
// The client connects, sends the size header, fetches the server's data
// chunk by chunk, writes its own, and disconnects. Which side is which is
// up to the caller (the old firmware: the lower MAC is the server).

#define BLE_LINK_MAX_LEN 5000  // largest transfer either way (old PACKING_BUF_MAX)

typedef enum
{
    BLE_LINK_SERVER,  // advertises, waits for the peer
    BLE_LINK_CLIENT,  // connects to the peer
} ble_link_role_t;

// success: the peer's data arrived complete. data/len: the peer's data,
// driver memory valid until the next ble_link_start(). Runs in a BLE task:
// keep it short.
typedef void (*ble_link_done_cb_t)(bool success, const uint8_t *data, size_t len);

typedef enum
{
    BLE_LINK_PROG_CONNECTING,     // not connected, or the total is not known yet
    BLE_LINK_PROG_ACTIVE,         // done/total are valid
    BLE_LINK_PROG_INDETERMINATE,  // transferring, no total
} ble_link_prog_t;

// Starts the NimBLE host and registers the service. Needs NVS (PHY
// calibration). Blocks until the host is ready, at most a few seconds.
esp_err_t ble_link_init(void);

// Our identity address, MSB first (as sent over ACOM), and its type.
esp_err_t ble_link_own_addr(uint8_t addr[6], uint8_t *type);

// One transfer: our data goes to the peer, the peer's comes back through cb.
// peer: MSB first, as received over ACOM. data is copied.
esp_err_t ble_link_start(const uint8_t peer[6], uint8_t peer_type, ble_link_role_t role,
                         const uint8_t *data, size_t len, ble_link_done_cb_t cb);

// Ends a transfer without calling cb. Not from inside cb.
void ble_link_stop(void);

bool ble_link_is_busy(void);
bool ble_link_is_connected(void);

// done/total in chunks, both directions together; valid when ACTIVE.
ble_link_prog_t ble_link_progress(uint16_t *done, uint16_t *total);
