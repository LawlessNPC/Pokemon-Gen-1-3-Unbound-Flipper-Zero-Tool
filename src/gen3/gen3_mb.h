#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/mlink_types.h>
#include <src/gen3/gen3_rules.h>

/* BIOS multiboot sender, the Flipper as the multiplayer parent and the GBA's BIOS (held at
 * Start+Select) as child 1: GBATEK "BIOS Multi Boot (Single Game Pak)", as specialised in
 * multiboot-and-save.md §2.2-§2.6. Pure C, no heap, no file-scope state.
 *
 * ISR-safe: gen3_mb_frame runs in the driver's frame callback (stream mode). It and everything it
 * calls (gen3_mb_crypt_init, gen3_mb_crypt_word, gen3_mb_crypt_final, gen3_mb_crc_step,
 * gen3_mb_hh, gen3_rng_next) only compute: no allocation, logging, blocking or furi call, at most
 * MLINK_BURST_LEN words per frame and one CRC step per 32-bit payload word, no loop over the
 * image. gen3_mb_image_check and gen3_mb_init are app-context calls. */

#define GEN3_MB_CLIENT_BIT 0x02u /* child 1 */
#define GEN3_MB_PALETTE 0x93u /* logo palette: color 1, direction 0, speed 1 */
#define GEN3_MB_HEADER_LEN 0xC0u
#define GEN3_MB_MIN_LEN 0x1C0u
/* our helper cap (the helper is about 6.5-8.5 KB); the BIOS allows 0x40000 */
#define GEN3_MB_MAX_LEN 0x3000u
/* Counted failures allowed; one more and the sender gives up, to ask for a power cycle with
 * Start+Select (multiboot-and-save.md §2.6). How the BIOS recovers is not documented:
 * multiboot-and-save.md M4. */
#define GEN3_MB_MAX_RESTARTS 3u
/* Restart wait = GEN3_MB_RESTART_MIN_US + rng % (GEN3_MB_RESTART_RAND_US + 1)
 * (multiboot-and-save.md §2.4), at least GBATEK's 1/16 s. */
#define GEN3_MB_RESTART_MIN_US 70000u
#define GEN3_MB_RESTART_RAND_US 170000u
/* Detection replies that are neither 0x0000, 0xFFFF nor 0x72x2 before a restart wait (not
 * counted) */
#define GEN3_MB_JUNK_LIMIT 16u
/* Detect, Palette and Final poll at most once per GBA frame (59.73 Hz), as a GBA master does,
 * with idle ticks between: how the BIOS takes faster polling is not documented. */
#define GEN3_MB_POLL_US 16740u
/* 16 palette polls at the GBA frame rate (multiboot-and-save.md §2.2 step 7), as time */
#define GEN3_MB_PALETTE_US 268000u
/* Silent wait before the length word: at least GBATEK's 1/16 s (multiboot-and-save.md §2.2
 * step 9); why 100 ms: multiboot-and-save.md M20 */
#define GEN3_MB_LENGTH_DELAY_US 100000u
/* Waiting for 0x0075 (multiboot-and-save.md §2.2 step 12) */
#define GEN3_MB_FINAL_US 5000000u

/* One CRC step: 32 rounds over data, polynomial 0xA517 (multiboot-and-save.md §2.3). */
uint32_t gen3_mb_crc_step(uint32_t crc, uint32_t data);
/* The handshake byte for one child: (0x11 + cc + 0xFF + 0xFF) & 0xFF, absent children as 0xFF. */
uint8_t gen3_mb_hh(uint8_t cc);

typedef struct {
    uint32_t seed;
    uint32_t crc;
} Gen3MbCrypt;

/* seed = pp | cc << 8 | 0xFFFF0000, crc = 0xFFF8 */
void gen3_mb_crypt_init(Gen3MbCrypt* k, uint8_t pp, uint8_t cc);
/* Plaintext word at byte offset ptr (0xC0, 0xC4, ...): CRC step, seed step, and the encrypted
 * word e, sent as e & 0xFFFF then e >> 16 (multiboot-and-save.md §2.3). */
uint32_t gen3_mb_crypt_word(Gen3MbCrypt* k, uint32_t ptr, uint32_t plain);
/* The final CRC: gen3_mb_crc_step(crc, hh | rr << 8 | 0xFFFF0000), 16 bits. */
uint16_t gen3_mb_crypt_final(const Gen3MbCrypt* k, uint8_t hh, uint8_t rr);

