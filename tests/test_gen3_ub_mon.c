#include "test.h"
#include "ub_bulk_file.h"
#include <stdbool.h>
#include <string.h>
#include <src/gen3/gen3_rules.h>
#include <src/gen3/gen3_text.h>
#include <src/gen3/gen3_ub_data.h>
#include <src/gen3/gen3_ub_mon.h>

/* The Unbound party and box record codec, stats, identity solver and new box Pokemon
 * (gen3_ub_mon.h), with the species data from the committed files/unbound_bulk.bin */

static void box_filled(Gen3UbMon* m) {
    memset(m, 0, sizeof(*m));
    for(uint8_t i = 0; i < 58; i++) m->raw[i] = (uint8_t)(0xA0 + i);
}

TEST(test_ub_box_moves_packing) {
    Gen3UbMon m;
    box_filled(&m);
    Gen3UbMon before = m;
    const uint16_t in[4] = {921, 1, 0, 600};
    gen3_ub_mon_set_moves(&m, in);
    uint16_t out[4];
    gen3_ub_mon_moves(&m, out);
    for(int i = 0; i < 4; i++) ASSERT_EQ(out[i], in[i]);
    uint64_t packed = 921u | (1u << 10) | ((uint64_t)600 << 30);
    for(int i = 0; i < 5; i++) ASSERT_EQ(m.raw[39 + i], (uint8_t)(packed >> (8 * i)));
    for(int i = 0; i < 100; i++) {
        if(i >= 39 && i < 44) continue;
        ASSERT_EQ(m.raw[i], before.raw[i]);
    }
}

TEST(test_ub_party_moves_pp) {
    Gen3UbMon m;
    memset(&m, 0, sizeof(m));
    m.party = true;
    const uint16_t in[4] = {1, 0, 0, 0};
    gen3_ub_mon_set_moves(&m, in);
    const uint8_t pp[4] = {35, 0, 0, 0};
    gen3_ub_mon_set_pp(&m, pp);
    ASSERT_EQ(m.raw[44], 1);
    ASSERT_EQ(m.raw[52], 35);
    uint8_t got[4];
    gen3_ub_mon_pp(&m, got);
    ASSERT_EQ(got[0], 35);
}

TEST(test_ub_ivs_keep_flags) {
    Gen3UbMon m;
    memset(&m, 0, sizeof(m));
    m.raw[54 + 3] = 0xC0;
    const uint8_t iv[6] = {31, 0, 15, 1, 2, 30};
    gen3_ub_mon_set_ivs(&m, iv);
    ASSERT_EQ(m.raw[57] & 0xC0, 0xC0);
    uint8_t out[6];
    gen3_ub_mon_ivs(&m, out);
    ASSERT_TRUE(memcmp(iv, out, 6) == 0);
    ASSERT_TRUE(gen3_ub_mon_hidden(&m));
    gen3_ub_mon_set_hidden(&m, false);
    ASSERT_EQ(m.raw[57] & 0xC0, 0x40);
}

TEST(test_ub_unknown_kept) {
    Gen3UbMon m;
    box_filled(&m);
    Gen3UbMon before = m;
    const uint16_t mv[4] = {1, 2, 3, 4};
    const uint8_t six[6] = {1, 2, 3, 4, 5, 6};
    uint8_t nick[10] = {0};
    uint8_t ot[7] = {0};
    gen3_ub_mon_set_species(&m, 5);
    gen3_ub_mon_set_item(&m, 6);
    gen3_ub_mon_set_exp(&m, 7);
    gen3_ub_mon_set_pp_ups(&m, 8);
    gen3_ub_mon_set_friendship(&m, 9);
    gen3_ub_mon_set_ball(&m, 10);
    gen3_ub_mon_set_moves(&m, mv);
    gen3_ub_mon_set_evs(&m, six);
    gen3_ub_mon_set_ivs(&m, six);
    gen3_ub_mon_set_hidden(&m, true);
    gen3_ub_mon_set_pid(&m, 1);
    gen3_ub_mon_set_otid(&m, 2);
    gen3_ub_mon_set_nickname(&m, nick);
    gen3_ub_mon_set_ot_name(&m, ot);
    ASSERT_EQ(m.raw[18], before.raw[18]);
    ASSERT_EQ(m.raw[19], before.raw[19]);
    for(int i = 50; i < 54; i++) ASSERT_EQ(m.raw[i], before.raw[i]);
    ASSERT_EQ(gen3_ub_mon_species(&m), 5);
    ASSERT_EQ(gen3_ub_mon_exp(&m), 7);
}

