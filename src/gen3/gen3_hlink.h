#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <src/gen3/mlink_types.h>

/* The Flipper's side of the helper protocol, the Flipper as the multiplayer parent and the helper
 * as child 1 (multiboot-and-save.md §6.1, §6.2, §6.5): frames one request into stream-mode driver
 * frames, then reads and checks the helper's response. One request at a time; the client (seq
 * numbers, resends, command layouts) sits on top. Pure C, no heap, no file-scope state.
 *
 * ISR-safe: gen3_hlink_frame runs in the driver's frame callback (stream mode). It and what it
 * calls (gen3_crc32_update) only compute: no allocation, logging, blocking or furi call, at most
 * MLINK_BURST_LEN words per frame, one CRC update (two bytes) per word, no loop over a payload,
 * and no local over a few bytes. Every other function here is app-side.
 *
 * Hand-off between the app and the ISR, lock-free:
 * - gen3_hlink_submit and gen3_hlink_sync act only while the state is Idle, Done or Failed, when
 *   the ISR reads nothing but the state. They write the request and reset the results, run a full
 *   fence (__atomic_thread_fence), and only then store the new state. Called in any other state
 *   they do nothing.
 * - The request's buffers (data, resp) belong to the pump from gen3_hlink_submit until the state
 *   is Done or Failed; the pump never touches them after that.
 * - gen3_hlink_state loads the state, then runs the same fence, so a caller that sees Done or
 *   Failed reads the final results with gen3_hlink_result. */

#define GEN3_HLINK_SOF 0x55AAu /* w0 of every packet */
#define GEN3_HLINK_READY 0xB0A0u /* the helper is hunting for a request */
#define GEN3_HLINK_BUSY 0xB0B0u /* the helper is executing one */
#define GEN3_HLINK_RECV 0xB0C0u /* the helper is receiving a packet */
#define GEN3_HLINK_IDLE 0x0000u /* the Flipper's word when it has nothing to send */
#define GEN3_HLINK_MAX_PAYLOAD 2064u /* payload words in one packet, either way */
#define GEN3_HLINK_MAX_ARGS 6u /* ARM_WRITE's, the most any command takes */

/* The word budgets below count transfers. With the cable out, the driver's cable guard reports a
 * full frame of timed-out words for every one-tick frame it skips (mlink_frame_us), so they run
 * out up to MLINK_BURST_LEN times sooner than the tick count suggests. That only ever fails a
 * request early: the helper finishes any flash work without the link, and the client's resend of
 * a write-class command gets the stored response instead of a second run
 * (multiboot-and-save.md §6.5, §6.8). */

/* After a request's last word: words in a row that are neither BUSY nor a SOF (timed-out words
 * count) before the response counts as lost, multiboot-and-save.md §6.5. A BUSY starts the count
 * again. About 34 ms of ticks; about 4 ms with the cable out. */
#define GEN3_HLINK_NO_RESPONSE_XFERS 32u
/* Before a request: words in a row that are neither READY nor BUSY (timed-out words count) before
 * the helper counts as gone. A BUSY starts the count again. About 69 ms of ticks; about 8 ms with
 * the cable out. */
#define GEN3_HLINK_NOT_READY_XFERS 64u
/* BUSY budget of a request that sets none (multiboot-and-save.md §6.5: every command but
 * CART_INFO, ERASE_SECTOR, WRITE_SECTOR and COPY_SECTOR). A wait's BUSY time runs from the frame of
 * its first valid BUSY, in the elapsed_us the caller passes. A frame the cable guard skips takes
 * one tick but counts as nine, so a cable that keeps dropping out can only end a long BUSY early,
 * never late. */
#define GEN3_HLINK_BUSY_US 6000000u

/* CRC-32, reflected polynomial 0xEDB88320, one byte at a time; no init or xorout, so the register
 * carries across calls. */
uint32_t gen3_crc32_update(uint32_t reg, const uint8_t* p, size_t n);
/* IEEE CRC-32 (init and xorout 0xFFFFFFFF): check("123456789") = 0xCBF43926. A packet's CRC is
 * this over w1 .. its last payload word, each word as its low byte, then its high byte. */
uint32_t gen3_crc32(const uint8_t* p, size_t n);

typedef enum {
    Gen3HlinkIdle, /* nothing to do: idle ticks */
    Gen3HlinkSync, /* waiting for a run of READY */
    Gen3HlinkSending, /* waiting for READY, then sending the request */
    Gen3HlinkWaiting, /* the request is out: waiting for the response's SOF */
    Gen3HlinkReceiving, /* reading the response */
    Gen3HlinkDone, /* idle ticks; results ready */
    Gen3HlinkFailed, /* idle ticks; gen3_hlink_result says why */
} Gen3HlinkState;

