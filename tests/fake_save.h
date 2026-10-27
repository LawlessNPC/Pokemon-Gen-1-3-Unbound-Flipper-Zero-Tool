#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Test-only: synthetic 128 KB Gen III flash images (multiboot-and-save.md §5.1-§5.4), as the games
 * write them. Written from the doc alone: its section sizes and checksum are its own, and it shares
 * nothing with the Flipper's save code.
 *
 * A present slot s holds ids 0..13, id k at physical sector 14 * s + (k + first[s]) % 14. Each
 * sector: data bytes [0, size[id]) from an LCG on the seed, zero up to 0xFF4 (the game clears the
 * whole sector before it fills it: pokeruby: src/save.c: WriteSingleChunk), then the footer: id at
 * 0xFF4, the checksum over size[id] bytes at 0xFF6, the signature 0x08012025 at 0xFF8 and the
 * counter at 0xFFC, all little-endian. */

#define FAKE_SAVE_LEN 131072u
#define FAKE_SAVE_SECTOR_LEN 4096u
#define FAKE_SAVE_SLOT_SECTORS 14u

typedef enum {
    FakeGameRS,
    FakeGameFRLG,
    FakeGameE,
} FakeGame;

typedef struct {
    FakeGame game;
    bool slot_present[2];
    uint32_t counter[2];
    uint8_t first[2]; /* physical index of id 0 */
    uint32_t seed; /* data bytes within size[id] from an LCG; past size[id] zero */
} FakeSaveSpec;

/* Absent slots and sectors 28..31 all 0xFF */
void fake_save_build(uint8_t img[131072], const FakeSaveSpec* spec);
/* Sector number 0..27 */
uint32_t fake_save_phys(const FakeSaveSpec* spec, uint8_t slot, uint8_t id);
/* The section's used length: multiboot-and-save.md §5.4 */
uint16_t fake_save_size(FakeGame game, uint8_t id);
/* Recomputes that sector's checksum over size[id] bytes, id from its footer (over all 0xF80 data
 * bytes for an id over 13). */
void fake_save_fix(uint8_t img[131072], uint8_t sector, FakeGame game);
void fake_save_set_counter(uint8_t img[131072], uint8_t sector, uint32_t counter);
/* The id only: the checksum is left as it is. */
void fake_save_set_id(uint8_t img[131072], uint8_t sector, uint16_t id);
/* Makes the stored checksum wrong. */
void fake_save_break_checksum(uint8_t img[131072], uint8_t sector);
