#include "test.h"
#include "ub_bulk_file.h"
#include <stdbool.h>
#include <string.h>
#include <src/gen3/gen3_rules.h>
#include <src/gen3/gen3_text.h>
#include <src/gen3/gen3_ub_data.h>
#include <src/gen3/gen3_ub_edit.h>
#include <src/gen3/gen3_ub_mon.h>

/* The Unbound editor model (gen3_ub_edit.h), with the species data from the committed
 * files/unbound_bulk.bin */

static void set_nick(Gen3UbMon* m, const char* s) {
    uint8_t n[GEN3_UB_MON_NICK];
    gen3_text_encode(s, n, sizeof(n), sizeof(n));
    gen3_ub_mon_set_nickname(m, n);
}

static void get_nick(const Gen3UbMon* m, char out[11]) {
    uint8_t n[GEN3_UB_MON_NICK];
    gen3_ub_mon_nickname(m, n);
    gen3_text_decode(n, sizeof(n), out, 11);
}

/* A level 30 Bulbasaur, restated so its stats are consistent */
static void bulbasaur(Gen3UbEditor* e, bool party) {
    Gen3UbMon m;
    memset(&m, 0, sizeof(m));
    m.party = party;
    gen3_ub_mon_set_species(&m, 1);
    gen3_ub_mon_set_pid(&m, 0);
    gen3_ub_mon_set_exp(&m, gen3_exp_for_level(3, 30));
    const uint8_t iv[6] = {31, 31, 31, 31, 31, 31};
    gen3_ub_mon_set_ivs(&m, iv);
    set_nick(&m, "BULBASAUR");
    if(party) gen3_ub_party_restat(&m);
    gen3_ub_edit_load(e, &m, NULL);
}

TEST(test_ub_edit_species) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbEditor e;
    bulbasaur(&e, true);
    ASSERT_TRUE(!e.changed);
    ASSERT_TRUE(!e.has_trainer);
    ASSERT_TRUE(gen3_ub_edit_set_species(&e, 4));
    ASSERT_TRUE(e.changed);
    ASSERT_EQ(gen3_ub_mon_species(&e.mon), 4);
    ASSERT_EQ(gen3_ub_mon_level(&e.mon), 30);
    char nick[11];
    get_nick(&e.mon, nick);
    ASSERT_STR_EQ(nick, "CHARMANDER");
    uint16_t lv[16], mv[4];
    uint8_t n = gen3_ub_levelup(4, lv, 16);
    gen3_ub_mon_moves(&e.mon, mv);
    uint8_t pp[4];
    gen3_ub_mon_pp(&e.mon, pp);
    for(uint8_t i = 0; i < 4; i++) {
        ASSERT_EQ(mv[i], i < n ? lv[i] : 0);
        ASSERT_EQ(pp[i], gen3_ub_move_pp(mv[i]));
    }
    ASSERT_EQ(gen3_ub_mon_pp_ups(&e.mon), 0);
    uint8_t bs[6];
    ASSERT_TRUE(gen3_ub_base_stats(4, bs));
    uint16_t hp = (uint16_t)(e.mon.raw[88] | (e.mon.raw[89] << 8));
    ASSERT_EQ(hp, (2 * bs[0] + 31) * 30 / 100 + 30 + 10);
    ASSERT_TRUE(!gen3_ub_edit_set_species(&e, 2000));
    ASSERT_EQ(gen3_ub_mon_species(&e.mon), 4);

    bulbasaur(&e, true);
    set_nick(&e.mon, "SPROUT");
    ASSERT_TRUE(gen3_ub_edit_set_species(&e, 4));
    get_nick(&e.mon, nick);
    ASSERT_STR_EQ(nick, "SPROUT");

    bulbasaur(&e, false);
    set_nick(&e.mon, "Bulbasaur");
    ASSERT_TRUE(gen3_ub_edit_set_species(&e, 4));
    get_nick(&e.mon, nick);
    ASSERT_STR_EQ(nick, "Charmander");
    ASSERT_EQ(gen3_ub_mon_level(&e.mon), 30);
}

TEST(test_ub_edit_item_rules) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbEditor e;
    bulbasaur(&e, true);
    uint16_t ball = 0, key = 0;
    for(uint16_t i = 1; i <= 728 && !(ball && key); i++) {
        char nm[18];
        if(!gen3_ub_item_name(i, nm)) continue;
        if(!ball && strcmp(nm, "POKE BALL") == 0) ball = i;
        if(!key && !gen3_ub_item_holdable(i)) key = i;
    }
    ASSERT_TRUE(ball && key);
    ASSERT_TRUE(!gen3_ub_edit_set_item(&e, key));
    ASSERT_TRUE(!e.changed);
    ASSERT_EQ(gen3_ub_mon_item(&e.mon), 0);
    ASSERT_TRUE(gen3_ub_edit_set_item(&e, ball));
    ASSERT_TRUE(e.changed);
    ASSERT_EQ(gen3_ub_mon_item(&e.mon), ball);
    e.changed = false;
    ASSERT_TRUE(gen3_ub_edit_set_item(&e, ball));
    ASSERT_TRUE(!e.changed);
    ASSERT_TRUE(gen3_ub_edit_set_item(&e, 0));
    ASSERT_TRUE(e.changed);
    ASSERT_EQ(gen3_ub_mon_item(&e.mon), 0);
}

