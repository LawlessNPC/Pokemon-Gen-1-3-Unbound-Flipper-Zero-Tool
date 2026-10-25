#pragma once

#include <stdint.h>

/* Driver contract between the Game Link hardware driver and the link layer.
 * The MlinkRx passed to the protocol is the result of the MlinkTx the protocol
 * produced in the previous frame. */

#define MLINK_BURST_LEN 9
#define MLINK_NO_DATA   0xFFFFu

typedef struct {
    uint8_t count; /* 1 (handshake) or 9 (burst) */
    uint16_t word[MLINK_BURST_LEN];
} MlinkTx;

typedef struct {
    uint8_t count; /* == count of the MlinkTx that produced it */
    uint16_t word[MLINK_BURST_LEN]; /* GBA's word per transfer, MLINK_NO_DATA if no answer */
    uint16_t timeout_mask; /* bit t: no start bit seen in transfer t */
    uint16_t framing_mask; /* bit t: stop bit low in transfer t */
} MlinkRx;
