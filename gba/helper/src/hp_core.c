#include "hp_core.h"
#include "hp_hal.h"
#include "hp_save.h"
#include "hp_version.h"
#include <string.h>

/* WRITE_SECTOR's request: sector, flags, then the 4 KB image as 2048 words */
#define HP_REQ_WRITE_SECTOR 2050u
#define HP_REQ_UNKNOWN 0x10000u /* never equal to a 16-bit n */
#define HP_SECTOR_WORDS (HP_FLASH_SECTOR_LEN / 2u)
/* OMEGA_WRITE_SECTOR's request: sector, then the 4 KB image as 2048 words */
#define HP_REQ_OMEGA_WRITE (1u + HP_SECTOR_WORDS)
#define HP_OMEGA_SECTORS 32u /* 4 KB sectors in the save file, 128 KB */

_Static_assert(HP_REQ_WRITE_SECTOR <= HP_MAX_PAYLOAD, "the longest request fits Hp.req");
_Static_assert(HP_REQ_OMEGA_WRITE <= HP_MAX_PAYLOAD, "OMEGA_WRITE_SECTOR's request fits Hp.req");
_Static_assert(HP_SECTOR_WORDS <= HP_MAX_PAYLOAD, "READ_SECTOR's response fits Hp.resp");

/* Sectors 0..27 hold the two save slots; 28..31 (Hall of Fame, then e-Reader or Trainer Tower
 * data) are never written (multiboot-and-save.md §5.1). */
#define HP_SLOT_SECTORS_END 28u
#define HP_SIG_OFF 0xFF8u /* the signature, whose first byte is the commit byte */
#define HP_COUNTER_OFF 0xFFCu
#define HP_NONE 0xFFu /* no sector */

/* The gates return 0 to go ahead, or the fail response's first word */
_Static_assert(HP_SOF != 0, "a queued response is never 0");

/* ROM_READ and ROM_CRC32: the cart bus's ROM window 0x08000000..0x09FFFFFF */
#define HP_ROM_BASE 0x08000000u
#define HP_ROM_WINDOW 0x02000000u
#define HP_ROM_READ_MAX HP_SECTOR_WORDS /* words */
#define HP_ROM_CRC_MAX 0x40000u /* bytes: H_BUSY well under the Flipper's 6 s */
#define HP_ROM_CHUNK 16u /* halfwords per CRC step */

/* A command's fixed request length in words, checked at w2 so that a false SOF swallows at most
 * two words; HP_REQ_UNKNOWN for a code the helper doesn't know. */