TEST(test_ub_edit_move_rules) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbEditor e;
    bulbasaur(&e, true);
    ASSERT_TRUE(gen3_ub_edit_set_move(&e, 0, 33));
    ASSERT_TRUE(!gen3_ub_edit_set_move(&e, 0, 923));
    ASSERT_TRUE(!gen3_ub_edit_set_move(&e, 0, 0));
    ASSERT_TRUE(!gen3_ub_edit_set_move(&e, 4, 33));
    uint16_t mv[4];
    gen3_ub_mon_moves(&e.mon, mv);
    ASSERT_EQ(mv[0], 33);
    ASSERT_TRUE(gen3_ub_edit_set_move(&e, 1, 922));
    /* PP-ups of the slot go, current PP becomes the base PP */
    gen3_ub_mon_set_pp_ups(&e.mon, 0xFF);
    ASSERT_TRUE(gen3_ub_edit_set_move(&e, 2, 45));
    ASSERT_EQ(gen3_ub_mon_pp_ups(&e.mon), 0xCF);
    uint8_t pp[4];
    gen3_ub_mon_pp(&e.mon, pp);
    ASSERT_EQ(pp[2], gen3_ub_move_pp(45));
    ASSERT_TRUE(gen3_ub_edit_set_move(&e, 2, 0));
    gen3_ub_mon_pp(&e.mon, pp);
    ASSERT_EQ(pp[2], 0);
    /* A box record has no PP to set */
    bulbasaur(&e, false);
    ASSERT_TRUE(gen3_ub_edit_set_move(&e, 3, 45));
    gen3_ub_mon_moves(&e.mon, mv);
    ASSERT_EQ(mv[3], 45);
}

TEST(test_ub_edit_identity_kept) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbEditor e;
    bulbasaur(&e, true);
    ASSERT_TRUE(gen3_ub_edit_set_shiny(&e, true));
    ASSERT_TRUE(gen3_ub_shiny(&e.mon));
    ASSERT_EQ(gen3_ub_nature(&e.mon), 0);
    uint8_t ab[3];
    ASSERT_TRUE(gen3_ub_species_abilities(1, ab));
    ASSERT_TRUE(gen3_ub_edit_set_ability(&e, 2) == (ab[2] != 0));
    uint8_t slot = gen3_ub_ability_slot(&e.mon);
    ASSERT_TRUE(gen3_ub_edit_set_nature(&e, 13));
    ASSERT_EQ(gen3_ub_nature(&e.mon), 13);
    ASSERT_TRUE(gen3_ub_shiny(&e.mon));
    ASSERT_EQ(gen3_ub_ability_slot(&e.mon), slot);
    ASSERT_TRUE(gen3_ub_edit_set_shiny(&e, false));
    ASSERT_TRUE(!gen3_ub_shiny(&e.mon));
    ASSERT_EQ(gen3_ub_nature(&e.mon), 13);
    ASSERT_EQ(gen3_ub_ability_slot(&e.mon), slot);
    ASSERT_TRUE(!gen3_ub_edit_set_nature(&e, 25));
    e.changed = false;
    ASSERT_TRUE(gen3_ub_edit_set_nature(&e, 13));
    ASSERT_TRUE(gen3_ub_edit_set_shiny(&e, false));
    ASSERT_TRUE(!e.changed);
    /* The nature moves the stats of a party Pokemon */
    ASSERT_TRUE(gen3_ub_edit_set_nature(&e, 0));
    uint8_t atk = e.mon.raw[90];
    ASSERT_TRUE(gen3_ub_edit_set_nature(&e, 3)); /* +Atk */
    ASSERT_EQ(e.mon.raw[90], atk * 110 / 100);
}

