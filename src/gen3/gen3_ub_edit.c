#include "gen3_ub_edit.h"
#include "gen3_ub_data.h"
#include "gen3_rules.h"
#include "gen3_text.h"
#include <string.h>

/* The editor rules are ported from PUSE (github.com/Zannael/PUSE, MIT, commit
 * 2c624440fb24940480a3430fa17214942d4ba04a): backend/modules/party.py and pc.py. */

#define EV_STAT_MAX 252u
#define EV_TOTAL_MAX 510u
#define LEVEL_MAX 100u
#define MOVE_MAX 922u
#define NATURE_COUNT 25u

void gen3_ub_edit_load(Gen3UbEditor* e, const Gen3UbMon* m, const Gen3UbTrainer* t) {
    memset(e, 0, sizeof(*e));
    e->mon = *m;
    if(t) {
        e->trainer = *t;
        e->has_trainer = true;
    }
}

/* Called after an operation: a record whose bytes did not change is a no-op (no restat, no flag);
 * otherwise a party record is restated and the editor is marked changed. */
static void ed_commit(Gen3UbEditor* e, const Gen3UbMon* before) {
    if(memcmp(e->mon.raw, before->raw, sizeof(e->mon.raw)) == 0) return;
    if(e->mon.party) gen3_ub_party_restat(&e->mon);
    e->changed = true;
}

static bool ed_name_equal_nocase(const char* a, const char* b) {
    for(; *a && *b; a++, b++) {
        char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if(x != y) return false;
    }
    return *a == *b;
}

/* If the nickname is the old species' name (any case), makes it the new species' name in the same
 * style: all caps if it was all caps, else each word's first letter up and the rest down. */
static void ed_follow_name(Gen3UbMon* m, uint16_t old_species, uint16_t new_species) {
    char nick[GEN3_UB_MON_NICK + 1u];
    char old_name[GEN3_UB_NAME_LEN + 1u];
    char new_name[GEN3_UB_NAME_LEN + 1u];
    uint8_t raw[GEN3_UB_MON_NICK];
    gen3_ub_mon_nickname(m, raw);
    gen3_text_decode(raw, sizeof(raw), nick, sizeof(nick));
    if(!gen3_ub_species_name(old_species, old_name) || nick[0] == '\0') return;
    if(!ed_name_equal_nocase(nick, old_name)) return;
    if(!gen3_ub_species_name(new_species, new_name)) return;
    bool caps = true;
    for(const char* p = nick; *p; p++) {
        if(*p >= 'a' && *p <= 'z') caps = false;
    }
    if(!caps) {
        bool start = true;
        for(char* p = new_name; *p; p++) {
            bool upper = *p >= 'A' && *p <= 'Z';
            bool lower = *p >= 'a' && *p <= 'z';
            if(upper && !start) *p = (char)(*p + 32);
            if(lower && start) *p = (char)(*p - 32);
            start = !(upper || lower);
        }
    }
    gen3_text_encode(new_name, raw, sizeof(raw), sizeof(raw));
    gen3_ub_mon_set_nickname(m, raw);
}

/* The moves a new species starts with: the first 4 of its level-up list, PP-ups cleared, PP full */
static void ed_default_moves(Gen3UbMon* m, uint16_t species) {
    uint16_t lv[4] = {0, 0, 0, 0};
    uint8_t pp[4];
    gen3_ub_levelup(species, lv, 4);
    gen3_ub_mon_set_moves(m, lv);
    gen3_ub_mon_set_pp_ups(m, 0);
    for(uint8_t i = 0; i < 4; i++) pp[i] = gen3_ub_move_pp(lv[i]);
    gen3_ub_mon_set_pp(m, pp);
}

/* Whether the species has the ability slot (0 always) */
static bool ed_has_slot(uint16_t species, uint8_t slot) {
    uint8_t a[3];
    if(slot == 0) return true;
    if(slot > 2 || !gen3_ub_species_abilities(species, a)) return false;
    if(slot == 1) return a[1] != 0 && a[1] != a[0];
    return a[2] != 0;
}

bool gen3_ub_edit_set_species(Gen3UbEditor* e, uint16_t species) {
    Gen3UbMon* m = &e->mon;
    char name[GEN3_UB_NAME_LEN + 1u];
    int g = gen3_ub_growth(species, NULL);
    if(g < 0 || !gen3_ub_species_name(species, name)) return false;
    uint16_t old = gen3_ub_mon_species(m);
    if(old == species) return true;
    Gen3UbMon before = *m;
    uint8_t level = gen3_ub_mon_level(m);
    uint8_t nature = gen3_ub_nature(m);
    int shiny = gen3_ub_shiny(m) ? 1 : 0;
    uint8_t slot = gen3_ub_ability_slot(m);
    if(!ed_has_slot(species, slot)) slot = 0;
    gen3_ub_mon_set_species(m, species);
    if(!gen3_ub_solve_identity(m, nature, shiny, -1, slot)) {
        *m = before;
        return false;
    }
    gen3_ub_mon_set_exp(m, gen3_exp_for_level((uint8_t)g, level ? level : 1u));
    ed_default_moves(m, species);
    ed_follow_name(m, old, species);
    ed_commit(e, &before);
    return true;
}

