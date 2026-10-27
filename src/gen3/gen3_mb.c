#include <src/gen3/gen3_mb.h>
#include <string.h>

/* Constants of GBATEK "BIOS Multi Boot (Single Game Pak)", MultiPlay row
 * (multiboot-and-save.md §2.3) */
#define MB_CRC_INIT 0xFFF8u
#define MB_CRC_POLY 0xA517u
#define MB_DATA_KEY 0x6465646Fu
#define MB_SEED_MUL 0x6F646573u
#define MB_LOGO_WORD 0x51AEFF24u /* the logo's first word, header offset 4 */

/* Header phase: 0x60 header halfwords, then 0x6200 and 0x6200 | client bit */
#define MB_HEADER_HALFWORDS 0x60u
#define MB_HEADER_STEPS (MB_HEADER_HALFWORDS + 2u)

#define MB_DETECT_REPLY (0x7200u | GEN3_MB_CLIENT_BIT)

_Static_assert(GEN3_MB_JUNK_LIMIT <= UINT8_MAX, "Gen3Mb.junk counts up to GEN3_MB_JUNK_LIMIT");

uint32_t gen3_mb_crc_step(uint32_t crc, uint32_t data) {
    for(uint8_t i = 0; i < 32; i++) {
        const uint32_t bit = (crc ^ data) & 1u;
        data >>= 1;
        crc >>= 1;
        if(bit) crc ^= MB_CRC_POLY;
    }
    return crc;
}

uint8_t gen3_mb_hh(uint8_t cc) {
    return (uint8_t)(0x11u + cc + 0xFFu + 0xFFu);
}

void gen3_mb_crypt_init(Gen3MbCrypt* k, uint8_t pp, uint8_t cc) {
    k->seed = pp | (uint32_t)cc << 8 | 0xFFFF0000u;
    k->crc = MB_CRC_INIT;
}

uint32_t gen3_mb_crypt_word(Gen3MbCrypt* k, uint32_t ptr, uint32_t plain) {
    k->crc = gen3_mb_crc_step(k->crc, plain);
    k->seed = k->seed * MB_SEED_MUL + 1u;
    return plain ^ (0xFE000000u - ptr) ^ k->seed ^ MB_DATA_KEY;
}

uint16_t gen3_mb_crypt_final(const Gen3MbCrypt* k, uint8_t hh, uint8_t rr) {
    return (uint16_t)gen3_mb_crc_step(k->crc, hh | (uint32_t)rr << 8 | 0xFFFF0000u);
}

