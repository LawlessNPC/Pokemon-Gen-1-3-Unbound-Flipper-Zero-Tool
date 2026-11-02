#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The Pokemon Unbound party record (100 bytes, unencrypted) and compact box record (58 bytes), with
 * the game rules the editor needs. The accessors read and write raw in place and leave every byte
 * they do not own as it was. Rules and layouts are ported from PUSE (github.com/Zannael/PUSE, MIT). */

#define GEN3_UB_MON_NICK 10u
#define GEN3_UB_MON_OT 7u

typedef struct {
    bool party; /* true: 100-byte party record; false: 58-byte box record, the rest zero */
    uint8_t raw[100];
} Gen3UbMon;

uint16_t gen3_ub_mon_species(const Gen3UbMon* m);
void gen3_ub_mon_set_species(Gen3UbMon* m, uint16_t v);
uint16_t gen3_ub_mon_item(const Gen3UbMon* m);
void gen3_ub_mon_set_item(Gen3UbMon* m, uint16_t v);
uint32_t gen3_ub_mon_exp(const Gen3UbMon* m);
void gen3_ub_mon_set_exp(Gen3UbMon* m, uint32_t v);
uint8_t gen3_ub_mon_pp_ups(const Gen3UbMon* m);
void gen3_ub_mon_set_pp_ups(Gen3UbMon* m, uint8_t v);
uint8_t gen3_ub_mon_friendship(const Gen3UbMon* m);
void gen3_ub_mon_set_friendship(Gen3UbMon* m, uint8_t v);
uint8_t gen3_ub_mon_ball(const Gen3UbMon* m);
void gen3_ub_mon_set_ball(Gen3UbMon* m, uint8_t v);
/* Moves are 10 bits each in a box record, u16 in a party record */
void gen3_ub_mon_moves(const Gen3UbMon* m, uint16_t out[4]);
void gen3_ub_mon_set_moves(Gen3UbMon* m, const uint16_t in[4]);
/* Current PP, party only: a box record reads 0 and ignores the set */
void gen3_ub_mon_pp(const Gen3UbMon* m, uint8_t out[4]);
void gen3_ub_mon_set_pp(Gen3UbMon* m, const uint8_t in[4]);
void gen3_ub_mon_evs(const Gen3UbMon* m, uint8_t out[6]);
void gen3_ub_mon_set_evs(Gen3UbMon* m, const uint8_t in[6]);
/* The six 5-bit IVs in the order HP, Atk, Def, Spe, SpA, SpD; the egg (30) and hidden-ability (31)
 * bits of the word are kept by the setter */
void gen3_ub_mon_ivs(const Gen3UbMon* m, uint8_t out[6]);
void gen3_ub_mon_set_ivs(Gen3UbMon* m, const uint8_t in[6]);
bool gen3_ub_mon_hidden(const Gen3UbMon* m);
void gen3_ub_mon_set_hidden(Gen3UbMon* m, bool v);
uint32_t gen3_ub_mon_pid(const Gen3UbMon* m);
void gen3_ub_mon_set_pid(Gen3UbMon* m, uint32_t v);
uint32_t gen3_ub_mon_otid(const Gen3UbMon* m);
void gen3_ub_mon_set_otid(Gen3UbMon* m, uint32_t v);
/* The game's own text bytes, 0xFF-terminated */
void gen3_ub_mon_nickname(const Gen3UbMon* m, uint8_t out[GEN3_UB_MON_NICK]);
void gen3_ub_mon_set_nickname(Gen3UbMon* m, const uint8_t in[GEN3_UB_MON_NICK]);
void gen3_ub_mon_ot_name(const Gen3UbMon* m, uint8_t out[GEN3_UB_MON_OT]);
void gen3_ub_mon_set_ot_name(Gen3UbMon* m, const uint8_t in[GEN3_UB_MON_OT]);
/* Party: the byte at +84. Box: from exp and the species' growth rate (0 if unknown). */
uint8_t gen3_ub_mon_level(const Gen3UbMon* m);
/* Party only: writes the level byte at +84 */
void gen3_ub_mon_set_level(Gen3UbMon* m, uint8_t v);

/* pid % 25 */
uint8_t gen3_ub_nature(const Gen3UbMon* m);
bool gen3_ub_shiny(const Gen3UbMon* m);
/* 2 if the hidden-ability bit is set, else pid & 1 */
uint8_t gen3_ub_ability_slot(const Gen3UbMon* m);
/* 0 male, 1 female, 2 genderless (or a species with no data) */
int gen3_ub_gender(const Gen3UbMon* m);

/* Finds a PID near the current one with the target nature (0..24) and the wanted shininess (0/1),
 * gender (0 male, 1 female, 2 genderless) and ability slot (0, 1, or 2 for hidden). -1 keeps the
 * record's current shininess, gender (none for a genderless species) or ability. Sets the PID and
 * the hidden bit; false, with the record unchanged, if there is no solution. */
bool gen3_ub_solve_identity(Gen3UbMon* m, uint8_t nature, int shiny, int gender, int ability_slot);

/* Party only: writes the level byte from exp, then the six stats from the base stats, nature and
 * IVs/EVs, clamping current HP; always zeroes the checksum field at +28. A species with unknown
 * base stats keeps its stats. */
void gen3_ub_party_restat(Gen3UbMon* m);

/* A new box Pokemon with PUSE's defaults. The owner's OT id, OT name, language and flags (without
 * bad egg or egg) come from owner, a party or box record. False for an unknown species or one no
 * Pokemon can be (gen3_ub_species_real). */
bool gen3_ub_new_box_mon(Gen3UbMon* m, uint16_t species, uint8_t level, const Gen3UbMon* owner);
