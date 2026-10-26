#include "gen3_letters.h"

size_t gen3_letters(const Gen3NameList* list, char* out, size_t max) {
    size_t n = 0;

    for(size_t i = 0; i < list->count && n < max; i++) {
        char c = list->name(list->id_at(i))[0];
        /* The list is sorted, so a letter is only ever repeated by its neighbour */
        if(n == 0 || out[n - 1] != c) out[n++] = c;
    }
    return n;
}

size_t gen3_letter_ids(const Gen3NameList* list, char letter, uint16_t* out, size_t max) {
    size_t n = 0;

    for(size_t i = 0; i < list->count && n < max; i++) {
        uint16_t id = list->id_at(i);
        if(list->name(id)[0] == letter) out[n++] = id;
    }
    return n;
}
