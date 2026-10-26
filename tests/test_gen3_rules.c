#include "test.h"
#include "gen3_golden.h"
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_rules.h>

#define MF   0
#define ERR  1
#define FLU  2
#define MS   3
#define FAST 4
#define SLOW 5

TEST(test_exp_table) {
    static const uint32_t t[6][3] = {
        {8, 125000, 1000000},
        {15, 125000, 600000},
        {4, 142500, 1640000},
        {9, 117360, 1059860},
        {6, 100000, 800000},
        {10, 156250, 1250000}};
    static const uint8_t lv[3] = {2, 50, 100};
    for(int g = 0; g < 6; g++) {
        for(int i = 0; i < 3; i++) ASSERT_EQ(gen3_exp_for_level(g, lv[i]), t[g][i]);
        ASSERT_EQ(gen3_exp_for_level(g, 0), 0);
        ASSERT_EQ(gen3_exp_for_level(g, 1), 1);
        ASSERT_EQ(gen3_exp_for_level(g, 200), t[g][2]);
    }
    ASSERT_EQ(gen3_exp_for_level(ERR, 51), 131324);
    ASSERT_EQ(gen3_exp_for_level(ERR, 68), 257834);
    ASSERT_EQ(gen3_exp_for_level(ERR, 69), 267406);
    ASSERT_EQ(gen3_exp_for_level(ERR, 98), 583539);
    ASSERT_EQ(gen3_exp_for_level(ERR, 99), 591882);
    ASSERT_EQ(gen3_exp_for_level(FLU, 15), 1957);
    ASSERT_EQ(gen3_exp_for_level(FLU, 16), 2457);
    ASSERT_EQ(gen3_exp_for_level(FLU, 36), 46656);
    ASSERT_EQ(gen3_exp_for_level(FLU, 37), 50653);
}

TEST(test_level_for_exp) {
    ASSERT_EQ(gen3_level_for_exp(MS, 134), 4);
    ASSERT_EQ(gen3_level_for_exp(MS, 135), 5);
    for(int g = 0; g < 6; g++) {
        ASSERT_EQ(gen3_level_for_exp(g, 0), 0);
        ASSERT_EQ(gen3_level_for_exp(g, 1), 1);
        ASSERT_EQ(gen3_level_for_exp(g, 0xFFFFFFFF), 100);
        for(int l = 2; l <= 100; l++) {
            ASSERT_EQ(gen3_level_for_exp(g, gen3_exp_for_level(g, l)), l);
        }
    }
    ASSERT_EQ(gen3_level_for_exp(FLU, 1640000), 100);
    ASSERT_EQ(gen3_level_for_exp(ERR, 599999), 99);
}

TEST(test_nature_effect) {
    /* gNatureStatTable from pokeemerald src/pokemon.c, columns Atk, Def, Spe, SpA, SpD. */
    static const int8_t tab[25][5] = {
        {0, 0, 0, 0, 0},  {1, -1, 0, 0, 0}, {1, 0, -1, 0, 0}, {1, 0, 0, -1, 0}, {1, 0, 0, 0, -1},
        {-1, 1, 0, 0, 0}, {0, 0, 0, 0, 0},  {0, 1, -1, 0, 0}, {0, 1, 0, -1, 0}, {0, 1, 0, 0, -1},
        {-1, 0, 1, 0, 0}, {0, -1, 1, 0, 0}, {0, 0, 0, 0, 0},  {0, 0, 1, -1, 0}, {0, 0, 1, 0, -1},
        {-1, 0, 0, 1, 0}, {0, -1, 0, 1, 0}, {0, 0, -1, 1, 0}, {0, 0, 0, 0, 0},  {0, 0, 0, 1, -1},
        {-1, 0, 0, 0, 1}, {0, -1, 0, 0, 1}, {0, 0, -1, 0, 1}, {0, 0, 0, -1, 1}, {0, 0, 0, 0, 0}};
    for(int n = 0; n < 25; n++) {
        ASSERT_EQ(gen3_nature_effect(n, Gen3StatHp), 0);
        for(int s = 1; s < Gen3StatCount; s++) ASSERT_EQ(gen3_nature_effect(n, s), tab[n][s - 1]);
    }
}