typedef enum {
    Gen3MbImageOk,
    Gen3MbImageBadLen,
    Gen3MbImageBadHeader,
    Gen3MbImageBadVersion,
} Gen3MbImageCheck;

/* In this order: len in [GEN3_MB_MIN_LEN, GEN3_MB_MAX_LEN] and a multiple of 16; the header (the
 * logo's first word le32(img + 4) == 0x51AEFF24, img[0xB2] == 0x96, img[0xBD] the complement of
 * 0xA0..0xBC, multiboot-and-save.md §2.1); img[0xBC] == version. */
Gen3MbImageCheck gen3_mb_image_check(const uint8_t* img, uint32_t len, uint8_t version);

typedef enum {
    Gen3MbPhaseWait, /* idle ticks: the restart wait */
    Gen3MbPhaseDetect,
    Gen3MbPhaseHello,
    Gen3MbPhaseHeader,
    Gen3MbPhasePalette,
    Gen3MbPhaseHandshake,
    Gen3MbPhaseDelay, /* idle ticks: the silent wait before the length word */
    Gen3MbPhaseLength,
    Gen3MbPhaseData,
    Gen3MbPhaseFinal,
    Gen3MbPhaseCrc,
    Gen3MbPhaseDone, /* the BIOS took the image: idle ticks from now on */
    Gen3MbPhaseFailed, /* too many counted failures, or a bad length: idle ticks from now on */
} Gen3MbPhase;

typedef enum {
    Gen3MbFailNone,
    Gen3MbFailHello,
    Gen3MbFailHeader,
    Gen3MbFailPalette,
    Gen3MbFailHandshake,
    Gen3MbFailLength,
    Gen3MbFailData,
    Gen3MbFailFinal,
    Gen3MbFailCrc,
} Gen3MbFail;

typedef struct {
    Gen3MbPhase phase;
    Gen3MbFail last_fail;
    uint8_t restarts; /* counted failures (all after detection succeeded) */
    uint32_t sent; /* payload bytes acknowledged */
    uint32_t total; /* payload bytes to send: len - GEN3_MB_HEADER_LEN */
    uint32_t waiting_us; /* time in Wait and Detect since the last 0x72x2 (UI hint after 3 s) */
} Gen3MbStatus;

/* Holds a pointer to the caller's image; no heap. Fields are private to gen3_mb.c. */
typedef struct Gen3Mb {
    const uint8_t* img;
    uint32_t len;
    Gen3Rng rng; /* restart waits */
    Gen3MbCrypt crypt;
    Gen3MbPhase phase;
    Gen3MbFail last_fail;
    uint8_t restarts;
    uint8_t cc; /* the slave's byte from 0x73cc */
    uint8_t rr; /* the slave's byte from 0x73rr */
    uint8_t junk; /* Detect: other replies since entering it */
    uint8_t tx_count; /* words in the frame in flight */
    uint16_t pos; /* Header: next word of 98; Data: next payload halfword */
    uint16_t crc; /* Crc: the CRC sent */
    uint32_t e; /* Data: the encrypted word whose high half goes next */
    uint32_t phase_us; /* time in this phase */
    uint32_t poll_us; /* polling phases: time since the last poll */
    uint32_t wait_us; /* Wait: this wait's length */
    uint32_t sent;
    uint32_t waiting_us;
} Gen3Mb;

/* Starts in Detect; the first gen3_mb_frame call (rx NULL) polls at once. img must pass
 * gen3_mb_image_check and stay valid until Done or Failed. A len outside GEN3_MB_MIN_LEN..
 * GEN3_MB_MAX_LEN or not a multiple of 16 starts Failed, and nothing past len is ever read. */
void gen3_mb_init(Gen3Mb* mb, const uint8_t* img, uint32_t len, uint32_t seed);

/* One call per driver frame (ISR context). rx is the result of the previous tx (NULL on the first
 * call); elapsed_us is that frame's time (mlink_frame_us(MlinkModeStream, rx->count); 0 on the
 * first call). Fills tx: count 0 (an idle tick), 1 (a poll or a handshake word) or up to
 * MLINK_BURST_LEN (header and data, checked reply by reply). */
void gen3_mb_frame(Gen3Mb* mb, const MlinkRx* rx, MlinkTx* tx, uint32_t elapsed_us);

/* Safe to call from a thread while the ISR runs: phase is one aligned field, written once per
 * change. sent, total, restarts, last_fail and waiting_us may be read mid-update: they are
 * progress only. */
void gen3_mb_status(const Gen3Mb* mb, Gen3MbStatus* out);