static uint32_t hp_req_len(uint8_t code) {
    switch(code) {
    case HP_CMD_HELLO:
    case HP_CMD_CART_INFO:
    case HP_CMD_SAVE_PROBE:
    case HP_CMD_DISARM:
    case HP_CMD_READ_FOOTERS:
    case HP_CMD_OMEGA_MOUNT:
        return 0;
    case HP_CMD_READ_SECTOR:
    case HP_CMD_SECTOR_CRC32:
    case HP_CMD_ERASE_SECTOR:
    case HP_CMD_COMMIT_SECTOR:
        return 1;
    case HP_CMD_ROM_READ:
        return 3;
    case HP_CMD_OMEGA_READ:
        return 4;
    case HP_CMD_OMEGA_CRC32:
        return 5;
    case HP_CMD_OMEGA_SHA256:
        return 6;
    case HP_CMD_OMEGA_ARM:
        return 5;
    case HP_CMD_OMEGA_WRITE_SECTOR:
        return HP_REQ_OMEGA_WRITE;
    case HP_CMD_OMEGA_OPEN:
        return 2u + HP_OMEGA_PATH_MAX / 2u;
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
    hp_omega_init(&hp->omega);
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

/* Ends any arming: the save chip's, and the Omega's (DISARM is the Flipper's way to end both) */
static void hp_disarm(Hp* hp) {
    hp->armed = false;
    hp->programmed = 0;
    hp->completed = 0;
    hp->omega_armed = false;
}

/* CART_INFO: 96 header words; flags; ROM size (2 words); library kind; its offset (2 words). The
 * cart may have changed since an arming checked it, or since the save chip was probed: it ends any
 * arming, and forgets the probe, so no flash command reaches the chip until SAVE_PROBE has looked
 * at it again (an SRAM cart swapped in would take a bank select as data). */
static uint16_t hp_cart_info(Hp* hp) {
    hp_disarm(hp);
    hp_omega_init(&hp->omega);
    hp->sha_on = false;
    hp->probed = false;
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

/* HELLO starts a session: nothing stays armed, and no erase from before it lets a write skip its
 * own. */
static uint16_t hp_hello(Hp* hp) {
    hp_disarm(hp);
    hp_omega_init(&hp->omega);
    hp->sha_on = false;
    hp->flash.erased_ok = 0;
    const uint8_t* own = hp_hal_own_header();
    uint16_t* p = hp->resp + 3;
    p[0] = HP_PROTO_VERSION;
    p[1] = HP_BUILD_ID;
    p[2] = HP_MAX_PAYLOAD;
    p[3] = (uint16_t)(own[0xC4] | own[0xC5] << 8);
    return hp_respond(hp, HP_ST_OK, 4);
}

/* ---- The save chip ---- */

/* The probe found a 128 KB flash: the bank command, which every access after the probe starts
 * with, can't land as data in an SRAM. */
static bool hp_flash128(const Hp* hp) {
    return hp->probed && hp->probe.cls == HpChipFlash128 && !hp->probe.sram;
}

/* multiboot-and-save.md §3.6 checks 1-4, from CART_INFO's and SAVE_PROBE's last results: 0 if the
 * cart may be written, else the HP_REFUSE_* reason. The game code must also name a save layout
 * the helper knows, which the slot checks need. */
static uint16_t hp_write_refusal(const Hp* hp) {
    const HpCart* c = &hp->cart;
    if(!hp->cart_scanned || !c->logo_ok || !c->fixed_ok || !c->complement_ok || !c->write_game ||
       c->lib != HpLibFlash1M || c->other_lib) {
        return HP_REFUSE_CART;
    }
    if(hp_save_game(c->code) == HpSaveGameUnknown) return HP_REFUSE_LAYOUT;
    if(!hp_flash128(hp) || (hp->probe.id != HP_ID_MX && hp->probe.id != HP_ID_SANYO)) {
        return HP_REFUSE_CHIP;
    }
    return 0;
}

/* The fail response for a flash read the chip can't take, or for a sector over 31; 0 if the read
 * may go ahead. */
static uint16_t hp_read_gate(Hp* hp, uint16_t sector) {
    if(!hp_flash128(hp)) return hp_fail(hp, HP_ST_UNSUPPORTED, 0, 0);
    if(sector >= HP_FLASH_SECTORS) return hp_fail(hp, HP_ST_BAD_ARG, 0, sector);
    return 0;
}

static uint16_t hp_read_sector(Hp* hp) {
    const uint16_t sector = hp->req[0];
    const uint16_t refused = hp_read_gate(hp, sector);
    if(refused) return refused;
    hp_flash_read((uint8_t)sector, 0, hp->sector_buf, HP_FLASH_SECTOR_LEN);
    for(uint32_t k = 0; k < HP_SECTOR_WORDS; k++) {
        hp->resp[3 + k] = (uint16_t)(hp->sector_buf[2 * k] | hp->sector_buf[2 * k + 1] << 8);
    }
    return hp_respond(hp, HP_ST_OK, HP_SECTOR_WORDS);
}

/* Per sector: id, checksum, signature (2 words), counter (2 words), full-length checksum, 0 */
static uint16_t hp_read_footers(Hp* hp) {
    const uint16_t refused = hp_read_gate(hp, 0);
    if(refused) return refused;
    uint16_t* p = hp->resp + 3;
    for(uint32_t s = 0; s < HP_FLASH_SECTORS; s++, p += 8) {
        HpFooter f;
        hp_save_footer((uint8_t)s, &f);
        p[0] = f.id;
        p[1] = f.checksum;
        p[2] = (uint16_t)f.signature;
        p[3] = (uint16_t)(f.signature >> 16);
        p[4] = (uint16_t)f.counter;
        p[5] = (uint16_t)(f.counter >> 16);
        p[6] = f.full_checksum;
        p[7] = 0;
    }
    return hp_respond(hp, HP_ST_OK, 8u * HP_FLASH_SECTORS);
}

static uint16_t hp_sector_crc32(Hp* hp) {
    const uint16_t sector = hp->req[0];
    const uint16_t refused = hp_read_gate(hp, sector);
    if(refused) return refused;
    const uint32_t crc = hp_flash_crc32((uint8_t)sector);
    hp->resp[3] = (uint16_t)crc;
    hp->resp[4] = (uint16_t)(crc >> 16);
    return hp_respond(hp, HP_ST_OK, 2);
}

/* ARM_WRITE: magic, flash id, target slot T, mode, N (2 words). Whatever the outcome, it first ends
 * any arming and forgets this session's erases, so nothing from before it (another cart, another
 * plan) can carry into the writes it allows. Which slot the game loads is judged with the game's
 * own section sizes, so a slot the game takes is never written, whatever its padding.
 * - Mode 0 writes a new save over the older slot: the game must load slot 1-T, and it must be one
 *   clean save (hp_save_slot_ok) with counter N, whose parity is its own (the game writes even
 *   counters to slot 0, odd to slot 1: pokeruby: src/save.c: WriteSingleChunk).
 * - Mode 1 restores onto a cart with no valid save: the game must load neither slot, and neither
 *   may be a clean save. */
static uint16_t hp_arm_write(Hp* hp) {
    const uint16_t* a = hp->req;
    hp_disarm(hp);
    hp->flash.erased_ok = 0;
    if(a[0] != HP_ARM_MAGIC || a[2] > 1u || a[3] > 1u) return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    const uint16_t why = hp_write_refusal(hp);
    if(why) return hp_fail(hp, HP_ST_REFUSED, why, 0);
    if(a[1] != hp->probe.id) return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_ID, 0);
    const uint8_t t = (uint8_t)a[2];
    const uint8_t keep = (uint8_t)(1u - t);
    const uint32_t n = a[4] | (uint32_t)a[5] << 16;
    const int loads = hp_save_active_slot(hp_save_game(hp->cart.code));
    uint32_t ctr = 0;
    bool ok;
    if(a[3] == 0) {
        ok = loads == keep && hp_save_slot_ok(keep, &ctr) && ctr == n && n % 2u == keep;
    } else {
        ok = loads == -1 && !hp_save_slot_ok(0, &ctr) && !hp_save_slot_ok(1, &ctr);
    }
    if(!ok) return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_SLOT, 0);
    hp->armed = true;
    hp->arm_slot = t;
    return hp_respond(hp, HP_ST_OK, 0);
}

static uint16_t hp_disarm_cmd(Hp* hp) {
    hp_disarm(hp);
    return hp_respond(hp, HP_ST_OK, 0);
}

/* The slot of a sector under 28 (no division: the helper links no divide routine) */
static uint8_t hp_sector_slot(uint16_t sector) {
    return sector >= HP_SAVE_SLOT_SECTORS ? 1u : 0u;
}

/* What every ERASE, WRITE, COPY and COMMIT passes before it touches the chip: the fail response,
 * or 0 if the sector is in the armed slot. */
static uint16_t hp_write_gate(Hp* hp, uint16_t sector) {
    const uint16_t why = hp_write_refusal(hp);
    if(why) return hp_fail(hp, HP_ST_REFUSED, why, sector);
    if(sector >= HP_SLOT_SECTORS_END) return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_SECTOR, sector);
    if(!hp->armed) return hp_fail(hp, HP_ST_NOT_ARMED, 0, sector);
    if(hp_sector_slot(sector) != hp->arm_slot) {
        return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_SECTOR, sector);
    }
    return 0;
}