TEST(test_stat_value) {
    ASSERT_EQ(gen3_stat_value(Gen3StatAtk, 49, 31, 0, 100, 3), 147);
    ASSERT_EQ(gen3_stat_value(Gen3StatSpa, 65, 31, 0, 100, 3), 149);
    ASSERT_EQ(gen3_stat_value(Gen3StatHp, 45, 31, 0, 100, 0), 231);
    ASSERT_EQ(gen3_stat_value(Gen3StatDef, 230, 31, 255, 100, 5), 614);
    ASSERT_EQ(gen3_stat_value(Gen3StatHp, 40, 31, 85, 50, 0), 126);
}

TEST(test_calc_stats_golden) {
    Gen3Pkm p;
    golden_build(&p);
    uint16_t st[Gen3StatCount];
    gen3_calc_stats(&p, st);
    const uint16_t want[Gen3StatCount] = {20, 12, 9, 13, 10, 11};
    for(int s = 0; s < Gen3StatCount; s++) ASSERT_EQ(st[s], want[s]);
}

TEST(test_shedinja_hp_one) {
    Gen3Pkm p;
    memset(&p, 0, sizeof(p));
    gen3_pkm_set(&p, Gen3FieldSpecies, 303); /* internal id of Shedinja */
    gen3_pkm_set(&p, Gen3FieldExp, gen3_exp_for_level(ERR, 50));
    for(int s = 0; s < Gen3StatCount; s++) {
        gen3_pkm_set(&p, Gen3FieldIvHp + s, 31);
        gen3_pkm_set(&p, Gen3FieldEvHp + s, 255);
    }
    uint16_t st[Gen3StatCount];
    gen3_calc_stats(&p, st);
    ASSERT_EQ(st[Gen3StatHp], 1);
    ASSERT_EQ(st[Gen3StatAtk], 142);
}

TEST(test_recalc_party) {
    Gen3Pkm p;
    memcpy(p.raw, GOLDEN_PK3, 100);
    memset(p.raw + 0x50, 0, 20);
    gen3_pkm_set(&p, Gen3FieldMail, 0xFF);
    gen3_recalc_party(&p);
    ASSERT_MEM_EQ(p.raw + 0x50, GOLDEN_PK3 + 0x50, 20);
}

TEST(test_gender_of) {
    ASSERT_EQ(gen3_gender_of(31, 30), Gen3GenderFemale);
    ASSERT_EQ(gen3_gender_of(31, 31), Gen3GenderMale);
    ASSERT_EQ(gen3_gender_of(0, 0), Gen3GenderMale);
    ASSERT_EQ(gen3_gender_of(254, 255), Gen3GenderFemale);
    ASSERT_EQ(gen3_gender_of(255, 0), Gen3GenderNone);
}

TEST(test_is_shiny) {
    ASSERT_TRUE(gen3_is_shiny(22222, (0u << 16) | (22222 ^ 7)));
    ASSERT_TRUE(!gen3_is_shiny(22222, (0u << 16) | (22222 ^ 8)));
}

TEST(test_unown_letter) {
    ASSERT_EQ(gen3_unown_letter(0x00000000), 0);
    ASSERT_EQ(gen3_unown_letter(0x00000001), 1);
    ASSERT_EQ(gen3_unown_letter(0x03030303), 3);
    ASSERT_EQ(gen3_unown_letter(0xFFFFFFFF), 3);
    ASSERT_EQ(gen3_unown_letter(0x01000000), 8);
    ASSERT_EQ(gen3_unown_letter(0x00010202), 26);
    ASSERT_EQ(gen3_unown_letter(0x00010203), 27);
}

TEST(test_traits_of_golden) {
    Gen3Pkm p;
    golden_build(&p);
    Gen3Traits t;
    gen3_traits_of(&p, &t);
    ASSERT_EQ(t.nature, 3);
    ASSERT_EQ(t.gender, Gen3GenderMale);
    ASSERT_TRUE(!t.shiny);
    ASSERT_EQ(t.ability_slot, 0);
}

int main(void) {
    RUN(test_exp_table);
    RUN(test_level_for_exp);
    RUN(test_nature_effect);
    RUN(test_stat_value);
    RUN(test_calc_stats_golden);
    RUN(test_shedinja_hp_one);
    RUN(test_recalc_party);
    RUN(test_gender_of);
    RUN(test_is_shiny);
    RUN(test_unown_letter);
    RUN(test_traits_of_golden);
    TEST_EXIT();
}
