#pragma once

#include <stdint.h>

/* Driver contract between the Game Link hardware driver and the link layer.
 * The MlinkRx passed to the protocol is the result of the MlinkTx the protocol
 * produced in the previous frame. */

#define MLINK_BURST_LEN 9
#define MLINK_NO_DATA   0xFFFFu

typedef struct {
    uint8_t count; /* frame mode 1..9; stream mode 0..9, 0 = idle tick (no transfer) */
    uint16_t word[MLINK_BURST_LEN];
} MlinkTx;

typedef struct {
    uint8_t count; /* == count of the MlinkTx that produced it */
    uint16_t word[MLINK_BURST_LEN]; /* GBA's word per transfer, MLINK_NO_DATA if no answer */
    uint16_t timeout_mask; /* bit t: no start bit seen in transfer t */
    uint16_t framing_mask; /* bit t: stop bit low in transfer t */
} MlinkRx;

/* Frame: frames start at the GBA frame rate (the Cable Club link). Stream: a frame starts on the
 * tick after the previous frame's callback, and count 0 is an idle tick (multiboot and the helper
 * protocol). */
typedef enum { MlinkModeFrame, MlinkModeStream } MlinkMode;

/* One tick, start to start (mlink_hw_furi.c MLINK_TICK_DECI_US / 10). */
#define MLINK_TICK_US 1073u
#define MLINK_FRAME_US 16740u /* frame mode: frame start to frame start */

/* Time one frame takes, for protocol timeouts. Stream: max(count, 1) ticks.
 * Frame: MLINK_FRAME_US. */
static inline uint32_t mlink_frame_us(MlinkMode mode, uint8_t count) {
    if(mode == MlinkModeFrame) return MLINK_FRAME_US;
    return (count ? count : 1u) * MLINK_TICK_US;
}