static uint16_t hp_slot_bit(const Hp* hp, uint8_t sector) {
    return (uint16_t)(1u << (sector - hp->arm_slot * HP_SAVE_SLOT_SECTORS));
}

/* The game ignores the armed slot until all 14 of its ids are valid (multiboot-and-save.md §5.5).
 * So that it can only become valid as a whole save this arming wrote, COMMIT_SECTOR completes only
 * a sector this arming programmed, and only while every other sector of the slot either was
 * completed since the arming or holds no valid signature. HP_NONE if so, else the first sector in
 * the way. */
static uint8_t hp_slot_in_way(Hp* hp, uint8_t sector) {
    static const uint8_t sig[4] = {0x25, 0x20, 0x01, 0x08};
    const uint8_t first = (uint8_t)(hp->arm_slot * HP_SAVE_SLOT_SECTORS);
    for(uint8_t s = first; s < first + HP_SAVE_SLOT_SECTORS; s++) {
        if(s == sector || (hp->completed & hp_slot_bit(hp, s))) continue;
        uint8_t b[4];
        hp_flash_read(s, HP_SIG_OFF, b, sizeof(b));
        if(memcmp(b, sig, sizeof(b)) == 0) return s;
    }
    return HP_NONE;
}

/* The response to a flash routine's result: any failure ends the arming. */
static uint16_t hp_flash_answer(Hp* hp, HpFlashResult r) {
    if(r.status == HP_ST_OK) return hp_respond(hp, HP_ST_OK, 0);
    hp_disarm(hp);
    return hp_fail(hp, r.status, r.code, r.off);
}

