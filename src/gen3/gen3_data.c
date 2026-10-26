#include <src/gen3/gen3_data.h>

_Static_assert(sizeof(Gen3SpeciesInfo) == 16, "Gen3SpeciesInfo must stay 16 bytes");

static const char gen3_unknown_name[] = "???";

const Gen3SpeciesInfo* gen3_species(uint16_t national) {
    if(national < 1 || national > GEN3_NATIONAL_COUNT) return NULL;
    return &gen3_species_info_tab[national];
}

const char* gen3_species_name(uint16_t national) {
    if(national < 1 || national > GEN3_NATIONAL_COUNT) return "?";
    return gen3_species_name_tab[national];
}

uint16_t gen3_national_from_internal(uint16_t internal) {
    if(internal >= GEN3_INTERNAL_COUNT) return 0;
    return gen3_internal_to_national_tab[internal];
}

uint16_t gen3_internal_from_national(uint16_t national) {
    const Gen3SpeciesInfo* info = gen3_species(national);
    return info ? info->internal : 0;
}

size_t gen3_learnset(uint16_t national, const uint16_t** entries) {
    if(national < 1 || national > GEN3_NATIONAL_COUNT) {
        if(entries) *entries = NULL;
        return 0;
    }
    uint16_t start = gen3_learnset_start_tab[national];
    if(entries) *entries = &gen3_learnset_tab[start];
    return gen3_learnset_start_tab[national + 1] - start;
}

const char* gen3_move_name(uint16_t move) {
    if(move >= GEN3_MOVE_COUNT) return gen3_unknown_name;
    return gen3_move_name_tab[move];
}

uint8_t gen3_move_pp(uint16_t move) {
    if(move >= GEN3_MOVE_COUNT) return 0;
    return gen3_move_pp_tab[move];
}

uint16_t gen3_move_by_name(size_t i) {
    if(i >= GEN3_MOVE_COUNT - 1) return 0;
    return gen3_move_by_name_tab[i];
}

const char* gen3_item_name(uint16_t item) {
    if(item >= GEN3_ITEM_COUNT || gen3_item_name_tab[item][0] == '\0') return gen3_unknown_name;
    return gen3_item_name_tab[item];
}

bool gen3_item_holdable(uint16_t item) {
    for(size_t i = 0; i < GEN3_HOLDABLE_COUNT; i++) {
        if(gen3_holdable_by_name_tab[i] == item) return true;
    }
    return false;
}

uint16_t gen3_holdable_by_name(size_t i) {
    if(i >= GEN3_HOLDABLE_COUNT) return 0;
    return gen3_holdable_by_name_tab[i];
}

const char* gen3_ability_name(uint8_t ability) {
    if(ability >= GEN3_ABILITY_COUNT) return gen3_unknown_name;
    return gen3_ability_name_tab[ability];
}

const char* gen3_nature_name(uint8_t nature) {
    if(nature >= GEN3_NATURE_COUNT) return gen3_unknown_name;
    return gen3_nature_name_tab[nature];
}

const char* gen3_type_name(uint8_t type) {
    if(type >= GEN3_TYPE_COUNT) return gen3_unknown_name;
    return gen3_type_name_tab[type];
}
