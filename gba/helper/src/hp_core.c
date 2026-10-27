#include "hp_core.h"
#include "hp_hal.h"
#include "hp_version.h"
#include <string.h>

/* WRITE_SECTOR's request: sector, flags, then the 4 KB image as 2048 words */
#define HP_REQ_WRITE_SECTOR 2050u
#define HP_REQ_UNKNOWN 0x10000u /* never equal to a 16-bit n */

_Static_assert(HP_REQ_WRITE_SECTOR <= HP_MAX_PAYLOAD, "the longest request fits Hp.req");

/* A command's fixed request length in words, checked at w2 so that a false SOF swallows at most
 * two words; HP_REQ_UNKNOWN for a code the helper doesn't know. */
static uint32_t hp_req_len(uint8_t code) {
    switch(code) {
    case HP_CMD_HELLO:
    case HP_CMD_CART_INFO:
    case HP_CMD_SAVE_PROBE:
    case HP_CMD_DISARM:
    case HP_CMD_READ_FOOTERS:
        return 0;
    case HP_CMD_READ_SECTOR:
    case HP_CMD_SECTOR_CRC32:
    case HP_CMD_ERASE_SECTOR:
    case HP_CMD_COMMIT_SECTOR:
        return 1;
    case HP_CMD_ROM_READ:
        return 3;
    case HP_CMD_ROM_CRC32:
        return 4;
    case HP_CMD_COPY_SECTOR:
        return 5;
    case HP_CMD_ARM_WRITE:
        return 6;
    case HP_CMD_WRITE_SECTOR:
        return HP_REQ_WRITE_SECTOR;
    default:
        return HP_REQ_UNKNOWN;
    }
}

void hp_init(Hp* hp) {
    memset(hp, 0, sizeof(*hp));
}

/* A SOF arrived: w1 comes next. */
static uint16_t hp_rx_start(Hp* hp) {
    hp->rx_pos = 1;
    hp->rx_crc = 0xFFFFFFFFu;
    return HP_RECV;
}

/* One word of a packet after its SOF. */
static uint16_t hp_rx_word(Hp* hp, uint16_t in) {
    const uint32_t k = hp->rx_pos;
    /* 0x55AA is neither a known w1 (code 0xAA) nor a valid w2 (n over HP_MAX_PAYLOAD): there it
     * can only be a SOF, and the one before it was false. */
    if(k <= 2u && in == HP_SOF) return hp_rx_start(hp);
    if(k == 1u) {
        hp->seq = (uint8_t)(in >> 8);
        hp->code = (uint8_t)in;
        hp->rx_crc = hp_crc32_word(hp->rx_crc, in);
    } else if(k == 2u) {
        if(in != hp_req_len(hp->code)) {
            hp->rx_pos = 0;
            return HP_READY;
        }
        hp->rx_n = in;
        hp->rx_crc = hp_crc32_word(hp->rx_crc, in);
    } else if(k < 3u + hp->rx_n) {
        hp->req[k - 3u] = in;
        hp->rx_crc = hp_crc32_word(hp->rx_crc, in);
    } else if(k == 3u + hp->rx_n) {
        hp->rx_crc_lo = in;
    } else {
        hp->rx_pos = 0;
        if((hp->rx_crc_lo | (uint32_t)in << 16) != ~hp->rx_crc) return HP_READY;
        hp->pending = true;
        return HP_BUSY;
    }
    hp->rx_pos++;
    return HP_RECV;
}

uint16_t hp_step(Hp* hp, uint16_t in, uint8_t flags) {
    if(hp->pending) return HP_BUSY;
    if(hp->tx_len) {
        /* Nothing went out: the Flipper stopped clocking. It sends the request again once it sees
         * HP_READY. */
        if(flags & HP_STEP_TIMEOUT) {
            hp->tx_len = 0;
            return HP_READY;
        }
        const uint16_t w = hp->resp[hp->tx_pos++];
        if(hp->tx_pos == hp->tx_len) hp->tx_len = 0;
        return w;
    }
    if(flags & (HP_STEP_ERR | HP_STEP_TIMEOUT)) {
        hp->rx_pos = 0;
        return HP_READY;
    }
    if(hp->rx_pos) return hp_rx_word(hp, in);
    return in == HP_SOF ? hp_rx_start(hp) : HP_READY;
}

bool hp_exec_pending(const Hp* hp) {
    return hp->pending;
}

/* Queues the response whose n payload words are already at resp[3]: the header words and the
 * CRC. */
static uint16_t hp_respond(Hp* hp, uint8_t status, uint16_t n) {
    uint16_t* r = hp->resp;
    r[0] = HP_SOF;
    r[1] = (uint16_t)(hp->seq << 8 | status);
    r[2] = n;
    uint32_t crc = 0xFFFFFFFFu;
    for(uint32_t i = 1; i < 3u + n; i++) crc = hp_crc32_word(crc, r[i]);
    crc = ~crc;
    r[3u + n] = (uint16_t)crc;
    r[4u + n] = (uint16_t)(crc >> 16);
    hp->tx_len = (uint16_t)(5u + n);
    hp->tx_pos = 1;
    return HP_SOF;
}

static uint16_t hp_fail(Hp* hp, uint8_t status, uint16_t code, uint16_t off) {
    hp->resp[3] = code;
    hp->resp[4] = off;
    return hp_respond(hp, status, 2);
}

static uint16_t hp_hello(Hp* hp) {
    const uint8_t* own = hp_hal_own_header();
    uint16_t* p = hp->resp + 3;
    p[0] = HP_PROTO_VERSION;
    p[1] = HP_BUILD_ID;
    p[2] = HP_MAX_PAYLOAD;
    p[3] = (uint16_t)(own[0xC4] | own[0xC5] << 8);
    return hp_respond(hp, HP_ST_OK, 4);
}

uint16_t hp_execute(Hp* hp) {
    if(!hp->pending) return HP_READY;
    hp->pending = false;
    switch(hp->code) {
    case HP_CMD_HELLO:
        return hp_hello(hp);
    default:
        return hp_fail(hp, HP_ST_UNSUPPORTED, 0, 0);
    }
}