/* The sector now holds a complete signature. Once all 14 do, the slot is a whole new save, likely
 * the one the game loads now: nothing more may be written to it. */
static void hp_completed(Hp* hp, uint8_t sector) {
    hp->completed |= hp_slot_bit(hp, sector);
    if(hp->completed == (1u << HP_SAVE_SLOT_SECTORS) - 1u) hp_disarm(hp);
}

/* Erasing or writing a sector undoes what the arming knew about it. */
static void hp_forget(Hp* hp, uint8_t sector) {
    const uint16_t bit = hp_slot_bit(hp, sector);
    hp->programmed &= (uint16_t)~bit;
    hp->completed &= (uint16_t)~bit;
}

static uint16_t hp_erase_sector(Hp* hp) {
    const uint16_t sector = hp->req[0];
    const uint16_t refused = hp_write_gate(hp, sector);
    if(refused) return refused;
    hp_forget(hp, (uint8_t)sector);
    return hp_flash_answer(hp, hp_flash_erase(&hp->flash, (uint8_t)sector));
}

/* Writes sector_buf into the (gated) sector with 0xFF8 left erased, for COMMIT_SECTOR. */
static uint16_t hp_program(Hp* hp, uint8_t sector) {
    hp_forget(hp, sector);
    const HpFlashResult r = hp_flash_program(&hp->flash, sector, hp->sector_buf, true);
    if(r.status == HP_ST_OK) hp->programmed |= hp_slot_bit(hp, sector);
    return hp_flash_answer(hp, r);
}

/* Writes always defer the commit byte: then a sector's signature, the last byte programmed, is only
 * ever valid over a complete sector, its counter included (multiboot-and-save.md §6.8). */
static bool hp_flags_ok(uint16_t flags) {
    return flags == HP_FLAG_DEFER_COMMIT;
}

/* WRITE_SECTOR: sector, flags, the image. The packet's CRC passed before this runs, so a torn
 * upload never reaches the chip. */
static uint16_t hp_write_sector(Hp* hp) {
    const uint16_t sector = hp->req[0];
    const uint16_t refused = hp_write_gate(hp, sector);
    if(refused) return refused;
    if(!hp_flags_ok(hp->req[1])) return hp_fail(hp, HP_ST_BAD_ARG, 0, sector);
    const uint16_t* w = hp->req + 2;
    for(uint32_t k = 0; k < HP_SECTOR_WORDS; k++) {
        hp->sector_buf[2 * k] = (uint8_t)w[k];
        hp->sector_buf[2 * k + 1] = (uint8_t)(w[k] >> 8);
    }
    return hp_program(hp, (uint8_t)sector);
}

/* COPY_SECTOR: src, dst, flags, counter (2 words). The source must be in the other slot, the one
 * the arming keeps; its copy gets the new counter at 0xFFC, outside the checksum. */
static uint16_t hp_copy_sector(Hp* hp) {
    const uint16_t src = hp->req[0];
    const uint16_t dst = hp->req[1];
    const uint16_t refused = hp_write_gate(hp, dst);
    if(refused) return refused;
    if(src >= HP_SLOT_SECTORS_END || hp_sector_slot(src) == hp->arm_slot ||
       !hp_flags_ok(hp->req[2])) {
        return hp_fail(hp, HP_ST_BAD_ARG, 0, src);
    }
    hp_flash_read((uint8_t)src, 0, hp->sector_buf, HP_FLASH_SECTOR_LEN);
    const uint16_t* c = hp->req + 3;
    hp->sector_buf[HP_COUNTER_OFF] = (uint8_t)c[0];
    hp->sector_buf[HP_COUNTER_OFF + 1] = (uint8_t)(c[0] >> 8);
    hp->sector_buf[HP_COUNTER_OFF + 2] = (uint8_t)c[1];
    hp->sector_buf[HP_COUNTER_OFF + 3] = (uint8_t)(c[1] >> 8);
    return hp_program(hp, (uint8_t)dst);
}

