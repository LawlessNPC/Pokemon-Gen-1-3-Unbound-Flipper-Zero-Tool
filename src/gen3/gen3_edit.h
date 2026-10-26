#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_rules.h>

/* Editor model. Every mutator leaves the Pokemon game-valid: the ability bit is 0 for a
 * one-ability species (a two-ability species keeps its stored slot; a new PID sets it to the
 * slot it was found for, pid & 1), the party stats match exp, IVs, EVs and nature
 * (gen3_recalc_party, so Hp = MaxHp), and the checksum is fixed. */

#define GEN3_EDIT_DEFAULT_NATIONAL 1
#define GEN3_EDIT_DEFAULT_LEVEL    5
/* same OT as plan 1's GEN3_TEST_TRAINER_*; plan 3 uses the player's */
#define GEN3_EDIT_DEFAULT_OT_NAME  "FLIPPER"
#define GEN3_EDIT_DEFAULT_TID      22222
#define GEN3_EDIT_DEFAULT_SID      0
#define GEN3_EDIT_MET_GAME         2 /* Ruby */
#define GEN3_EDIT_BALL             4 /* POKE BALL */
#define GEN3_EDIT_BALL_MAX         12 /* balls are items 1..12, MASTER BALL..PREMIER BALL */
#define GEN3_EDIT_MAX_EV           85 /* 6 x 85 = 510, the game's MAX_TOTAL_EVS */

typedef enum { /* same order and values as EvIv in src/include/stat_nl.h */
    Gen3PresetRandIvZeroEv,
    Gen3PresetRandIvLevelEv,
    Gen3PresetRandIvMaxEv,
    Gen3PresetMaxIvZeroEv,
    Gen3PresetMaxIvLevelEv,
    Gen3PresetMaxIvMaxEv,
} Gen3Preset;
#define GEN3_PRESET_COUNT 6

typedef struct Gen3Editor {
    Gen3Pkm pkm;
    Gen3Preset preset;
    bool preset_applied; /* false after gen3_edit_load until a preset or species is chosen */
    Gen3Rng rng;
} Gen3Editor;

/* What gen3_normalise changed. Party stats are always recalculated and aren't reported. */
typedef struct {
    bool moves_defaulted; /* no move was left: the default moveset, full PP, PP Ups 0 */
    bool duplicates_removed; /* a repeated move was dropped with its PP and PP Ups */
    bool gaps_packed; /* the input had an empty move slot before a used one */
    bool exp_clamped; /* exp was outside the level 1..100 range */
    bool ability_fixed; /* ability bit 1 on a one-ability species, now 0 */
} Gen3NormaliseInfo;

/* Makes a validated Pokemon (a real species, not an egg, move ids < 355) game-valid, for loads
 * and imports: moves packed with duplicates dropped, exp clamped to the level 1..100 range, the
 * default moveset if no move is left, ability bit 0 on a one-ability species, party stats
 * recalculated with the current HP kept but clamped to MaxHp, checksum fixed. Every other byte
 * is kept. info (may be NULL) is always reset, then filled. false, with *pkm untouched, if the
 * species maps to no national. */
bool gen3_normalise(Gen3Pkm* pkm, Gen3NormaliseInfo* info);

/* Bulbasaur L5, default OT, preset RandIvZeroEv. */
void gen3_edit_init(Gen3Editor* ed, uint32_t seed);
/* Takes pkm and runs gen3_normalise on it; preset_applied = false. */
void gen3_edit_load(Gen3Editor* ed, const Gen3Pkm* pkm);
uint16_t gen3_edit_national(const Gen3Editor* ed);
/* D5: a fresh Pokemon of that species, keeping OT name, TID, SID, OT gender, level and preset.
 * No-op if invalid or unchanged. */
void gen3_edit_set_species(Gen3Editor* ed, uint16_t national);
/* Clamped 1..100; exp = exp_for_level. */
void gen3_edit_set_level(Gen3Editor* ed, uint8_t level);
/* Applies IVs (re-rolls random ones) and EVs. No-op for a value >= GEN3_PRESET_COUNT. */
void gen3_edit_set_preset(Gen3Editor* ed, Gen3Preset preset);
/* "" -> species name (game encoding). */
void gen3_edit_set_nickname(Gen3Editor* ed, const char* ascii);
void gen3_edit_set_ot_name(Gen3Editor* ed, const char* ascii);
/* Keeps shininess (D6) by finding a new PID when needed, and the stored ability slot. */
void gen3_edit_set_ot_id(Gen3Editor* ed, uint16_t tid, uint16_t sid);
/* 0 (no item) or an id gen3_item_holdable accepts; anything else is refused (no-op). */
void gen3_edit_set_item(Gen3Editor* ed, uint16_t item);
/* A move: full base PP, that slot's PP Ups cleared. 0: clears the slot. Then the moves are packed
 * toward slot 0, carrying their PP and PP Ups. false (unchanged): slot >= 4, move >= 355, a
 * duplicate, or removing the last move. */
bool gen3_edit_set_move(Gen3Editor* ed, uint8_t slot, uint16_t move);
/* gen3_default_moves for the species and level, each with full base PP; PP Ups 0. */
void gen3_edit_default_moves(Gen3Editor* ed);
/* 1..GEN3_EDIT_BALL_MAX; anything else is a no-op. */
void gen3_edit_set_ball(Gen3Editor* ed, uint8_t ball);
/* raw byte: strain << 4 | days */
void gen3_edit_set_pokerus(Gen3Editor* ed, uint8_t value);
/* New PID with these traits (normalised as gen3_pid_find does); changes only the PID, the ability
 * bit, party stats and the checksum. If the normalised want already equals gen3_traits_of, true
 * and nothing changes (the PID and stored ability slot are kept). false (unchanged) only if
 * gen3_pid_find fails. */
bool gen3_edit_set_traits(Gen3Editor* ed, const Gen3Traits* want);
/* 0 "Clean"; days != 0 "Infected"; else "Cured". */
const char* gen3_pokerus_status(uint8_t value);