static uint32_t mb_le32(const uint8_t* p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool mb_len_ok(uint32_t len) {
    return len >= GEN3_MB_MIN_LEN && len <= GEN3_MB_MAX_LEN && (len % 16u) == 0;
}

Gen3MbImageCheck gen3_mb_image_check(const uint8_t* img, uint32_t len, uint8_t version) {
    if(!mb_len_ok(len)) return Gen3MbImageBadLen;
    if(mb_le32(img + 4) != MB_LOGO_WORD || img[0xB2] != 0x96) return Gen3MbImageBadHeader;
    uint8_t chk = 0;
    for(uint32_t i = 0xA0; i <= 0xBC; i++) chk -= img[i];
    if(img[0xBD] != (uint8_t)(chk - 0x19u)) return Gen3MbImageBadHeader;
    if(img[0xBC] != version) return Gen3MbImageBadVersion;
    return Gen3MbImageOk;
}

static uint32_t mb_add(uint32_t a, uint32_t b) {
    return a > UINT32_MAX - b ? UINT32_MAX : a + b;
}

static void mb_enter(Gen3Mb* mb, Gen3MbPhase phase) {
    mb->phase_us = 0;
    mb->poll_us = GEN3_MB_POLL_US; /* Final polls at once */
    mb->pos = 0;
    mb->junk = 0;
    mb->palette_other = false;
    mb->stale = phase == Gen3MbPhaseDetect;
    mb->phase = phase;
}

static void mb_wait(Gen3Mb* mb) {
    mb->wait_us =
        GEN3_MB_RESTART_MIN_US + gen3_rng_next(&mb->rng) % (GEN3_MB_RESTART_RAND_US + 1u);
    mb->sent = 0;
    mb_enter(mb, Gen3MbPhaseWait);
}

/* A counted failure: wait and restart, or give up after GEN3_MB_MAX_RESTARTS. */
static void mb_fail(Gen3Mb* mb, Gen3MbFail why) {
    mb->last_fail = why;
    mb->restarts++;
    if(mb->restarts > GEN3_MB_MAX_RESTARTS) {
        mb_enter(mb, Gen3MbPhaseFailed);
    } else {
        mb_wait(mb);
    }
}

/* Word t of the frame in flight came back unmarked: the driver saw its start and stop bits. */
static bool mb_valid(const Gen3Mb* mb, const MlinkRx* rx, uint8_t t) {
    return t < mb->tx_count && t < rx->count &&
           !(((rx->timeout_mask | rx->framing_mask) >> t) & 1u);
}

static uint16_t mb_header_word(const Gen3Mb* mb, uint16_t step) {
    if(step == MB_HEADER_HALFWORDS) return 0x6200u;
    if(step > MB_HEADER_HALFWORDS) return 0x6200u | GEN3_MB_CLIENT_BIT;
    return (uint16_t)(mb->img[2 * step] | mb->img[2 * step + 1] << 8);
}

/* The slave counts the header halfwords down (0x60xx .. 0x01xx), 0x00xx after the last one;
 * 0x7200 | bit after the 0x6200 that ends the header. */
static uint16_t mb_header_reply(uint16_t step) {
    if(step <= MB_HEADER_HALFWORDS) {
        return (uint16_t)((MB_HEADER_HALFWORDS - step) << 8 | GEN3_MB_CLIENT_BIT);
    }
    return MB_DETECT_REPLY;
}

static uint16_t mb_data_halfwords(const Gen3Mb* mb) {
    return (uint16_t)((mb->len - GEN3_MB_HEADER_LEN) / 2u);
}

static void mb_receive(Gen3Mb* mb, const MlinkRx* rx) {
    const uint16_t r = rx->word[0];
    switch(mb->phase) {
    case Gen3MbPhaseDetect:
        /* MultiPlay replies run one transfer behind (multiboot-and-save.md §2.2), so the first
         * reply here answers whatever was sent before Detect began, after gen3_mb_init or a wait:
         * a 0x7202 left over from a palette wait would start a Hello that fails. A marked word
         * may not be that reply (a timed-out transfer never reached the slave, whose stale word
         * then goes out next), so the skip waits for an unmarked one; at worst it costs a tick. */
        if(mb->stale) {
            if(mb_valid(mb, rx, 0)) mb->stale = false;
            break;
        }
        if(!mb_valid(mb, rx, 0)) break; /* polling: a marked word is ignored */
        if((r & 0xFFF0u) == 0x7200u && (r & 0xFu) == GEN3_MB_CLIENT_BIT) {
            mb->waiting_us = 0;
            mb_enter(mb, Gen3MbPhaseHello);
        } else if(r != 0x0000u && r != 0xFFFFu && ++mb->junk >= GEN3_MB_JUNK_LIMIT) {
            mb_wait(mb);
        }
        break;
    case Gen3MbPhaseHello:
        if(mb_valid(mb, rx, 0) && r == MB_DETECT_REPLY) {
            mb->attempting = true; /* the first header frame goes out next */
            mb_enter(mb, Gen3MbPhaseHeader);
        } else {
            mb_fail(mb, Gen3MbFailHello);
        }
        break;
    case Gen3MbPhaseHeader:
        for(uint8_t t = 0; t < mb->tx_count; t++) {
            if(!mb_valid(mb, rx, t) || rx->word[t] != mb_header_reply(mb->pos + t)) {
                mb_fail(mb, Gen3MbFailHeader);
                return;
            }
        }
        mb->pos += mb->tx_count;
        if(mb->pos >= MB_HEADER_STEPS) mb_enter(mb, Gen3MbPhasePalette);
        break;
    case Gen3MbPhasePalette:
        if(mb_valid(mb, rx, 0) && (r & 0xFF00u) == 0x7300u) {
            mb->cc = (uint8_t)r;
            mb_enter(mb, Gen3MbPhaseHandshake);
            break;
        }
        if(!mb_valid(mb, rx, 0) || r != MB_DETECT_REPLY) mb->palette_other = true;
        if(mb->phase_us < GEN3_MB_PALETTE_US) break;
        if(mb->palette_other) {
            mb_fail(mb, Gen3MbFailPalette);
        } else {
            mb->last_fail = Gen3MbFailPalette; /* only busy replies: a slow slave, not counted */
            mb_wait(mb);
        }
        break;
    case Gen3MbPhaseHandshake:
        if(mb_valid(mb, rx, 0) && (r & 0xFF00u) == 0x7300u) {
            mb_enter(mb, Gen3MbPhaseDelay);
        } else {
            mb_fail(mb, Gen3MbFailHandshake);
        }
        break;
    case Gen3MbPhaseLength:
        if(mb_valid(mb, rx, 0) && (r & 0xFF00u) == 0x7300u) {
            mb->rr = (uint8_t)r;
            gen3_mb_crypt_init(&mb->crypt, GEN3_MB_PALETTE, mb->cc);
            mb_enter(mb, Gen3MbPhaseData);
        } else {
            mb_fail(mb, Gen3MbFailLength);
        }
        break;
    case Gen3MbPhaseData:
        /* The slave answers each halfword with the byte offset of the next one it expects. */
        for(uint8_t t = 0; t < mb->tx_count; t++) {
            const uint32_t ptr = GEN3_MB_HEADER_LEN + 2u * (mb->pos + t);
            if(!mb_valid(mb, rx, t) || rx->word[t] != (uint16_t)ptr) {
                mb_fail(mb, Gen3MbFailData);
                return;
            }
        }
        mb->pos += mb->tx_count;
        mb->sent = 2u * mb->pos;
        if(mb->pos >= mb_data_halfwords(mb)) mb_enter(mb, Gen3MbPhaseFinal);
        break;
    case Gen3MbPhaseFinal:
        if(mb_valid(mb, rx, 0) && r == 0x0075u) {
            mb_enter(mb, Gen3MbPhaseCrc);
        } else if(mb->phase_us >= GEN3_MB_FINAL_US) {
            mb_fail(mb, Gen3MbFailFinal);
        }
        break;
    case Gen3MbPhaseCrc:
        if(mb_valid(mb, rx, 0) && r == 0x0075u && mb_valid(mb, rx, 1) && rx->word[1] == mb->crc) {
            mb_enter(mb, Gen3MbPhaseDone);
        } else {
            mb_fail(mb, Gen3MbFailCrc);
        }
        break;
    case Gen3MbPhaseWait:
    case Gen3MbPhaseDelay:
    case Gen3MbPhaseDone:
    case Gen3MbPhaseFailed:
        break; /* idle ticks only */
    }
}

static void mb_one(MlinkTx* tx, uint16_t word) {
    tx->count = 1;
    tx->word[0] = word;
}

static void mb_poll(Gen3Mb* mb, MlinkTx* tx, uint16_t word) {
    if(mb->poll_us < GEN3_MB_POLL_US) return; /* idle tick */
    mb->poll_us = 0;
    mb_one(tx, word);
}

static void mb_send(Gen3Mb* mb, MlinkTx* tx) {
    tx->count = 0;
    if(mb->phase == Gen3MbPhaseWait && mb->phase_us >= mb->wait_us) {
        mb_enter(mb, Gen3MbPhaseDetect);
    }
    if(mb->phase == Gen3MbPhaseDelay && mb->phase_us >= GEN3_MB_LENGTH_DELAY_US) {
        mb_enter(mb, Gen3MbPhaseLength);
    }
    switch(mb->phase) {
    case Gen3MbPhaseDetect:
        mb_one(tx, 0x6200u);
        break;
    case Gen3MbPhaseHello:
        mb_one(tx, 0x6100u | GEN3_MB_CLIENT_BIT);
        break;
    case Gen3MbPhaseHeader:
        while(tx->count < MLINK_BURST_LEN && mb->pos + tx->count < MB_HEADER_STEPS) {
            tx->word[tx->count] = mb_header_word(mb, mb->pos + tx->count);
            tx->count++;
        }
        break;
    case Gen3MbPhasePalette:
        mb_one(tx, 0x6300u | GEN3_MB_PALETTE);
        break;
    case Gen3MbPhaseHandshake:
        mb_one(tx, 0x6400u | gen3_mb_hh(mb->cc));
        break;
    case Gen3MbPhaseLength:
        mb_one(tx, (uint16_t)((mb->len - 0x190u) / 4u));
        break;
    case Gen3MbPhaseData:
        while(tx->count < MLINK_BURST_LEN && mb->pos + tx->count < mb_data_halfwords(mb)) {
            const uint32_t half = mb->pos + tx->count;
            const uint32_t ptr = GEN3_MB_HEADER_LEN + 2u * half;
            if(!(half & 1u)) {
                mb->e = gen3_mb_crypt_word(&mb->crypt, ptr, mb_le32(mb->img + ptr));
                tx->word[tx->count] = (uint16_t)mb->e;
            } else {
                tx->word[tx->count] = (uint16_t)(mb->e >> 16);
            }
            tx->count++;
        }
        break;
    case Gen3MbPhaseFinal:
        mb_poll(mb, tx, 0x0065u);
        break;
    case Gen3MbPhaseCrc:
        mb->crc = gen3_mb_crypt_final(&mb->crypt, gen3_mb_hh(mb->cc), mb->rr);
        tx->count = 2;
        tx->word[0] = 0x0066u;
        tx->word[1] = mb->crc;
        break;
    case Gen3MbPhaseWait:
    case Gen3MbPhaseDelay:
    case Gen3MbPhaseDone:
    case Gen3MbPhaseFailed:
        break; /* idle tick */
    }
    mb->tx_count = tx->count;
}

void gen3_mb_init(Gen3Mb* mb, const uint8_t* img, uint32_t len, uint32_t seed) {
    memset(mb, 0, sizeof(*mb));
    mb->img = img;
    mb->len = len;
    gen3_rng_seed(&mb->rng, seed);
    mb_enter(mb, mb_len_ok(len) ? Gen3MbPhaseDetect : Gen3MbPhaseFailed);
}

void gen3_mb_frame(Gen3Mb* mb, const MlinkRx* rx, MlinkTx* tx, uint32_t elapsed_us) {
    mb->phase_us = mb_add(mb->phase_us, elapsed_us);
    mb->poll_us = mb_add(mb->poll_us, elapsed_us);
    if(mb->phase == Gen3MbPhaseWait || mb->phase == Gen3MbPhaseDetect) {
        mb->waiting_us = mb_add(mb->waiting_us, elapsed_us);
    }
    if(mb->attempting) mb->attempt_us = mb_add(mb->attempt_us, elapsed_us);
    if(rx) mb_receive(mb, rx);
    /* After the reply, so a CRC that matches on the last frame still ends Done. */
    if(mb->attempting && mb->attempt_us >= GEN3_MB_ATTEMPT_US && mb->phase != Gen3MbPhaseDone &&
       mb->phase != Gen3MbPhaseFailed) {
        mb->last_fail = Gen3MbFailPowerCycle;
        mb_enter(mb, Gen3MbPhaseFailed);
    }
    mb_send(mb, tx);
}

void gen3_mb_status(const Gen3Mb* mb, Gen3MbStatus* out) {
    out->phase = mb->phase;
    out->last_fail = mb->last_fail;
    out->restarts = mb->restarts;
    out->sent = mb->sent;
    out->total = mb->len > GEN3_MB_HEADER_LEN ? mb->len - GEN3_MB_HEADER_LEN : 0;
    out->waiting_us = mb->waiting_us;
}