static uint16_t hp_commit_sector(Hp* hp) {
    const uint16_t sector = hp->req[0];
    const uint16_t refused = hp_write_gate(hp, sector);
    if(refused) return refused;
    if(!(hp->programmed & hp_slot_bit(hp, (uint8_t)sector))) {
        return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_NOT_PROGRAMMED, sector);
    }
    const uint8_t in_way = hp_slot_in_way(hp, (uint8_t)sector);
    if(in_way != HP_NONE) return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_VALID, in_way);
    const HpFlashResult r = hp_flash_commit(&hp->flash, (uint8_t)sector);
    if(r.status == HP_ST_OK) hp_completed(hp, (uint8_t)sector);
    return hp_flash_answer(hp, r);
}

/* ---- The cart ROM ---- */

/* addr .. addr + len - 1 inside the ROM window, addr even: true with *off its offset. */
static bool hp_rom_range(uint32_t addr, uint32_t len, uint32_t* off) {
    if(addr < HP_ROM_BASE || addr - HP_ROM_BASE >= HP_ROM_WINDOW || (addr & 1u)) return false;
    *off = addr - HP_ROM_BASE;
    return len <= HP_ROM_WINDOW - *off;
}

/* ROM_READ: address (2 words), words 1..2048 */
static uint16_t hp_rom_read(Hp* hp) {
    const uint32_t addr = hp->req[0] | (uint32_t)hp->req[1] << 16;
    const uint16_t words = hp->req[2];
    uint32_t off;
    if(words == 0 || words > HP_ROM_READ_MAX || !hp_rom_range(addr, 2u * words, &off)) {
        return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    }
    for(uint32_t k = 0; k < words; k++) hp->resp[3 + k] = hp_hal_rom16(off + 2u * k);
    return hp_respond(hp, HP_ST_OK, words);
}

/* ROM_CRC32: address (2 words), length (2 words: 2..0x40000, even) */
static uint16_t hp_rom_crc32(Hp* hp) {
    const uint32_t addr = hp->req[0] | (uint32_t)hp->req[1] << 16;
    const uint32_t len = hp->req[2] | (uint32_t)hp->req[3] << 16;
    uint32_t off;
    if(len < 2u || len > HP_ROM_CRC_MAX || (len & 1u) || !hp_rom_range(addr, len, &off)) {
        return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    }
    uint8_t chunk[2u * HP_ROM_CHUNK];
    uint32_t crc = 0xFFFFFFFFu;
    for(uint32_t done = 0; done < len;) {
        const uint32_t n = len - done < sizeof(chunk) ? len - done : sizeof(chunk);
        for(uint32_t i = 0; i < n; i += 2) {
            const uint16_t hw = hp_hal_rom16(off + done + i);
            chunk[i] = (uint8_t)hw;
            chunk[i + 1] = (uint8_t)(hw >> 8);
        }
        crc = hp_crc32_bytes(crc, chunk, n);
        done += n;
    }
    crc = ~crc;
    hp->resp[3] = (uint16_t)crc;
    hp->resp[4] = (uint16_t)(crc >> 16);
    return hp_respond(hp, HP_ST_OK, 2);
}

/* ---- The EZ-Flash Omega's microSD (multiboot-and-save.md §8) ---- */

/* A reader failure: UNSUPPORTED with its HpOmegaResult as the fail code, BAD_ARG for BadArg */
static uint16_t hp_omega_fail(Hp* hp, HpOmegaResult r) {
    if(r == HpOmegaBadArg) return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    return hp_fail(hp, HP_ST_UNSUPPORTED, (uint16_t)r, 0);
}

/* OMEGA_MOUNT: only after this session's CART_INFO found a header with an empty title and game
 * code, as the Omega's kernel has (first bytes 0xA0 and 0xAC checked), so that no Omega command
 * ever reaches a genuine cart */
static uint16_t hp_omega_mount_cmd(Hp* hp) {
    hp->omega_armed = false;
    const HpCart* c = &hp->cart;
    if(!hp->cart_scanned || c->write_game || c->title0 != 0 || c->code[0] != 0) {
        return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_CART, 0);
    }
    hp->sha_on = false;
    const HpOmegaResult r = hp_omega_mount(&hp->omega);
    if(r != HpOmegaOk) return hp_omega_fail(hp, r);
    const HpOmega* o = &hp->omega;
    uint16_t* p = hp->resp + 3;
    p[0] = (uint16_t)o->part;
    p[1] = (uint16_t)(o->part >> 16);
    p[2] = o->spc;
    p[3] = o->reserved;
    p[4] = o->fats;
    p[5] = (uint16_t)o->fatsz;
    p[6] = (uint16_t)(o->fatsz >> 16);
    p[7] = (uint16_t)o->root;
    p[8] = (uint16_t)(o->root >> 16);
    return hp_respond(hp, HP_ST_OK, 9);
}