TEST(test_ub_edit_ability_slots) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbEditor e;
    bulbasaur(&e, true);
    /* Find a species with no second ability and one with a hidden one */
    uint16_t no2 = 0, hid = 0;
    for(uint16_t s = 1; s <= GEN3_UB_SPECIES_MAX && !(no2 && hid); s++) {
        uint8_t a[3];
        if(!gen3_ub_species_abilities(s, a) || gen3_ub_growth(s, NULL) < 0) continue;
        if(!no2 && (a[1] == 0 || a[1] == a[0]) && a[2] != 0 && a[0] != 0) no2 = s;
        if(!hid && a[2] != 0 && a[1] != 0 && a[1] != a[0]) hid = s;
    }
    ASSERT_TRUE(no2 && hid);
    ASSERT_TRUE(gen3_ub_edit_set_species(&e, no2));
    ASSERT_TRUE(!gen3_ub_edit_set_ability(&e, 1));
    ASSERT_TRUE(!gen3_ub_edit_set_ability(&e, 3));
    ASSERT_TRUE(gen3_ub_edit_set_ability(&e, 2));
    ASSERT_TRUE(gen3_ub_mon_hidden(&e.mon));
    ASSERT_EQ(gen3_ub_ability_slot(&e.mon), 2);
    ASSERT_TRUE(gen3_ub_edit_set_ability(&e, 0));
    ASSERT_TRUE(!gen3_ub_mon_hidden(&e.mon));
    ASSERT_EQ(gen3_ub_ability_slot(&e.mon), 0);
    ASSERT_TRUE(gen3_ub_edit_set_species(&e, hid));
    ASSERT_TRUE(gen3_ub_edit_set_ability(&e, 1));
    ASSERT_EQ(gen3_ub_ability_slot(&e.mon), 1);
    /* The slot is kept by a species change where the new species has it, else slot 0 */
    ASSERT_TRUE(gen3_ub_edit_set_species(&e, no2));
    ASSERT_EQ(gen3_ub_ability_slot(&e.mon), 0);
}

TEST(test_ub_edit_evs_clamp) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbEditor e;
    bulbasaur(&e, true);
    const uint8_t all[6] = {252, 252, 252, 252, 252, 252};
    gen3_ub_edit_set_evs(&e, all);
    uint8_t got[6];
    gen3_ub_mon_evs(&e.mon, got);
    unsigned total = 0;
    for(int i = 0; i < 6; i++) total += got[i];
    ASSERT_EQ(total, 510);
    ASSERT_EQ(got[0], 252);
    ASSERT_EQ(got[1], 252);
    ASSERT_EQ(got[2], 6);
    ASSERT_EQ(got[3], 0);
    ASSERT_EQ(got[5], 0);
    ASSERT_TRUE(e.changed);
    const uint8_t big[6] = {255, 0, 0, 0, 0, 255};
    gen3_ub_edit_set_evs(&e, big);
    gen3_ub_mon_evs(&e.mon, got);
    ASSERT_EQ(got[0], 252);
    ASSERT_EQ(got[5], 252);
}

TEST(test_ub_edit_level_ivs_nickname) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbEditor e;
    bulbasaur(&e, true);
    gen3_ub_edit_set_level(&e, 50);
    ASSERT_EQ(gen3_ub_mon_exp(&e.mon), gen3_exp_for_level(3, 50));
    ASSERT_EQ(e.mon.raw[84], 50);
    ASSERT_EQ(e.mon.raw[88] | (e.mon.raw[89] << 8), 120);
    e.changed = false;
    gen3_ub_edit_set_level(&e, 50);
    ASSERT_TRUE(!e.changed);
    gen3_ub_edit_set_level(&e, 0);
    gen3_ub_edit_set_level(&e, 101);
    ASSERT_EQ(e.mon.raw[84], 50);
    ASSERT_TRUE(!e.changed);

    gen3_ub_edit_set_ivs(&e, Gen3UbIvsAll0);
    uint8_t iv[6];
    gen3_ub_mon_ivs(&e.mon, iv);
    for(int i = 0; i < 6; i++) ASSERT_EQ(iv[i], 0);
    ASSERT_TRUE(e.changed);
    gen3_ub_edit_set_ivs(&e, Gen3UbIvsAll31);
    gen3_ub_mon_ivs(&e.mon, iv);
    for(int i = 0; i < 6; i++) ASSERT_EQ(iv[i], 31);
    ASSERT_EQ(e.mon.raw[88] | (e.mon.raw[89] << 8), 120);

    char nick[11];
    ASSERT_TRUE(gen3_ub_edit_set_nickname(&e, "ASH"));
    get_nick(&e.mon, nick);
    ASSERT_STR_EQ(nick, "ASH");
    ASSERT_TRUE(!gen3_ub_edit_set_nickname(&e, ""));
    ASSERT_TRUE(!gen3_ub_edit_set_nickname(&e, "ELEVENCHARS"));
    ASSERT_TRUE(!gen3_ub_edit_set_nickname(&e, "A~B"));
    get_nick(&e.mon, nick);
    ASSERT_STR_EQ(nick, "ASH");
}

int main(void) {
    RUN(test_ub_edit_species);
    RUN(test_ub_edit_item_rules);
    RUN(test_ub_edit_move_rules);
    RUN(test_ub_edit_identity_kept);
    RUN(test_ub_edit_ability_slots);
    RUN(test_ub_edit_evs_clamp);
    RUN(test_ub_edit_level_ivs_nickname);
    TEST_EXIT();
}
