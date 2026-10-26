#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Gen III game data, indexed by National Dex number. The game's internal species ids appear only
 * in the Pokemon bytes and in gen3_internal_to_national_tab. Names keep the game's spelling,
 * mapped to ASCII: e-acute -> 'E', female '\200', male '\201'. */

#define GEN3_NATIONAL_COUNT      386 /* National Dex 1..386 */
#define GEN3_INTERNAL_COUNT      412 /* game species ids 0..411 (412 is the egg, excluded) */
#define GEN3_MOVE_COUNT          355 /* move ids 0..354 */
#define GEN3_ITEM_COUNT          377 /* item ids 0..376 */
#define GEN3_ABILITY_COUNT       78
#define GEN3_NATURE_COUNT        25
#define GEN3_TYPE_COUNT          18
#define GEN3_LEARNSET_COUNT      3947
#define GEN3_HOLDABLE_COUNT      243
#define GEN3_SPRITE_MAX_NATIONAL 251 /* files/all_sprites.fxbm covers 1..251 */
#define GEN3_NATIONAL_MEW        151
#define GEN3_NATIONAL_UNOWN      201
#define GEN3_NATIONAL_SHEDINJA   292
#define GEN3_NATIONAL_DEOXYS     386
#define GEN3_GENDER_MALE_ONLY    0
#define GEN3_GENDER_FEMALE_ONLY  254
#define GEN3_GENDERLESS          255

typedef enum {
    Gen3GrowthMediumFast,
    Gen3GrowthErratic,
    Gen3GrowthFluctuating,
    Gen3GrowthMediumSlow,
    Gen3GrowthFast,
    Gen3GrowthSlow
} Gen3Growth; /* game values 0..5 */

typedef struct {
    uint8_t base[6]; /* HP, Atk, Def, Spe, SpA, SpD */
    uint8_t type[2]; /* game type ids; one-type species repeat it */
    /* 0 male only, 254 female only, 255 genderless, else female if (pid & 0xFF) < ratio */
    uint8_t gender_ratio;
    uint8_t growth; /* Gen3Growth */
    uint8_t ability[2]; /* ability[1] == 0: one ability */
    uint8_t friendship; /* base friendship */
    uint16_t internal; /* game species id */
} Gen3SpeciesInfo; /* 16 bytes */

const Gen3SpeciesInfo* gen3_species(uint16_t national); /* NULL unless 1..386 */
const char* gen3_species_name(uint16_t national); /* "?" unless 1..386 */
uint16_t gen3_national_from_internal(uint16_t internal); /* 0 = not a real species */
uint16_t gen3_internal_from_national(uint16_t national); /* 0 unless 1..386 */
/* entries are (level << 9) | move, in game order; 0 + NULL if invalid */
size_t gen3_learnset(uint16_t national, const uint16_t** entries);
const char* gen3_move_name(uint16_t move); /* "???" if >= GEN3_MOVE_COUNT; move 0 is "-" */
uint8_t gen3_move_pp(uint16_t move); /* base PP; 0 if out of range */
uint16_t gen3_move_by_name(size_t i); /* i < GEN3_MOVE_COUNT - 1: ids 1..354 sorted by name */
const char* gen3_item_name(uint16_t item); /* "???" if unused or >= GEN3_ITEM_COUNT */
bool gen3_item_holdable(uint16_t item);
uint16_t gen3_holdable_by_name(size_t i); /* i < GEN3_HOLDABLE_COUNT, sorted by name */
const char* gen3_ability_name(uint8_t ability); /* "???" if out of range */
const char* gen3_nature_name(uint8_t nature); /* "???" if >= 25 */
const char* gen3_type_name(uint8_t type); /* "???" if >= 18 */

/* Generated arrays (gen3_data_gen.c). Use the functions above. */
extern const Gen3SpeciesInfo gen3_species_info_tab[GEN3_NATIONAL_COUNT + 1];
extern const char gen3_species_name_tab[GEN3_NATIONAL_COUNT + 1][11];
extern const uint16_t gen3_internal_to_national_tab[GEN3_INTERNAL_COUNT];
/* [n]..[n+1]; [0]=[1]=0, [387]=3947 */
extern const uint16_t gen3_learnset_start_tab[GEN3_NATIONAL_COUNT + 2];
extern const uint16_t gen3_learnset_tab[GEN3_LEARNSET_COUNT];
extern const char gen3_move_name_tab[GEN3_MOVE_COUNT][13];
extern const uint8_t gen3_move_pp_tab[GEN3_MOVE_COUNT];
extern const uint16_t gen3_move_by_name_tab[GEN3_MOVE_COUNT - 1];
extern const char gen3_item_name_tab[GEN3_ITEM_COUNT][15]; /* "" for unused ids */
extern const uint16_t gen3_holdable_by_name_tab[GEN3_HOLDABLE_COUNT];
extern const char gen3_ability_name_tab[GEN3_ABILITY_COUNT][13];
extern const char gen3_nature_name_tab[GEN3_NATURE_COUNT][8];
extern const char gen3_type_name_tab[GEN3_TYPE_COUNT][7];
