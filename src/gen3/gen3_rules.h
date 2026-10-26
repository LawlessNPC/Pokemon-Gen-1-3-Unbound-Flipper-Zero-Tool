#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "gen3_pkm.h"

#define GEN3_SHEDINJA_NATIONAL 292

/* Total exp for a level, growth is a Gen3Growth. Level is clamped to 100; 0 -> 0, 1 -> 1. */
uint32_t gen3_exp_for_level(uint8_t growth, uint8_t level);
/* Largest level L <= 100 with gen3_exp_for_level(growth, L) <= exp (0 for exp 0). */
uint8_t gen3_level_for_exp(uint8_t growth, uint32_t exp);

/* +1 raises, -1 lowers, 0 neutral. HP is never affected. */
int8_t gen3_nature_effect(uint8_t nature, Gen3Stat stat);
uint16_t gen3_stat_value(
    Gen3Stat stat,
    uint8_t base,
    uint8_t iv,
    uint8_t ev,
    uint8_t level,
    uint8_t nature);
/* Level comes from exp. Shedinja's max HP is 1. All zero for an unknown species. */
void gen3_calc_stats(const Gen3Pkm* p, uint16_t out[Gen3StatCount]);
/* Sets Level, MaxHp..Spd and Hp = MaxHp. Status and Mail are untouched. No-op for an
 * unknown species. */
void gen3_recalc_party(Gen3Pkm* p);

typedef enum {
    Gen3GenderMale,
    Gen3GenderFemale,
    Gen3GenderNone
} Gen3Gender;

Gen3Gender gen3_gender_of(uint8_t gender_ratio, uint32_t pid);
bool gen3_is_shiny(uint32_t ot_id, uint32_t pid);
/* 0..25 = A..Z, 26 = '!', 27 = '?'. */
uint8_t gen3_unown_letter(uint32_t pid);

typedef struct {
    uint8_t nature; /* pid % 25 */
    Gen3Gender gender;
    bool shiny;
    uint8_t ability_slot; /* the stored ability bit */
    uint8_t unown_letter;
} Gen3Traits;

/* Gen3FieldSpecies holds the internal id. Gender is None for an unknown species. */
void gen3_traits_of(const Gen3Pkm* p, Gen3Traits* out);
