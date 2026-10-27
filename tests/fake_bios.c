#include "fake_bios.h"
#include <string.h>

#define FAKE_BIOS_NO_REPLY 0xFFFFu
#define FAKE_BIOS_CHILD_BIT 0x02u /* child 1 */
#define FAKE_BIOS_READY (0x7200u | FAKE_BIOS_CHILD_BIT)
/* Longest transfer the parent runs: its word (about 165 us at 115200 bit/s), up to 100 us until
 * the child's start bit, the child's word (about 156 us) */
#define FAKE_BIOS_XFER_US 430u
#define FAKE_BIOS_GAP_US 36u /* GBATEK "Multiboot Communication": after each transfer */
#define FAKE_BIOS_SILENT_US 62500u /* 1/16 s before the length word */
#define FAKE_BIOS_HEADER_HALFWORDS 0x60u

enum {
    StepDetect, /* 0x6200 (answers 0x7202), then 0x6102 */
    StepHeader, /* 0x60 header halfwords */
    StepHeaderEnd, /* 0x6200 */
    StepInfo, /* 0x6202 */
    StepPalette, /* 0x63pp until it answered 0x73cc, then 0x64hh */
    StepLength, /* after 1/16 s of silence: llll */
    StepData, /* encrypted payload halfwords */
    StepFinal, /* 0x0065 until it answered 0x0075, then 0x0066 */
    StepCrc, /* the parent's CRC */
};

/* GBATEK's form of the CRC step: c ^= data, then 32 times shift right and xor 0xA517 on carry. */
static uint32_t fake_bios_crc(uint32_t c, uint32_t data) {
    c ^= data;
    for(int i = 0; i < 32; i++) {
        const uint32_t carry = c & 1u;
        c >>= 1;
        if(carry) c ^= 0xA517u;
    }
    return c;
}

/* GBATEK SWI 25h: 0x81 + color * 0x10 + direction * 8 + speed * 2 (color 0..6, direction 0..1,
 * speed 0..3), or 0xF1 + color * 2 for a fixed palette. */
static bool fake_bios_palette_ok(uint8_t pp) {
    for(unsigned color = 0; color <= 6; color++) {
        if(pp == 0xF1u + color * 2u) return true;
        for(unsigned direction = 0; direction <= 1; direction++) {
            for(unsigned speed = 0; speed <= 3; speed++) {
                if(pp == 0x81u + color * 0x10u + direction * 8u + speed * 2u) return true;
            }
        }
    }
    return false;
}

/* multiboot-and-save.md §2.1: the logo's first word, 0x96 at 0xB2, the complement at 0xBD. */
static bool fake_bios_header_ok(const uint8_t* h) {
    if(h[4] != 0x24 || h[5] != 0xFF || h[6] != 0xAE || h[7] != 0x51) return false;
    if(h[0xB2] != 0x96) return false;
    uint8_t chk = 0;
    for(unsigned i = 0xA0; i <= 0xBC; i++) chk = (uint8_t)(chk - h[i]);
    return h[0xBD] == (uint8_t)(chk - 0x19u);
}

static void fake_bios_violation(FakeBios* b, const char* why) {
    b->error = why;
    b->step = StepDetect;
    b->send = 0x0000;
}

static void fake_bios_boot(FakeBios* b) {
    b->phase = FakeBiosBooted;
    b->boots++;
}

