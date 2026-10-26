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

TEST(test_rng) {
    Gen3Rng r;
    gen3_rng_seed(&r, 1);
    ASSERT_EQ(gen3_rng_next(&r), 0x00042021);
    gen3_rng_seed(&r, 0);
    ASSERT_EQ(r.state, 0x6D2B79F5);
}

static void check_find(Gen3Rng* rng, uint16_t nat, uint32_t ot_id, const Gen3Traits* want) {
    const Gen3SpeciesInfo* info = gen3_species(nat);
    Gen3Traits n = *want;
    if(info->gender_ratio == 0) n.gender = Gen3GenderMale;
    if(info->gender_ratio == 254) n.gender = Gen3GenderFemale;
    if(info->gender_ratio == 255) n.gender = Gen3GenderNone;
    if(info->ability[1] == 0) n.ability_slot = 0;
    if(nat != GEN3_NATIONAL_UNOWN) n.unown_letter = 0;
    uint32_t pid = 0;
    ASSERT_TRUE(gen3_pid_find(rng, nat, ot_id, want, &pid));
    ASSERT_EQ(pid % 25, n.nature);
    ASSERT_EQ(gen3_gender_of(info->gender_ratio, pid), n.gender);
    ASSERT_EQ(gen3_is_shiny(ot_id, pid), n.shiny);
    if(info->ability[1] != 0) ASSERT_EQ(pid & 1, n.ability_slot);
    if(nat == GEN3_NATIONAL_UNOWN) ASSERT_EQ(gen3_unown_letter(pid), n.unown_letter);
}

TEST(test_pid_find_matrix) {
    static const uint16_t species[] = {1, 280, 29, 81, 201};
    static const uint8_t letters[] = {0, 25, 26, 27};
    Gen3Rng rng;
    gen3_rng_seed(&rng, 42);
    for(unsigned si = 0; si < sizeof(species) / sizeof(species[0]); si++) {
        bool unown = species[si] == GEN3_NATIONAL_UNOWN;
        for(uint8_t nature = 0; nature < 25; nature++)
            for(int shiny = 0; shiny < 2; shiny++)
                for(int g = 0; g < 2; g++)
                    for(uint8_t ab = 0; ab < 2; ab++)
                        for(int li = 0; li < (unown ? 4 : 1); li++) {
                            Gen3Traits want = {
                                nature,
                                g ? Gen3GenderFemale : Gen3GenderMale,
                                shiny != 0,
                                ab,
                                unown ? letters[li] : 0};
                            check_find(&rng, species[si], 0x1234ABCDu, &want);
                        }
    }
}

TEST(test_pid_find_deterministic) {
    Gen3Traits want = {7, Gen3GenderFemale, true, 1, 0};
    Gen3Rng a, b;
    gen3_rng_seed(&a, 99);
    gen3_rng_seed(&b, 99);
    uint32_t p1 = 0, p2 = 0;
    ASSERT_TRUE(gen3_pid_find(&a, 280, 0xCAFEF00Du, &want, &p1));
    ASSERT_TRUE(gen3_pid_find(&b, 280, 0xCAFEF00Du, &want, &p2));
    ASSERT_EQ(p1, p2);
}

static void check_moves(uint16_t nat, uint8_t lv, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    uint16_t m[4];
    gen3_default_moves(nat, lv, m);
    ASSERT_EQ(m[0], a);
    ASSERT_EQ(m[1], b);
    ASSERT_EQ(m[2], c);
    ASSERT_EQ(m[3], d);
}

TEST(test_default_moves) {
    check_moves(252, 5, 1, 43, 0, 0);
    check_moves(252, 6, 1, 43, 71, 0);
    check_moves(252, 30, 98, 228, 103, 72);
    check_moves(2, 5, 33, 45, 73, 0);
    check_moves(2, 22, 22, 77, 79, 75);
    check_moves(201, 50, 237, 0, 0, 0);
    check_moves(1, 1, 33, 0, 0, 0);
    check_moves(0, 50, 0, 0, 0, 0);
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
    RUN(test_rng);
    RUN(test_pid_find_matrix);
    RUN(test_pid_find_deterministic);
    RUN(test_default_moves);
    TEST_EXIT();
}