static void bulbasaur_party(Gen3UbMon* m, uint8_t level) {
    memset(m, 0, sizeof(*m));
    m->party = true;
    gen3_ub_mon_set_species(m, 1);
    gen3_ub_mon_set_pid(m, 0); /* nature 0, neutral */
    gen3_ub_mon_set_exp(m, gen3_exp_for_level(3, level));
    const uint8_t iv[6] = {31, 31, 31, 31, 31, 31};
    gen3_ub_mon_set_ivs(m, iv);
}

TEST(test_ub_party_level_stats) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbMon m;
    bulbasaur_party(&m, 50);
    m.raw[28] = 0x34;
    m.raw[29] = 0x12;
    gen3_ub_party_restat(&m);
    ASSERT_EQ(m.raw[84], 50);
    ASSERT_EQ(m.raw[88] | (m.raw[89] << 8), 120);
    ASSERT_EQ(m.raw[90], 69);
    ASSERT_EQ(m.raw[92], 69);
    ASSERT_EQ(m.raw[94], 65);
    ASSERT_EQ(m.raw[96], 85);
    ASSERT_EQ(m.raw[98], 85);
    ASSERT_EQ(m.raw[28], 0);
    ASSERT_EQ(m.raw[29], 0);
    m.raw[86] = 120;
    gen3_ub_mon_set_exp(&m, gen3_exp_for_level(3, 10));
    gen3_ub_party_restat(&m);
    ASSERT_EQ(m.raw[84], 10);
    uint16_t max_hp = (uint16_t)(m.raw[88] | (m.raw[89] << 8));
    ASSERT_TRUE(max_hp < 120);
    ASSERT_EQ(m.raw[86] | (m.raw[87] << 8), max_hp);
}

TEST(test_ub_nature_modifiers_shedinja) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbMon m;
    bulbasaur_party(&m, 50);
    gen3_ub_mon_set_pid(&m, 3); /* Adamant-like: +Atk (index 0) -SpA (index 3) */
    gen3_ub_party_restat(&m);
    ASSERT_EQ(m.raw[90], 69 * 110 / 100);
    ASSERT_EQ(m.raw[96], 85 * 90 / 100);
    gen3_ub_mon_set_species(&m, 303);
    gen3_ub_party_restat(&m);
    ASSERT_EQ(m.raw[88], 1);
}

TEST(test_ub_identity) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbMon m;
    memset(&m, 0, sizeof(m));
    gen3_ub_mon_set_species(&m, 1);
    gen3_ub_mon_set_otid(&m, 12345u | (54321u << 16));
    gen3_ub_mon_set_pid(&m, 0x12345678u);
    ASSERT_TRUE(gen3_ub_solve_identity(&m, 3, 1, -1, 1));
    ASSERT_EQ(gen3_ub_nature(&m), 3);
    ASSERT_TRUE(gen3_ub_shiny(&m));
    ASSERT_EQ(gen3_ub_mon_pid(&m) & 1u, 1);
    ASSERT_EQ(gen3_ub_ability_slot(&m), 1);
    /* a nature change keeps shininess and gender */
    int gender = gen3_ub_gender(&m);
    ASSERT_TRUE(gen3_ub_solve_identity(&m, 7, -1, -1, -1));
    ASSERT_EQ(gen3_ub_nature(&m), 7);
    ASSERT_TRUE(gen3_ub_shiny(&m));
    ASSERT_EQ(gen3_ub_gender(&m), gender);
    ASSERT_EQ(gen3_ub_mon_pid(&m) & 1u, 1);
    /* not shiny */
    ASSERT_TRUE(gen3_ub_solve_identity(&m, 7, 0, 1, 2));
    ASSERT_TRUE(!gen3_ub_shiny(&m));
    ASSERT_EQ(gen3_ub_gender(&m), 1);
    ASSERT_EQ(gen3_ub_ability_slot(&m), 2);
    ASSERT_EQ(gen3_ub_nature(&m), 7);
}

