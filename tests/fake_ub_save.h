#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Test-only Pokemon Unbound 2.1.1.1 saves (131072 bytes), as PUSE's profile lays them out
 * (github.com/Zannael/PUSE, backend/data/profiles/unbound-2.1.1.1.json): FireRed's container with
 * the signatures 0x01121999 / 0x01121998, the party in section 1, the PC stream of 58-byte records
 * gathered from sections 5..13, the shared sectors 30 and 31, and the tails of sections 2 and 3.
 * It shares no code with src/gen3/gen3_ub_save.c. */

#define FAKE_UB_LEN 131072u
#define FAKE_UB_SIG 0x01121999u

/* Every byte 0xFF, as an erased flash */
void fake_ub_erase(uint8_t img[FAKE_UB_LEN]);
/* Slot slot (0: sectors 0..13, 1: 14..27) with every footer's counter, the section with id k at
 * sector slot * 14 + (k + 14 - rotate) % 14, a marker byte in each payload, the PC stream and the
 * party empty, and each checksum right. Sectors 30 and 31 are zeroed (no footer). */
void fake_ub_build(uint8_t img[FAKE_UB_LEN], uint8_t slot, uint32_t counter, uint8_t rotate);
/* The physical sector of section id in slot, from its footers */
uint8_t fake_ub_phys(const uint8_t img[FAKE_UB_LEN], uint8_t slot, uint8_t id);
/* The record of box (0..23) slot k (0..29) in slot's PC stream, the checksums put right */
void fake_ub_put_box(uint8_t img[FAKE_UB_LEN], uint8_t slot, uint8_t box, uint8_t k, const uint8_t rec[58]);
/* Party member i (0..5), and the count */
void fake_ub_put_party(uint8_t img[FAKE_UB_LEN], uint8_t slot, uint8_t i, const uint8_t rec[100]);
void fake_ub_set_party_count(uint8_t img[FAKE_UB_LEN], uint8_t slot, uint8_t count);
/* The trainer in section 0: the name (encoded by the caller, 8 bytes), gender, TID and SID */
void fake_ub_put_trainer(
    uint8_t img[FAKE_UB_LEN],
    uint8_t slot,
    const uint8_t name[8],
    uint8_t gender,
    uint16_t tid,
    uint16_t sid);
/* Recomputes the checksum of every sector 0..27 with an Unbound signature */
void fake_ub_fix_checksums(uint8_t img[FAKE_UB_LEN]);
/* A box record: species, exp, the nickname (encoded, 10 bytes), the IV word */
void fake_ub_box_rec(uint8_t rec[58], uint16_t species, uint32_t exp, const uint8_t nick[10], uint32_t ivs);
/* A party record: species, level, the nickname, the IV word */
void fake_ub_party_rec(uint8_t rec[100], uint16_t species, uint8_t level, const uint8_t nick[10], uint32_t ivs);
