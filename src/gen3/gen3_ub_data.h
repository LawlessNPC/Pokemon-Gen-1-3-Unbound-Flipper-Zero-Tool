#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Pokemon Unbound's game data by its own species id, from files/unbound_bulk.bin on the SD card,
 * which tools/unbound_tables.py makes from PUSE's data (github.com/Zannael/PUSE, MIT). The
 * accessors read through the installed source: call them from the app's threads, never an ISR. */

#define GEN3_UB_SPECIES_MAX 1267u /* species ids 1..1267 have names */
#define GEN3_UB_NAME_LEN 10u

typedef bool (*Gen3UbRead)(void* ctx, uint32_t offset, void* buf, uint32_t len);

/* Installs read as the source if the file's header, table directory and CRC-32 check out; else
 * leaves no source. */
bool gen3_ub_data_open(Gen3UbRead read, void* ctx);
void gen3_ub_data_close(void);
bool gen3_ub_data_ok(void);
/* The species' name, uppercase; false (out "#<id>") for an id with none, or a read failure */
bool gen3_ub_species_name(uint16_t species, char out[GEN3_UB_NAME_LEN + 1u]);
/* The growth rate 0..5, -1 if unknown or unreadable. A Gen3UbGrowthFn (ctx unused). */
int gen3_ub_growth(uint16_t species, void* ctx);
/* The National Dex number, 0 if unmapped or unreadable */
uint16_t gen3_ub_national(uint16_t species);

/* Moves (ids 1..922), items (1..728), abilities (1..254): the name in uppercase, "#<id>" and false
 * for an id with none or a read failure */
bool gen3_ub_move_name(uint16_t move, char out[13]);
/* Base PP, 0 for an unknown move */
uint8_t gen3_ub_move_pp(uint16_t move);
bool gen3_ub_item_name(uint16_t item, char out[18]);
/* Whether a Pokemon can hold the item: it is in the main, ball or berry pocket */
bool gen3_ub_item_holdable(uint16_t item);
bool gen3_ub_ability_name(uint8_t ability, char out[16]);
/* Ability ids of slot 1, slot 2 and the hidden one, 0 for none; false for an unknown species */
bool gen3_ub_species_abilities(uint16_t species, uint8_t out[3]);
/* Base stats in the order HP, Atk, Def, Spe, SpA, SpD; false for an unknown species */
bool gen3_ub_base_stats(uint16_t species, uint8_t out[6]);
/* The gender threshold byte, -1 for an unknown species or a read failure */
int gen3_ub_gender_threshold(uint16_t species);
/* Copies up to max of the species' level-up move ids, in the game's order; returns the count */
uint8_t gen3_ub_levelup(uint16_t species, uint16_t* out, uint8_t max);

/* Ids sorted by name, for pick lists: species 1..1267, moves 1..922, and the items a Pokemon can hold */
typedef enum { Gen3UbSortSpecies, Gen3UbSortMoves, Gen3UbSortItems } Gen3UbSort;
uint16_t gen3_ub_sorted_count(Gen3UbSort s);
/* The id at index of the list, 0 out of range */
uint16_t gen3_ub_sorted(Gen3UbSort s, uint16_t index);