/* The parent's word, after this transfer's reply went out. */
static void fake_bios_handle(FakeBios* b, uint16_t word, uint64_t end_us) {
    switch(b->step) {
    case StepDetect:
        if(word == 0x6200) {
            b->send = FAKE_BIOS_READY;
        } else if(word == (0x6100 | FAKE_BIOS_CHILD_BIT) && b->prev_reply == FAKE_BIOS_READY) {
            b->step = StepHeader;
            b->count = 0;
            b->send = (uint16_t)(FAKE_BIOS_HEADER_HALFWORDS << 8 | FAKE_BIOS_CHILD_BIT);
        } else {
            fake_bios_violation(b, "detection: not 0x6200, or 0x6102 without a 0x7202 reply");
        }
        break;
    case StepHeader:
        b->mem[2 * b->count] = (uint8_t)word;
        b->mem[2 * b->count + 1] = (uint8_t)(word >> 8);
        b->count++;
        b->send = (uint16_t)((FAKE_BIOS_HEADER_HALFWORDS - b->count) << 8 | FAKE_BIOS_CHILD_BIT);
        if(b->count < FAKE_BIOS_HEADER_HALFWORDS) break;
        if(fake_bios_header_ok(b->mem)) {
            b->step = StepHeaderEnd;
        } else {
            fake_bios_violation(b, "header: logo, 0x96 or complement");
        }
        break;
    case StepHeaderEnd:
        if(word == 0x6200) {
            b->send = FAKE_BIOS_READY;
            b->step = StepInfo;
        } else {
            fake_bios_violation(b, "header end: not 0x6200");
        }
        break;
    case StepInfo:
        if(word == (0x6200 | FAKE_BIOS_CHILD_BIT)) {
            b->busy_until_us = end_us + b->palette_busy_us;
            b->send = end_us < b->busy_until_us ? FAKE_BIOS_READY : (uint16_t)(0x7300 | b->cc);
            b->step = StepPalette;
        } else {
            fake_bios_violation(b, "not 0x6202");
        }
        break;
    case StepPalette:
        if((word & 0xFF00) == 0x6300 && fake_bios_palette_ok((uint8_t)word)) {
            b->pp = (uint8_t)word;
            b->send = end_us < b->busy_until_us ? FAKE_BIOS_READY : (uint16_t)(0x7300 | b->cc);
        } else if(
            (word & 0xFF00) == 0x6400 && b->prev_reply == (0x7300 | b->cc) &&
            (uint8_t)word == (uint8_t)(0x11u + b->cc + 0xFFu + 0xFFu)) {
            b->hh = (uint8_t)word;
            b->send = (uint16_t)(0x7300 | b->rr);
            b->handshake_end_us = end_us;
            b->step = StepLength;
        } else {
            fake_bios_violation(b, "palette: not 0x63pp (documented pp), nor 0x64hh after 0x73cc");
        }
        break;
    case StepLength: {
        const uint32_t len = (uint32_t)word * 4u + 0x190u;
        const uint32_t payload = len - 0xC0u;
        if(payload < 0x100u || payload > 0x3FF40u || payload % 0x10u) {
            fake_bios_violation(b, "length word");
            break;
        }
        b->len = len;
        b->pos = 0xC0;
        b->seed = b->pp | (uint32_t)b->cc << 8 | 0xFFFF0000u;
        b->crc = 0xFFF8;
        b->send = (uint16_t)b->pos;
        b->step = StepData;
        break;
    }
    case StepData:
        if(b->pos % 4u == 0) {
            b->lo = word;
        } else {
            const uint32_t ptr = b->pos - 2u;
            const uint32_t e = b->lo | (uint32_t)word << 16;
            b->seed = b->seed * 0x6F646573u + 1u;
            const uint32_t w = e ^ (0xFE000000u - ptr) ^ b->seed ^ 0x6465646Fu;
            b->crc = fake_bios_crc(b->crc, w);
            for(unsigned i = 0; i < 4; i++) b->mem[ptr + i] = (uint8_t)(w >> (8 * i));
        }
        b->pos += 2;
        b->send = (uint16_t)b->pos; /* the next offset it expects */
        if(b->pos == b->len) {
            b->final_started = false;
            b->step = StepFinal;
        }
        break;
    case StepFinal:
        if(word == 0x0065) {
            if(!b->final_started) {
                b->final_started = true;
                b->busy_until_us = end_us + b->final_busy_us;
            }
            b->send = end_us < b->busy_until_us ? 0x0074 : 0x0075;
        } else if(word == 0x0066 && b->prev_reply == 0x0075) {
            uint32_t c = fake_bios_crc(b->crc, b->hh | (uint32_t)b->rr << 8 | 0xFFFF0000u);
            if(b->bad_crc) c++;
            b->crc_final = (uint16_t)c;
            b->send = b->crc_final;
            b->step = StepCrc;
        } else {
            fake_bios_violation(b, "final: not 0x0065, or 0x0066 without a 0x0075 reply");
        }
        break;
    case StepCrc:
        if(word == b->crc_final) {
            fake_bios_boot(b);
        } else {
            fake_bios_violation(b, "CRC mismatch");
        }
        break;
    default:
        fake_bios_violation(b, "unknown step");
        break;
    }
}