/* OMEGA_OPEN: slot, path length 1..95, the path's bytes, 2 a word, low byte first */
static uint16_t hp_omega_open_cmd(Hp* hp) {
    if(hp->req[0] == 0) hp->omega_armed = false;
    if(!hp->omega.mounted) return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_OMEGA, 0);
    const uint16_t slot = hp->req[0], len = hp->req[1];
    if(slot >= HP_OMEGA_SLOTS || len < 1u || len >= HP_OMEGA_PATH_MAX) {
        return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    }
    char path[HP_OMEGA_PATH_MAX];
    for(uint32_t i = 0; i < len; i++) path[i] = (char)(hp->req[2u + i / 2u] >> ((i & 1u) * 8u));
    path[len] = 0;
    for(uint32_t i = 0; i < len; i++) {
        if(path[i] == 0) return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    }
    if(hp->sha_on && hp->sha_slot == slot) hp->sha_on = false;
    const HpOmegaResult r = hp_omega_open(&hp->omega, (uint8_t)slot, path);
    if(r != HpOmegaOk) return hp_omega_fail(hp, r);
    const HpOmegaFile* f = &hp->omega.files[slot];
    uint16_t* p = hp->resp + 3;
    p[0] = (uint16_t)f->size;
    p[1] = (uint16_t)(f->size >> 16);
    p[2] = (uint16_t)f->first;
    p[3] = (uint16_t)(f->first >> 16);
    p[4] = f->date;
    p[5] = f->time;
    return hp_respond(hp, HP_ST_OK, 6);
}

/* OMEGA_READ: slot, offset (2 words), blocks 1..8 */
static uint16_t hp_omega_read_cmd(Hp* hp) {
    if(!hp->omega.mounted) return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_OMEGA, 0);
    const uint32_t off = hp->req[1] | (uint32_t)hp->req[2] << 16;
    const uint16_t blocks = hp->req[3];
    if(hp->req[0] >= HP_OMEGA_SLOTS || blocks < 1u || blocks > HP_OMEGA_READ_MAX) {
        return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    }
    const HpOmegaResult r =
        hp_omega_read(&hp->omega, (uint8_t)hp->req[0], off, (uint8_t)blocks, &hp->resp[3]);
    if(r != HpOmegaOk) return hp_omega_fail(hp, r);
    return hp_respond(hp, HP_ST_OK, (uint16_t)(blocks * 256u));
}

/* Feeds len bytes of slot's file from off, in runs of up to 4 blocks a card read, block by block
 * through sector_buf, to the CRC-32 register *crc and/or the SHA-256 hash; off % 512 == 0, and
 * only the range's end may be short of a block, at the file's end */
static HpOmegaResult
    hp_omega_feed(Hp* hp, uint8_t slot, uint32_t off, uint32_t len, uint32_t* crc, bool sha) {
    uint16_t* words = &hp->resp[3]; /* a block's words, then its bytes into sector_buf */
    for(uint32_t done = 0; done < len;) {
        const uint32_t blocks = (len - done + 511u) / 512u < 4u ? (len - done + 511u) / 512u : 4u;
        const HpOmegaResult r = hp_omega_read(&hp->omega, slot, off + done, (uint8_t)blocks, words);
        if(r != HpOmegaOk) return r;
        for(uint32_t k = 0; k < blocks; k++) {
            const uint32_t n = len - done < 512u ? len - done : 512u;
            for(uint32_t i = 0; i < 256u; i++) {
                const uint16_t w = words[k * 256u + i];
                hp->sector_buf[2u * i] = (uint8_t)w;
                hp->sector_buf[2u * i + 1u] = (uint8_t)(w >> 8);
            }
            if(crc) *crc = hp_crc32_bytes(*crc, hp->sector_buf, n);
            if(sha) hp_sha256_update(&hp->sha, hp->sector_buf, n);
            done += n;
        }
    }
    return HpOmegaOk;
}