void gen3_ub_edit_set_level(Gen3UbEditor* e, uint8_t level) {
    int g = gen3_ub_growth(gen3_ub_mon_species(&e->mon), NULL);
    if(level < 1 || level > LEVEL_MAX || g < 0) return;
    Gen3UbMon before = e->mon;
    gen3_ub_mon_set_exp(&e->mon, gen3_exp_for_level((uint8_t)g, level));
    ed_commit(e, &before);
}

bool gen3_ub_edit_set_item(Gen3UbEditor* e, uint16_t item) {
    if(item != 0 && !gen3_ub_item_holdable(item)) return false;
    Gen3UbMon before = e->mon;
    gen3_ub_mon_set_item(&e->mon, item);
    ed_commit(e, &before);
    return true;
}

bool gen3_ub_edit_set_move(Gen3UbEditor* e, uint8_t i, uint16_t move) {
    if(i > 3 || move > MOVE_MAX || (move == 0 && i == 0)) return false;
    Gen3UbMon* m = &e->mon;
    uint16_t mv[4];
    gen3_ub_mon_moves(m, mv);
    if(mv[i] == move) return true;
    Gen3UbMon before = *m;
    mv[i] = move;
    gen3_ub_mon_set_moves(m, mv);
    gen3_ub_mon_set_pp_ups(m, (uint8_t)(gen3_ub_mon_pp_ups(m) & ~(3u << (2u * i))));
    if(m->party) {
        uint8_t pp[4];
        gen3_ub_mon_pp(m, pp);
        pp[i] = gen3_ub_move_pp(move);
        gen3_ub_mon_set_pp(m, pp);
    }
    ed_commit(e, &before);
    return true;
}

/* Re-solves the PID for any of nature, shininess (-1 keeps) and ability slot (-1 keeps) */
static bool ed_identity(Gen3UbEditor* e, uint8_t nature, int shiny, int slot) {
    Gen3UbMon before = e->mon;
    if(!gen3_ub_solve_identity(&e->mon, nature, shiny, -1, slot)) return false;
    ed_commit(e, &before);
    return true;
}

bool gen3_ub_edit_set_nature(Gen3UbEditor* e, uint8_t nature) {
    if(nature >= NATURE_COUNT) return false;
    if(gen3_ub_nature(&e->mon) == nature) return true;
    return ed_identity(e, nature, -1, -1);
}

bool gen3_ub_edit_set_shiny(Gen3UbEditor* e, bool shiny) {
    if(gen3_ub_shiny(&e->mon) == shiny) return true;
    return ed_identity(e, gen3_ub_nature(&e->mon), shiny ? 1 : 0, -1);
}

bool gen3_ub_edit_set_ability(Gen3UbEditor* e, uint8_t slot) {
    if(!ed_has_slot(gen3_ub_mon_species(&e->mon), slot)) return false;
    if(gen3_ub_ability_slot(&e->mon) == slot) return true;
    return ed_identity(e, gen3_ub_nature(&e->mon), -1, slot);
}

void gen3_ub_edit_set_ivs(Gen3UbEditor* e, Gen3UbIvs p) {
    uint8_t iv[6];
    Gen3UbMon before = e->mon;
    memset(iv, p == Gen3UbIvsAll31 ? 31 : 0, sizeof(iv));
    gen3_ub_mon_set_ivs(&e->mon, iv);
    ed_commit(e, &before);
}

void gen3_ub_edit_set_evs(Gen3UbEditor* e, const uint8_t evs[6]) {
    uint8_t ev[6];
    unsigned total = 0;
    Gen3UbMon before = e->mon;
    for(int i = 0; i < 6; i++) {
        ev[i] = evs[i] > EV_STAT_MAX ? (uint8_t)EV_STAT_MAX : evs[i];
        total += ev[i];
    }
    for(int i = 5; i >= 0 && total > EV_TOTAL_MAX; i--) {
        unsigned cut = total - EV_TOTAL_MAX;
        if(cut > ev[i]) cut = ev[i];
        ev[i] = (uint8_t)(ev[i] - cut);
        total -= cut;
    }
    gen3_ub_mon_set_evs(&e->mon, ev);
    ed_commit(e, &before);
}

bool gen3_ub_edit_set_nickname(Gen3UbEditor* e, const char* ascii) {
    uint8_t raw[GEN3_UB_MON_NICK];
    if(ascii[0] == '\0' || !gen3_text_encodable(ascii, GEN3_UB_MON_NICK)) return false;
    Gen3UbMon before = e->mon;
    gen3_text_encode(ascii, raw, sizeof(raw), sizeof(raw));
    gen3_ub_mon_set_nickname(&e->mon, raw);
    ed_commit(e, &before);
    return true;
}
