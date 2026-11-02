#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gen3_ub_mon.h"
#include "gen3_ub_save.h"

/* The Unbound editor model: operations that keep a party or box record consistent the way the game
 * would. A party Pokemon has its stats recomputed after each change, a species change keeps the
 * level, nature, shininess and ability slot, and the rules on items, moves, abilities and EVs hold.
 * The rules are ported from PUSE (github.com/Zannael/PUSE, MIT, commit
 * 2c624440fb24940480a3430fa17214942d4ba04a): backend/modules/party.py and pc.py. Every operation
 * edits the caller's Gen3UbEditor in place; `changed` turns true when the record's bytes change and
 * stays as it was for a refused or no-op call. */

typedef struct {
    Gen3UbMon mon;
    Gen3UbTrainer trainer; /* valid only if has_trainer */
    bool has_trainer;
    bool changed;
} Gen3UbEditor;

/* t may be NULL (no trainer); changed starts false */
void gen3_ub_edit_load(Gen3UbEditor* e, const Gen3UbMon* m, const Gen3UbTrainer* t);

/* Keeps level, nature, shininess and the ability slot where the species has it (else slot 0). The
 * nickname follows if it was the old species' name (all caps stays all caps, else each word is
 * capitalised); the moves become the first 4 of its level-up list, PP-ups cleared and PP refilled.
 * False, with the record unchanged, for an unknown species or one no Pokemon can be
 * (gen3_ub_species_real). */
bool gen3_ub_edit_set_species(Gen3UbEditor* e, uint16_t species);
/* 1..100, else ignored. Sets exp for the species' growth rate. */
void gen3_ub_edit_set_level(Gen3UbEditor* e, uint8_t level);
/* A held item or 0 only */
bool gen3_ub_edit_set_item(Gen3UbEditor* e, uint16_t item);
/* Move ids 1..922, or 0 to empty a slot other than 0. Clears the slot's PP-ups and, in a party
 * record, sets its PP to the move's base PP. */
bool gen3_ub_edit_set_move(Gen3UbEditor* e, uint8_t i, uint16_t move);
bool gen3_ub_edit_set_nature(Gen3UbEditor* e, uint8_t nature);
bool gen3_ub_edit_set_shiny(Gen3UbEditor* e, bool shiny);
/* 0, 1 (if the species has a second ability) or 2 (if it has a hidden one) */
bool gen3_ub_edit_set_ability(Gen3UbEditor* e, uint8_t slot);
typedef enum { Gen3UbIvsAll31, Gen3UbIvsAll0 } Gen3UbIvs;
void gen3_ub_edit_set_ivs(Gen3UbEditor* e, Gen3UbIvs p);
#define GEN3_UB_EV_STAT_MAX 252u
#define GEN3_UB_EV_TOTAL_MAX 510u
/* Each is cut to 252, then the total to 510 by reducing from the last stat backwards (a backstop:
 * the number scene refuses an EV that would take the total over 510) */
void gen3_ub_edit_set_evs(Gen3UbEditor* e, const uint8_t evs[6]);
/* 1..10 characters the game can store */
bool gen3_ub_edit_set_nickname(Gen3UbEditor* e, const char* ascii);
