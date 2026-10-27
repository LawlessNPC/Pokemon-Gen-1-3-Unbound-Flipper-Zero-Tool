#include "hp_save.h"
#include "hp_flash.h"

/* A sector: 0xF80 data bytes, then the footer from 0xFF4 (pokeruby: src/save.c: struct SaveSector;
 * em: include/save.h: SECTOR_DATA_SIZE) */
#define HP_SAVE_DATA_LEN 0xF80u
#define HP_SAVE_FOOTER_OFF 0xFF4u
#define HP_SAVE_FOOTER_LEN 12u
#define HP_SAVE_ALL_IDS 0x3FFFu /* ids 0..13 */
#define HP_SAVE_CHUNK 64u /* bytes summed per hp_flash_read */

_Static_assert(HP_SAVE_DATA_LEN % HP_SAVE_CHUNK == 0, "whole chunks");

static uint32_t hp_save_le32(const uint8_t* b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

void hp_save_footer(uint8_t sector, HpFooter* out) {
    uint8_t buf[HP_SAVE_CHUNK];
    /* pokeruby: src/save.c: CalculateChecksum: the 32-bit sum of little-endian words, folded */
    uint32_t sum = 0;
    for(uint32_t off = 0; off < HP_SAVE_DATA_LEN; off += HP_SAVE_CHUNK) {
        hp_flash_read(sector, (uint16_t)off, buf, HP_SAVE_CHUNK);
        for(uint32_t i = 0; i < HP_SAVE_CHUNK; i += 4) sum += hp_save_le32(buf + i);
    }
    out->full_checksum = (uint16_t)((sum >> 16) + sum);
    hp_flash_read(sector, HP_SAVE_FOOTER_OFF, buf, HP_SAVE_FOOTER_LEN);
    out->id = (uint16_t)(buf[0] | buf[1] << 8);
    out->checksum = (uint16_t)(buf[2] | buf[3] << 8);
    out->signature = hp_save_le32(buf + 4);
    out->counter = hp_save_le32(buf + 8);
}

bool hp_save_slot_ok(uint8_t slot, uint32_t* counter) {
    if(slot > 1) return false;
    uint32_t seen = 0;
    uint32_t ctr = 0;
    for(uint32_t i = 0; i < HP_SAVE_SLOT_SECTORS; i++) {
        HpFooter f;
        hp_save_footer((uint8_t)(slot * HP_SAVE_SLOT_SECTORS + i), &f);
        if(f.signature != HP_SAVE_SIGNATURE || f.checksum != f.full_checksum) return false;
        if(f.id >= HP_SAVE_SLOT_SECTORS || (seen & (1u << f.id))) return false;
        if(i > 0 && f.counter != ctr) return false;
        seen |= 1u << f.id;
        ctr = f.counter;
    }
    *counter = ctr;
    return true;
}

/* One slot as pokeruby: src/save.c: GetSaveValidStatus sees it: OK when the sectors with a valid
 * signature and checksum cover ids 0..13, with the counter of the last of them in physical order.
 * An id over 13 sets a bit outside 0..13, so its slot can't be OK; the game indexes its size table
 * with that id and can't find 14 valid ids among the slot's other 13 sectors either. */
static bool hp_save_slot_loads(uint8_t slot, uint32_t* counter) {
    uint32_t valid = 0;
    uint32_t ctr = 0;
    for(uint32_t i = 0; i < HP_SAVE_SLOT_SECTORS; i++) {
        HpFooter f;
        hp_save_footer((uint8_t)(slot * HP_SAVE_SLOT_SECTORS + i), &f);
        if(f.signature != HP_SAVE_SIGNATURE || f.checksum != f.full_checksum) continue;
        ctr = f.counter;
        valid |= f.id < HP_SAVE_SLOT_SECTORS ? 1u << f.id : 1u << HP_SAVE_SLOT_SECTORS;
    }
    *counter = ctr;
    return valid == HP_SAVE_ALL_IDS;
}

int hp_save_active_slot(void) {
    uint32_t a = 0;
    uint32_t b = 0;
    const bool a_ok = hp_save_slot_loads(0, &a);
    const bool b_ok = hp_save_slot_loads(1, &b);
    uint32_t ctr;
    if(a_ok && b_ok) {
        /* GetSaveValidStatus: the newer counter; 0 is newer than 0xFFFFFFFF */
        if((a == 0xFFFFFFFFu && b == 0) || (a == 0 && b == 0xFFFFFFFFu)) {
            ctr = (a + 1u) < (b + 1u) ? b : a;
        } else {
            ctr = a < b ? b : a;
        }
    } else if(a_ok) {
        ctr = a;
    } else if(b_ok) {
        ctr = b;
    } else {
        return -1;
    }
    /* pokeruby: src/save.c: sub_81258BC (em: CopySaveSlotData) loads slot gSaveCounter % 2 */
    return (int)(ctr % 2u);
}
