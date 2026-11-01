#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pokemon Unbound 2.1.1.1's save, as PUSE documents it (github.com/Zannael/PUSE, MIT:
 * backend/data/profiles/unbound-2.1.1.1.json, backend/modules/party.py and pc.py): FireRed's
 * container of 32 sectors of 4 KB, two slots of 14 sections each, with the signatures 0x01121999
 * and 0x01121998; the party in section 1; 24 boxes of 30 records of 58 bytes, unencrypted, in a PC
 * stream gathered from several regions, two of them in the sectors 30 and 31 that both slots share.
 * Pure C, no heap, read-only. */

#define GEN3_UB_SECTORS 32u
#define GEN3_UB_SLOT_SECTORS 14u
#define GEN3_UB_SECTOR_SIZE 4096u
#define GEN3_UB_SAVE_SIZE 131072u
#define GEN3_UB_BOXES 24u
#define GEN3_UB_BOX_SLOTS 30u
#define GEN3_UB_BOX_MON 58u
#define GEN3_UB_PARTY_MON 100u
#define GEN3_UB_PARTY_MAX 6u
#define GEN3_UB_SIG_A 0x01121999u
#define GEN3_UB_SIG_B 0x01121998u

typedef struct {
    uint16_t id, checksum;
    uint32_t signature, counter;
    bool sig_ok; /* an Unbound signature */
    bool sum_ok; /* the checksum over the section's length matches; true for id 4 (not checked) */
} Gen3UbSector;

/* The footer of a 4 KB sector, and its checksum checked */
void gen3_ub_sector_info(const uint8_t sector[GEN3_UB_SECTOR_SIZE], Gen3UbSector* out);

typedef struct {
    bool valid[2];
    uint32_t counter[2];
    int8_t loaded; /* 0 slot A (sectors 0..13), 1 slot B (14..27), -1 none */
    uint8_t phys[GEN3_UB_SLOT_SECTORS]; /* the loaded slot: section id -> physical sector */
} Gen3UbSlots;

/* PUSE's rule: a slot is valid with each id 0..13 once, each signature an Unbound one, each checksum
 * good (id 4 not checked) and one counter in all 14; of two, the newer by modulo half range, slot A
 * on a tie. infos: sectors 0..27. */
void gen3_ub_slots(const Gen3UbSector infos[2u * GEN3_UB_SLOT_SECTORS], Gen3UbSlots* out);

typedef struct {
    char name[8]; /* decoded */
    uint8_t gender;
    uint16_t tid, sid;
} Gen3UbTrainer;

/* From section 0: the name at 0x00, the gender at 0x08, the trainer id at 0x0A (TID, then SID) */
void gen3_ub_trainer(const uint8_t section0[GEN3_UB_SECTOR_SIZE], Gen3UbTrainer* out);
/* From section 1, at 0x34; at most 6 */
uint8_t gen3_ub_party_count(const uint8_t section1[GEN3_UB_SECTOR_SIZE]);
/* Party member i's 100 bytes are at section 1 + this */
#define GEN3_UB_PARTY_OFF(i) (0x38u + (uint32_t)(i) * GEN3_UB_PARTY_MON)

/* A box record's bytes in the loaded slot: at most 2 pieces (physical sector, offset, length) */
typedef struct {
    uint8_t n;
    uint8_t sector[2];
    uint16_t off[2], len[2];
} Gen3UbPieces;

/* false if no slot is loaded, or box > 23 or slot > 29 */
bool gen3_ub_box_pieces(const Gen3UbSlots* s, uint8_t box, uint8_t slot, Gen3UbPieces* out);

typedef struct {
    uint16_t species; /* 0: empty */
    uint8_t level; /* 0 if unknown */
    bool egg;
    char nickname[11];
} Gen3UbSummary;

/* A species' growth rate, 0..5, or -1 if unknown */
typedef int (*Gen3UbGrowthFn)(uint16_t species, void* ctx);

/* A box record: species at 28, exp at 32 (its level through g), nickname at 8, egg = bit 30 of the
 * IV word at 54 */
void gen3_ub_box_summary(
    const uint8_t rec[GEN3_UB_BOX_MON],
    Gen3UbGrowthFn g,
    void* ctx,
    Gen3UbSummary* out);
/* A party record: species at 32, level at 84, nickname at 8, egg = bit 30 of the IV word at 72 */
void gen3_ub_party_summary(const uint8_t rec[GEN3_UB_PARTY_MON], Gen3UbSummary* out);
