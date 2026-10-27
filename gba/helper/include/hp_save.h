#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The Gen III save's two slots, as the helper checks them before it arms a write
 * (multiboot-and-save.md §5.1-§5.5). Slot 0 is sectors 0..13, slot 1 sectors 14..27.
 *
 * The helper has no per-game section sizes, so it checks each sector's checksum over the whole
 * 0xF80-byte data area (multiboot-and-save.md §5.3). That equals the game's exact-size checksum
 * when the bytes past the section are zero, as the game writes them (pokeruby: src/save.c:
 * WriteSingleChunk clears the sector first); a sector with other bytes there fails the check.
 *
 * Every function reads the save chip through hp_flash_read: call them only once the probe has said
 * HpChipFlash128. */

#define HP_SAVE_SLOT_SECTORS 14u
#define HP_SAVE_SIGNATURE 0x08012025u

/* A sector's footer (multiboot-and-save.md §5.2), and its full-length checksum */
typedef struct {
    uint16_t id; /* 0xFF4 */
    uint16_t checksum; /* 0xFF6 */
    uint32_t signature; /* 0xFF8 */
    uint32_t counter; /* 0xFFC */
    uint16_t full_checksum; /* multiboot-and-save.md §5.3 over all 0xF80 data bytes */
} HpFooter;

/* Sector 0..31 */
void hp_save_footer(uint8_t sector, HpFooter* out);
/* Slot s (0/1) holds 14 sectors with signature 0x08012025, full-length checksum ok (§5.3 over
 * 0xF80 bytes), ids 0..13 once each, one counter. Writes it to *counter. */
bool hp_save_slot_ok(uint8_t slot, uint32_t* counter);
/* multiboot-and-save.md §5.5 over both slots with full-length checksums: 0/1 = the slot the game
 * loads, -1 = none valid. As the game does, that is the parity of the winning counter, wherever
 * the counter was found. */
int hp_save_active_slot(void);