TEST(test_ub_identity_gender_impossible) {
    ASSERT_TRUE(ub_bulk_file_open());
    /* find a male-only species (threshold 0) */
    uint16_t sp = 0;
    for(uint16_t s = 1; s <= GEN3_UB_SPECIES_MAX && !sp; s++)
        if(gen3_ub_gender_threshold(s) == 0) sp = s;
    ASSERT_TRUE(sp != 0);
    Gen3UbMon m;
    memset(&m, 0, sizeof(m));
    gen3_ub_mon_set_species(&m, sp);
    uint32_t pid = gen3_ub_mon_pid(&m);
    ASSERT_TRUE(!gen3_ub_solve_identity(&m, 3, 0, 1, -1));
    ASSERT_EQ(gen3_ub_mon_pid(&m), pid);
    ASSERT_TRUE(gen3_ub_solve_identity(&m, 3, 0, 0, -1));
}

TEST(test_ub_new_box_mon) {
    ASSERT_TRUE(ub_bulk_file_open());
    Gen3UbMon owner;
    memset(&owner, 0, sizeof(owner));
    owner.party = true;
    gen3_text_encode("RED", &owner.raw[20], 7, 7);
    gen3_ub_mon_set_otid(&owner, 12345u | (54321u << 16));
    owner.raw[18] = 2;
    owner.raw[19] = 0x06;
    Gen3UbMon m;
    ASSERT_TRUE(gen3_ub_new_box_mon(&m, 1, 5, &owner));
    ASSERT_TRUE(!m.party);
    char ot[8];
    gen3_text_decode(&m.raw[20], 7, ot, sizeof(ot));
    ASSERT_STR_EQ(ot, "RED");
    ASSERT_EQ(gen3_ub_mon_otid(&m), 12345u | (54321u << 16));
    ASSERT_EQ(m.raw[18], 2);
    ASSERT_EQ(m.raw[19], 0x02);
    uint16_t lv[16], mv[4];
    uint8_t n = gen3_ub_levelup(1, lv, 16);
    gen3_ub_mon_moves(&m, mv);
    for(uint8_t i = 0; i < 4; i++) ASSERT_EQ(mv[i], i < n ? lv[i] : 0);
    ASSERT_EQ(gen3_ub_mon_level(&m), 5);
    ASSERT_EQ(gen3_ub_mon_ball(&m), 3);
    ASSERT_EQ(gen3_ub_mon_friendship(&m), 70);
    ASSERT_EQ(gen3_ub_ability_slot(&m), 0);
    uint8_t iv[6];
    gen3_ub_mon_ivs(&m, iv);
    for(int i = 0; i < 6; i++) ASSERT_EQ(iv[i], 31);
    char nick[11];
    gen3_text_decode(&m.raw[8], 10, nick, sizeof(nick));
    ASSERT_STR_EQ(nick, "BULBASAUR");
    ASSERT_TRUE(!gen3_ub_new_box_mon(&m, 2000, 5, &owner));
}

int main(void) {
    RUN(test_ub_box_moves_packing);
    RUN(test_ub_party_moves_pp);
    RUN(test_ub_ivs_keep_flags);
    RUN(test_ub_unknown_kept);
    RUN(test_ub_party_level_stats);
    RUN(test_ub_nature_modifiers_shedinja);
    RUN(test_ub_identity);
    RUN(test_ub_identity_gender_impossible);
    RUN(test_ub_new_box_mon);
    TEST_EXIT();
}