/* OMEGA_CRC32: slot, offset (2 words), length (2 words: 512..0x40000, a multiple of 512) */
static uint16_t hp_omega_crc32_cmd(Hp* hp) {
    if(!hp->omega.mounted) return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_OMEGA, 0);
    const uint32_t off = hp->req[1] | (uint32_t)hp->req[2] << 16;
    const uint32_t len = hp->req[3] | (uint32_t)hp->req[4] << 16;
    if(hp->req[0] >= HP_OMEGA_SLOTS || len < 512u || len > HP_OMEGA_CRC_MAX || (len % 512u) ||
       (off % 512u)) {
        return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    }
    uint32_t crc = 0xFFFFFFFFu;
    const HpOmegaResult r = hp_omega_feed(hp, (uint8_t)hp->req[0], off, len, &crc, false);
    if(r != HpOmegaOk) return hp_omega_fail(hp, r);
    crc = ~crc;
    hp->resp[3] = (uint16_t)crc;
    hp->resp[4] = (uint16_t)(crc >> 16);
    return hp_respond(hp, HP_ST_OK, 2);
}

/* OMEGA_SHA256: slot, flags, offset (2 words), length (2 words, at most HP_SHA_STEP_MAX). INIT at
 * offset 0 starts the hash; without it the call goes on from the last one's end, on its slot.
 * Only a range that ends at the file's size may end short of a block. FINAL answers the digest. */
static uint16_t hp_omega_sha_cmd(Hp* hp) {
    if(!hp->omega.mounted) return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_OMEGA, 0);
    const uint16_t slot = hp->req[0], flags = hp->req[1];
    const uint32_t off = hp->req[2] | (uint32_t)hp->req[3] << 16;
    const uint32_t len = hp->req[4] | (uint32_t)hp->req[5] << 16;
    if(slot >= HP_OMEGA_SLOTS || !hp->omega.files[slot].used || (flags & ~(HP_SHA_INIT | HP_SHA_FINAL)) ||
       len > HP_SHA_STEP_MAX || (off % 512u)) {
        return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    }
    const uint32_t size = hp->omega.files[slot].size;
    const bool to_end = (uint64_t)off + len == size;
    if((uint64_t)off + len > size || ((len % 512u) && !to_end)) return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    if(flags & HP_SHA_INIT) {
        if(off != 0) return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
        hp_sha256_init(&hp->sha);
        hp->sha_on = true;
        hp->sha_slot = (uint8_t)slot;
    } else if(!hp->sha_on || hp->sha_slot != slot || hp->sha_next != off) {
        return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    }
    const HpOmegaResult r = hp_omega_feed(hp, (uint8_t)slot, off, len, NULL, true);
    if(r != HpOmegaOk) {
        hp->sha_on = false;
        return hp_omega_fail(hp, r);
    }
    hp->sha_next = off + len;
    if(!(flags & HP_SHA_FINAL)) return hp_respond(hp, HP_ST_OK, 0);
    uint8_t d[32];
    hp_sha256_final(&hp->sha, d);
    hp->sha_on = false;
    for(uint32_t i = 0; i < 16u; i++) hp->resp[3u + i] = (uint16_t)(d[2u * i] | d[2u * i + 1u] << 8);
    return hp_respond(hp, HP_ST_OK, 16);
}

/* OMEGA_ARM: magic, the save's size (2 words) and first cluster (2 words). It first ends any
 * arming. REFUSED unless the card is mounted and slot 0 is open with exactly that size and first
 * cluster, and that size is a save's (32 sectors, 131072 bytes): the Flipper names the one file
 * that may be written, and the helper checks it is the file it has open. */
static uint16_t hp_omega_arm_cmd(Hp* hp) {
    const uint16_t* a = hp->req;
    hp->omega_armed = false;
    if(a[0] != HP_ARM_MAGIC) return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    const HpOmegaFile* f = &hp->omega.files[0];
    if(!hp->omega.mounted || !f->used || f->size != HP_OMEGA_SECTORS * HP_FLASH_SECTOR_LEN ||
       f->size != (a[1] | (uint32_t)a[2] << 16) || f->first != (a[3] | (uint32_t)a[4] << 16)) {
        return hp_fail(hp, HP_ST_REFUSED, HP_REFUSE_OMEGA, 0);
    }
    hp->omega_armed = true;
    return hp_respond(hp, HP_ST_OK, 0);
}

/* OMEGA_WRITE_SECTOR: sector 0..31, then the image's 2048 words. Answers the CRC-32 of the 4096
 * bytes read back from the card, once it equals the image's; VERIFY_FAIL if it doesn't (with the
 * usual two fail words, no CRC). */
