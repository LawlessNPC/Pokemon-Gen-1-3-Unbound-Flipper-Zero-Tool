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

/* CART_INFO: 96 header words; flags; ROM size (2 words); library kind; its offset (2 words). */
static uint16_t hp_cart_info(Hp* hp) {
    hp_cart_scan(&hp->cart);
    hp->cart_scanned = true;
    const HpCart* c = &hp->cart;
    uint16_t* p = hp->resp + 3;
    for(uint32_t k = 0; k < 96; k++) p[k] = hp_hal_rom16(2u * k);
    p[96] =
        (uint16_t)((c->logo_ok ? HP_CART_LOGO_OK : 0u) | (c->fixed_ok ? HP_CART_FIXED_OK : 0u) |
                   (c->complement_ok ? HP_CART_COMPLEMENT_OK : 0u) |
                   (c->other_lib ? HP_CART_OTHER_LIB : 0u) |
                   (c->write_game ? HP_CART_WRITE_GAME : 0u));
    p[97] = (uint16_t)c->rom_size;
    p[98] = (uint16_t)(c->rom_size >> 16);
    p[99] = (uint16_t)c->lib;
    p[100] = (uint16_t)c->lib_off;
    p[101] = (uint16_t)(c->lib_off >> 16);
    return hp_respond(hp, HP_ST_OK, 102);
}

/* SAVE_PROBE: flash ID; class; pre0 | pre1 << 8; id0 | id1 << 8; SRAM. The game code and version
 * come from the cart header itself, whether CART_INFO ran or not. */
static uint16_t hp_save_probe(Hp* hp) {
    if(!hp->probed) {
        const uint16_t c01 = hp_hal_rom16(0xAC);
        const uint16_t c23 = hp_hal_rom16(0xAE);
        const uint8_t code[4] = {
            (uint8_t)c01, (uint8_t)(c01 >> 8), (uint8_t)c23, (uint8_t)(c23 >> 8)};
        hp_flash_probe(&hp->probe, code, (uint8_t)hp_hal_rom16(0xBC));
        hp->probed = true;
    }
    const HpProbe* pr = &hp->probe;
    uint16_t* p = hp->resp + 3;
    p[0] = pr->id;
    p[1] = (uint16_t)pr->cls;
    p[2] = (uint16_t)(pr->pre0 | pr->pre1 << 8);
    p[3] = (uint16_t)(pr->id0 | pr->id1 << 8);
    p[4] = pr->sram ? 1u : 0u;
    return hp_respond(hp, HP_ST_OK, 5);
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
    case HP_CMD_CART_INFO:
        return hp_cart_info(hp);
    case HP_CMD_SAVE_PROBE:
        return hp_save_probe(hp);
    default:
        return hp_fail(hp, HP_ST_UNSUPPORTED, 0, 0);
    }
}