static void fake_bios_xfer(FakeBios* b, uint16_t word, uint64_t start_us, MlinkRx* rx, uint8_t t) {
    b->transfers++;
    if(b->phase != FakeBiosWaiting || b->startup) {
        if(b->startup) b->startup--;
        rx->word[t] = MLINK_NO_DATA;
        rx->timeout_mask |= (uint16_t)(1u << t);
        return;
    }
    const uint16_t reply = b->send;
    rx->word[t] = b->transfers == b->corrupt_at ? (uint16_t)(reply ^ 0x0100u) : reply;
    const uint64_t end_us = start_us + FAKE_BIOS_XFER_US;
    if(b->has_last && start_us < b->last_end_us + FAKE_BIOS_GAP_US) {
        fake_bios_violation(b, "less than 36 us after the previous transfer");
    } else if(b->step == StepLength && start_us < b->handshake_end_us + FAKE_BIOS_SILENT_US) {
        fake_bios_violation(b, "a transfer inside the 1/16 s wait");
    } else {
        fake_bios_handle(b, word, end_us);
    }
    b->prev_reply = reply;
    b->has_last = true;
    b->last_end_us = end_us;
}

void fake_bios_init(FakeBios* b, uint8_t cc, uint8_t rr) {
    memset(b, 0, sizeof(*b));
    b->phase = FakeBiosOff;
    b->cc = cc;
    b->rr = rr;
}

void fake_bios_power_on(FakeBios* b) {
    b->phase = FakeBiosWaiting;
    b->startup = 2;
    b->step = StepDetect;
    b->send = 0x0000;
    b->prev_reply = FAKE_BIOS_NO_REPLY;
    b->has_last = false;
    b->transfers = 0;
    b->len = 0;
}

void fake_bios_set_busy(FakeBios* b, uint32_t palette_us, uint32_t final_us) {
    b->palette_busy_us = palette_us;
    b->final_busy_us = final_us;
}

void fake_bios_corrupt_reply(FakeBios* b, uint32_t transfer) {
    b->corrupt_at = transfer;
}

void fake_bios_force_bad_crc(FakeBios* b, bool on) {
    b->bad_crc = on;
}

void fake_bios_frame(FakeBios* b, const MlinkTx* tx, MlinkRx* rx) {
    memset(rx, 0, sizeof(*rx));
    uint8_t count = tx->count;
    if(count > MLINK_BURST_LEN) {
        b->error = "more than MLINK_BURST_LEN words in a frame";
        count = MLINK_BURST_LEN;
    }
    rx->count = count;
    for(uint8_t t = 0; t < count; t++) {
        fake_bios_xfer(b, tx->word[t], b->now_us + (uint64_t)t * MLINK_TICK_US, rx, t);
    }
    b->now_us += (uint64_t)(count ? count : 1u) * MLINK_TICK_US;
}

FakeBiosPhase fake_bios_phase(const FakeBios* b) {
    return b->phase;
}

const uint8_t* fake_bios_image(const FakeBios* b, uint32_t* len) {
    if(len) *len = b->len;
    return b->mem;
}

uint32_t fake_bios_boots(const FakeBios* b) {
    return b->boots;
}

const char* fake_bios_error(const FakeBios* b) {
    return b->error;
}
