#pragma once

#include <stddef.h>
#include <stdint.h>

/* A list of ids sorted by name, for the first-letter pickers of moves and held items. */
typedef struct {
    size_t count;
    uint16_t (*id_at)(size_t i); /* the id at sorted position i < count */
    const char* (*name)(uint16_t id);
} Gen3NameList;

/* The distinct first characters of the names, in list order, at most max. Returns how many. */
size_t gen3_letters(const Gen3NameList* list, char* out, size_t max);
/* The ids whose name starts with letter, in list order, at most max. Returns how many. */
size_t gen3_letter_ids(const Gen3NameList* list, char letter, uint16_t* out, size_t max);

/* Room that covers every letter list and every letter's ids for moves and holdable items
 * (asserted in tests/test_gen3_letters.c). */
#define GEN3_LETTERS_MAX    40
#define GEN3_LETTER_IDS_MAX 96