static uint16_t hp_omega_write_cmd(Hp* hp) {
    if(!hp->omega_armed) return hp_fail(hp, HP_ST_NOT_ARMED, 0, 0);
    const uint16_t sector = hp->req[0];
    if(sector >= HP_OMEGA_SECTORS) return hp_fail(hp, HP_ST_BAD_ARG, 0, 0);
    const uint16_t* image = &hp->req[1];
    uint32_t want = 0xFFFFFFFFu;
    for(uint32_t i = 0; i < HP_SECTOR_WORDS; i++) want = hp_crc32_word(want, image[i]);
    want = ~want;
    uint32_t got = 0;
    hp_hal_status(HpStatusWriting);
    const HpOmegaResult r = hp_omega_write_sector(&hp->omega, sector, image, &got);
    hp_hal_status(HpStatusBusy);
    if(r != HpOmegaOk) return hp_omega_fail(hp, r);
    if(got != want) return hp_fail(hp, HP_ST_VERIFY_FAIL, 0, sector);
    hp->resp[3] = (uint16_t)got;
    hp->resp[4] = (uint16_t)(got >> 16);
    return hp_respond(hp, HP_ST_OK, 2);
}

/* ---- Dispatch ---- */

static bool hp_write_class(uint8_t code) {
    return code == HP_CMD_ARM_WRITE ||
           (code >= HP_CMD_ERASE_SECTOR && code <= HP_CMD_COMMIT_SECTOR) ||
           code == HP_CMD_OMEGA_ARM || code == HP_CMD_OMEGA_WRITE_SECTOR;
}

static uint16_t hp_run(Hp* hp) {
    switch(hp->code) {
    case HP_CMD_HELLO:
        return hp_hello(hp);
    case HP_CMD_CART_INFO:
        return hp_cart_info(hp);
    case HP_CMD_SAVE_PROBE:
        return hp_save_probe(hp);
    case HP_CMD_ARM_WRITE:
        return hp_arm_write(hp);
    case HP_CMD_DISARM:
        return hp_disarm_cmd(hp);
    case HP_CMD_READ_SECTOR:
        return hp_read_sector(hp);
    case HP_CMD_READ_FOOTERS:
        return hp_read_footers(hp);
    case HP_CMD_SECTOR_CRC32:
        return hp_sector_crc32(hp);
    case HP_CMD_ERASE_SECTOR:
        return hp_erase_sector(hp);
    case HP_CMD_WRITE_SECTOR:
        return hp_write_sector(hp);
    case HP_CMD_COPY_SECTOR:
        return hp_copy_sector(hp);
    case HP_CMD_COMMIT_SECTOR:
        return hp_commit_sector(hp);
    case HP_CMD_ROM_READ:
        return hp_rom_read(hp);
    case HP_CMD_ROM_CRC32:
        return hp_rom_crc32(hp);
    case HP_CMD_OMEGA_MOUNT:
        return hp_omega_mount_cmd(hp);
    case HP_CMD_OMEGA_OPEN:
        return hp_omega_open_cmd(hp);
    case HP_CMD_OMEGA_READ:
        return hp_omega_read_cmd(hp);
    case HP_CMD_OMEGA_CRC32:
        return hp_omega_crc32_cmd(hp);
    case HP_CMD_OMEGA_SHA256:
        return hp_omega_sha_cmd(hp);
    case HP_CMD_OMEGA_ARM:
        return hp_omega_arm_cmd(hp);
    case HP_CMD_OMEGA_WRITE_SECTOR:
        return hp_omega_write_cmd(hp);
    default:
        return hp_fail(hp, HP_ST_UNSUPPORTED, 0, 0);
    }
}

uint16_t hp_execute(Hp* hp) {
    if(!hp->pending) return HP_READY;
    hp->pending = false;
    /* The packet's CRC: rx_crc stays as its last word left it, because hp_step receives nothing
     * while a packet is pending. */
    const uint32_t crc = ~hp->rx_crc;
    const uint16_t w1 = (uint16_t)(hp->seq << 8 | hp->code);
    if(hp->last_valid && w1 == hp->last_w1 && crc == hp->last_crc && hp_write_class(hp->code)) {
        /* The same request again, and resp still holds its response: send that once more. */
        hp->tx_len = hp->last_len;
        hp->tx_pos = 1;
        return HP_SOF;
    }
    const uint16_t first = hp_run(hp);
    hp->last_valid = true;
    hp->last_w1 = w1;
    hp->last_crc = crc;
    hp->last_len = hp->tx_len;
    return first;
}