typedef enum {
    Gen3HlinkFailNone,
    /* No READY: GEN3_HLINK_NOT_READY_XFERS other words before a request, or a sync's timeout */
    Gen3HlinkFailNoReady,
    /* GEN3_HLINK_NO_RESPONSE_XFERS words with neither BUSY nor a SOF after the request */
    Gen3HlinkFailNoResponse,
    /* BUSY for longer than the request's budget, before or after the request */
    Gen3HlinkFailBusyTimeout,
    Gen3HlinkFailBadCrc, /* the response's CRC doesn't match */
    /* A response word came back timed out or with a framing error. The pump reads the rest of the
     * packet first, so that the helper is hunting again for the resend; only a bad n (w2) ends
     * it at once, as the length is then unknown. */
    Gen3HlinkFailWire,
    Gen3HlinkFailSeq, /* a valid response for another seq */
    /* The response's n is over resp_max_words or GEN3_HLINK_MAX_PAYLOAD (decided at w2), or the
     * request has more than GEN3_HLINK_MAX_ARGS args or GEN3_HLINK_MAX_PAYLOAD payload words (at
     * once, with nothing sent) */
    Gen3HlinkFailTooLong,
} Gen3HlinkFail;

/* One request. Initialise it with designated initializers. */
typedef struct {
    uint8_t code; /* the command (multiboot-and-save.md §6.3) */
    uint8_t seq; /* the response echoes it */
    uint16_t args[GEN3_HLINK_MAX_ARGS];
    uint8_t nargs;
    const uint8_t* data; /* payload after args, LE bytes (WRITE_SECTOR's 4 KB image) */
    uint16_t data_words;
    uint8_t* resp; /* response payload lands here as LE bytes */
    uint16_t resp_max_words;
    /* BUSY budget in µs, 0 for GEN3_HLINK_BUSY_US; the client sets the longer ones of
     * multiboot-and-save.md §6.5 */
    uint32_t busy_us;
} Gen3HlinkReq;

/* Fields other than state are private to gen3_hlink.c. */
typedef struct Gen3Hlink {
    volatile Gen3HlinkState state; /* the hand-off: see above */
    Gen3HlinkReq req; /* a copy; its buffers are borrowed */
    uint32_t busy_max; /* the request's BUSY budget */
    uint32_t busy_us; /* BUSY time of this wait, once busy is set */
    uint32_t sync_max; /* Sync: the timeout */
    uint32_t sync_us; /* Sync: time so far */
    uint32_t crc; /* CRC register: the request's words while sending, the response's while
                   * receiving */
    uint16_t n; /* the request's payload words */
    uint16_t pos; /* Sending: the next request word (0 = SOF); Receiving: the next response word */
    uint16_t other; /* words in a row that were neither what the wait wants nor BUSY */
    uint16_t rw1; /* the response's w1 */
    uint16_t rn; /* the response's n */
    uint16_t crc_lo; /* the response's CRC, low half */
    uint16_t resp_words; /* result: the response's n */
    uint8_t status; /* result: the response's status, w1 & 0xFF */
    uint8_t skip; /* rx slots of the frame in flight that answer request words */
    uint8_t need; /* Sync: READYs in a row to see */
    uint8_t run; /* Sync: READYs in a row so far */
    bool ready; /* Sending: a READY came back, so the request goes out */
    bool busy; /* this wait saw a BUSY */
    bool wire; /* Receiving: a word of this packet came back invalid */
    Gen3HlinkFail fail; /* result */
} Gen3Hlink;

/* Idle. Call before the ISR can see h. */
void gen3_hlink_init(Gen3Hlink* h);
/* App side, only while the state is Idle, Done or Failed (see above). Copies *req; its buffers
 * belong to the pump until Done or Failed. Sending: idle words until a valid READY, then the
 * request from the next frame: SOF, seq << 8 | code, n = nargs + data_words, the args, the data
 * words, and the CRC, low half then high half. */
void gen3_hlink_submit(Gen3Hlink* h, const Gen3HlinkReq* req);
/* App side, as gen3_hlink_submit. Done after `need` valid READYs in a row (0 counts as 1); any
 * other word starts the run again. Failed NoReady once timeout_us has passed without it. */
void gen3_hlink_sync(Gen3Hlink* h, uint8_t need, uint32_t timeout_us);
/* ISR side: one call per driver frame. rx is the result of the previous tx (NULL on the first
 * call); elapsed_us is that frame's time (mlink_frame_us(MlinkModeStream, rx->count)). Fills tx:
 * count 0 in Idle, Done and Failed; otherwise 9 words, the request's first, then
 * GEN3_HLINK_IDLE. The replies to the transfers after the request's last word go, in order, to the
 * response logic, later words of the same frame included. */
void gen3_hlink_frame(Gen3Hlink* h, const MlinkRx* rx, MlinkTx* tx, uint32_t elapsed_us);
/* App side: the state, then a full fence (see above). */
Gen3HlinkState gen3_hlink_state(const Gen3Hlink* h);
/* App side, after gen3_hlink_state gave Done or Failed. Done: status is the response's (w1 &
 * 0xFF), resp_words its n and fail None; after a sync, 0, 0 and None. Failed: 0, 0 and why. */
void gen3_hlink_result(
    const Gen3Hlink* h,
    uint8_t* status,
    uint16_t* resp_words,
    Gen3HlinkFail* fail);
