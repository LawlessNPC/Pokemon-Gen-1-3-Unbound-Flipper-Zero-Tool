#include "test.h"
#include "gen3_golden.h"
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_rules.h>

#define G(f) gen3_pkm_get(&ed.pkm, Gen3Field##f)

#define RULE(cond, what) \
    do { \
        if(!(cond)) { \
            printf("  edit_consistent: %s\n", what); \
            return false; \
        } \
    } while(0)

/* The game-valid invariant. full_hp: Hp == MaxHp (every mutator); else Hp <= MaxHp (a loaded
 * Pokemon keeps its current HP). */
static bool edit_consistent_hp(const Gen3Editor* ed, bool full_hp) {
    const Gen3Pkm* p = &ed->pkm;
    RULE(gen3_pkm_checksum(p) == gen3_pkm_get(p, Gen3FieldChecksum), "checksum mismatch");
    RULE(gen3_pkm_get(p, Gen3FieldHasSpecies) == 1, "HasSpecies is 0");
    RULE(gen3_pkm_get(p, Gen3FieldIsEgg) == 0, "IsEgg set");
    RULE(gen3_pkm_get(p, Gen3FieldEggFlag) == 0, "EggFlag set");
    RULE(gen3_pkm_get(p, Gen3FieldBadEgg) == 0, "BadEgg set");
    const Gen3SpeciesInfo* info =
        gen3_species(gen3_national_from_internal(gen3_pkm_get(p, Gen3FieldSpecies)));
    RULE(info != NULL, "species maps to no national");
    uint8_t level = gen3_level_for_exp(info->growth, gen3_pkm_get(p, Gen3FieldExp));
    RULE(level >= 1, "exp below level 1");
    RULE(gen3_pkm_get(p, Gen3FieldLevel) == level, "Level != level_for_exp");
    uint16_t st[Gen3StatCount];
    gen3_calc_stats(p, st);
    for(int s = 0; s < Gen3StatCount; s++)
        RULE(gen3_pkm_get(p, Gen3FieldMaxHp + s) == st[s], "stats != calc_stats");
    uint32_t hp = gen3_pkm_get(p, Gen3FieldHp);
    RULE(full_hp ? hp == st[Gen3StatHp] : hp <= st[Gen3StatHp], "Hp vs MaxHp");
    RULE(
        info->ability[1] != 0 || gen3_pkm_get(p, Gen3FieldAbilityBit) == 0,
        "ability bit set on a one-ability species");
    int n = 0;
    bool gap = false;
    for(int s = 0; s < 4; s++) {
        uint32_t m = gen3_pkm_get(p, Gen3FieldMove1 + s);
        if(m == 0) {
            gap = true;
            continue;
        }
        RULE(!gap, "moves not packed");
        for(int t = 0; t < s; t++)
            RULE(gen3_pkm_get(p, Gen3FieldMove1 + t) != m, "duplicate move");
        n++;
    }
    RULE(n >= 1, "no moves");
    return true;
}

static bool edit_consistent(const Gen3Editor* ed) {
    return edit_consistent_hp(ed, true);
}

static void golden_pkm(Gen3Pkm* p) {
    memcpy(p->raw, GOLDEN_PK3, sizeof(p->raw));
}

static bool moves_are(const Gen3Editor* ed, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    const uint16_t want[4] = {a, b, c, d};
    for(int s = 0; s < 4; s++)
        if(gen3_pkm_get(&ed->pkm, Gen3FieldMove1 + s) != want[s]) return false;
    return true;
}

static bool pp_are(const Gen3Editor* ed, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t want[4] = {a, b, c, d};
    for(int s = 0; s < 4; s++)
        if(gen3_pkm_get(&ed->pkm, Gen3FieldPp1 + s) != want[s]) return false;
    return true;
}

TEST(test_edit_init_defaults) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    char buf[16];
    ASSERT_EQ(gen3_edit_national(&ed), 1);
    ASSERT_EQ(G(Species), 1);
    ASSERT_EQ(G(Level), 5);
    ASSERT_EQ(G(Exp), 135);
    gen3_pkm_nickname(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "BULBASAUR");
    gen3_pkm_ot_name(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "FLIPPER");
    ASSERT_EQ(G(OtId), 22222);
    ASSERT_EQ(G(OtGender), 0);
    ASSERT_TRUE(moves_are(&ed, 33, 45, 0, 0));
    ASSERT_TRUE(pp_are(&ed, 35, 40, 0, 0));
    ASSERT_EQ(G(PpBonuses), 0);
    ASSERT_EQ(G(HeldItem), 0);
    ASSERT_EQ(G(Ball), 4);
    ASSERT_EQ(G(MetLocation), 0xFF);
    ASSERT_EQ(G(MetLevel), 5);
    ASSERT_EQ(G(MetGame), 2);
    ASSERT_EQ(G(Friendship), 70);
    ASSERT_EQ(G(Language), 2);
    ASSERT_EQ(G(Mail), 0xFF);
    ASSERT_EQ(G(Status), 0);
    ASSERT_EQ(G(Pokerus), 0);
    ASSERT_EQ(G(Fateful), 0);
    for(int s = 0; s < Gen3StatCount; s++) ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), 0);
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), G(Pid)));
    ASSERT_EQ(ed.preset, Gen3PresetRandIvZeroEv);
    ASSERT_TRUE(ed.preset_applied);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_species_resets_keeps_ot) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_ot_name(&ed, "ASH");
    gen3_edit_set_ot_id(&ed, 1234, 4321);
    gen3_pkm_set(&ed.pkm, Gen3FieldOtGender, 1); /* no editor setter; the next edit commits it */
    gen3_pkm_set(&ed.pkm, Gen3FieldPpBonuses, 0xFF);
    gen3_pkm_set(&ed.pkm, Gen3FieldRibbons, 0x00ABCDEF);
    gen3_pkm_set(&ed.pkm, Gen3FieldCoolness, 9);
    gen3_pkm_set(&ed.pkm, Gen3FieldMarkings, 0x0F);
    gen3_pkm_set(&ed.pkm, Gen3FieldStatus, 0x08);
    gen3_edit_set_level(&ed, 30);
    gen3_edit_set_item(&ed, 200);
    gen3_edit_set_ball(&ed, 1);
    gen3_edit_set_pokerus(&ed, 0x41);
    gen3_edit_set_nickname(&ed, "LEAFY");
    gen3_edit_set_preset(&ed, Gen3PresetMaxIvLevelEv);

    gen3_edit_set_species(&ed, 252);
    char buf[16];
    ASSERT_EQ(gen3_edit_national(&ed), 252);
    gen3_pkm_nickname(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "TREECKO");
    ASSERT_EQ(G(Level), 30);
    ASSERT_EQ(G(Exp), 21760);
    gen3_pkm_ot_name(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "ASH");
    ASSERT_EQ(G(OtId), (4321u << 16) | 1234u);
    ASSERT_EQ(G(OtGender), 1);
    ASSERT_EQ(G(HeldItem), 0);
    ASSERT_EQ(G(Ball), 4);
    ASSERT_EQ(G(Pokerus), 0);
    ASSERT_TRUE(moves_are(&ed, 98, 228, 103, 72));
    ASSERT_TRUE(pp_are(&ed, 30, 20, 40, 10)); /* full base PP */
    ASSERT_EQ(G(PpBonuses), 0);
    ASSERT_EQ(G(MetLevel), 30);
    ASSERT_EQ(G(MetLocation), 0xFF);
    ASSERT_EQ(G(MetGame), 2);
    ASSERT_EQ(G(Friendship), 70);
    ASSERT_EQ(G(Ribbons), 0);
    ASSERT_EQ(G(Coolness), 0);
    ASSERT_EQ(G(Markings), 0);
    ASSERT_EQ(G(Status), 0);
    ASSERT_EQ(G(Mail), 0xFF);
    ASSERT_EQ(G(Language), 2);
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), G(Pid)));
    /* The preset is kept and applied to the new Pokemon. */
    ASSERT_EQ(ed.preset, Gen3PresetMaxIvLevelEv);
    ASSERT_TRUE(ed.preset_applied);
    for(int s = 0; s < Gen3StatCount; s++) {
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldIvHp + s), 31);
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), 85 * 30 / 100);
    }
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_species_stores_internal) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 252);
    ASSERT_EQ(G(Species), 277);
    ASSERT_EQ(ed.pkm.raw[0x20], 0x15);
    ASSERT_EQ(ed.pkm.raw[0x21], 0x01);
    ASSERT_EQ(gen3_edit_national(&ed), 252);
    ASSERT_TRUE(edit_consistent(&ed));
    gen3_edit_set_species(&ed, 386);
    ASSERT_EQ(G(Species), 410);
    ASSERT_EQ(gen3_edit_national(&ed), 386);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_species_same_is_noop) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_nickname(&ed, "LEAFY");
    Gen3Pkm before = ed.pkm;
    uint32_t rng = ed.rng.state;
    gen3_edit_set_species(&ed, 1);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    gen3_edit_set_species(&ed, 0);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    gen3_edit_set_species(&ed, 387);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_EQ(ed.rng.state, rng);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_species_skips_shiny_pid) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    /* Make the next random PID shiny for this OT: the new Pokemon must not take it. */
    Gen3Rng peek = ed.rng;
    uint32_t r = gen3_rng_next(&peek);
    uint16_t tid = (r >> 16) ^ (r & 0xFFFF);
    gen3_pkm_set(&ed.pkm, Gen3FieldOtId, tid);
    ASSERT_TRUE(gen3_is_shiny(tid, r));
    gen3_edit_set_species(&ed, 4);
    ASSERT_TRUE(G(Pid) != r);
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), G(Pid)));
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_species_fateful) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 151);
    ASSERT_EQ(G(Fateful), 1);
    ASSERT_EQ(G(Friendship), 100); /* species base friendship, not a fixed 70 */
    ASSERT_TRUE(edit_consistent(&ed));
    gen3_edit_set_species(&ed, 386);
    ASSERT_EQ(G(Fateful), 1);
    ASSERT_EQ(G(Friendship), 0);
    ASSERT_TRUE(edit_consistent(&ed));
    gen3_edit_set_species(&ed, 1);
    ASSERT_EQ(G(Fateful), 0);
    ASSERT_EQ(G(Friendship), 70);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_all_species) {
    static const uint8_t levels[2] = {1, 100};
    for(int li = 0; li < 2; li++) {
        for(uint16_t n = 1; n <= GEN3_NATIONAL_COUNT; n++) {
            const Gen3SpeciesInfo* info = gen3_species(n);
            Gen3Editor ed;
            gen3_edit_init(&ed, n);
            gen3_edit_set_level(&ed, levels[li]);
            gen3_edit_set_species(&ed, n);
            bool ok = gen3_edit_national(&ed) == n && G(Species) == info->internal &&
                      G(Level) == levels[li] && G(Friendship) == info->friendship &&
                      edit_consistent(&ed);
            /* D6: a new Pokemon's ability slot is pid & 1 on a two-ability species. */
            if(info->ability[1] != 0) ok = ok && G(AbilityBit) == (G(Pid) & 1);
            for(int s = 0; ok && s < 4; s++) {
                uint16_t m = gen3_pkm_get(&ed.pkm, Gen3FieldMove1 + s);
                ok = gen3_pkm_get(&ed.pkm, Gen3FieldPp1 + s) == (m ? gen3_move_pp(m) : 0);
            }
            if(!ok) printf("  national %u level %u\n", n, levels[li]);
            ASSERT_TRUE(ok);
        }
    }
}

TEST(test_edit_level) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_level(&ed, 50);
    ASSERT_EQ(G(Level), 50);
    ASSERT_EQ(G(Exp), 117360);
    ASSERT_TRUE(moves_are(&ed, 33, 45, 0, 0));
    ASSERT_EQ(G(MetLevel), 5);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_level_clamped) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_level(&ed, 0);
    ASSERT_EQ(G(Level), 1);
    ASSERT_EQ(G(Exp), 1);
    ASSERT_TRUE(edit_consistent(&ed));
    gen3_edit_set_level(&ed, 101);
    ASSERT_EQ(G(Level), 100);
    ASSERT_EQ(G(Exp), 1059860);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_presets) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_level(&ed, 50);
    /* EV per preset at level 50: zero, 85 * 50 / 100, max. */
    static const uint8_t ev[6] = {0, 42, 85, 0, 42, 85};
    ASSERT_EQ(GEN3_PRESET_COUNT, 6);
    ASSERT_EQ(Gen3PresetMaxIvMaxEv + 1, GEN3_PRESET_COUNT);
    for(int pr = Gen3PresetRandIvZeroEv; pr < GEN3_PRESET_COUNT; pr++) {
        gen3_edit_set_preset(&ed, (Gen3Preset)pr);
        ASSERT_EQ(ed.preset, pr);
        ASSERT_TRUE(ed.preset_applied);
        int sum = 0;
        for(int s = 0; s < Gen3StatCount; s++) {
            uint32_t iv = gen3_pkm_get(&ed.pkm, Gen3FieldIvHp + s);
            ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), ev[pr]);
            sum += gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s);
            if(pr >= Gen3PresetMaxIvZeroEv) ASSERT_EQ(iv, 31);
        }
        if(pr == Gen3PresetMaxIvMaxEv) ASSERT_EQ(sum, 510);
        ASSERT_TRUE(edit_consistent(&ed));
    }
    /* Random IVs: 5 bits each of the next RNG word, so each application re-rolls them. */
    for(int round = 0; round < 2; round++) {
        Gen3Rng peek = ed.rng;
        uint32_t r = gen3_rng_next(&peek);
        gen3_edit_set_preset(&ed, Gen3PresetRandIvLevelEv);
        for(int s = 0; s < Gen3StatCount; s++)
            ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldIvHp + s), (r >> (5 * s)) & 31);
        ASSERT_EQ(ed.rng.state, peek.state);
    }
    Gen3Pkm before = ed.pkm;
    gen3_edit_set_preset(&ed, (Gen3Preset)GEN3_PRESET_COUNT);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_EQ(ed.preset, Gen3PresetRandIvLevelEv);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_level_ev_follows_level) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_preset(&ed, Gen3PresetMaxIvLevelEv);
    gen3_edit_set_level(&ed, 100);
    for(int s = 0; s < Gen3StatCount; s++) ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), 85);
    ASSERT_TRUE(edit_consistent(&ed));
    gen3_edit_set_level(&ed, 20);
    for(int s = 0; s < Gen3StatCount; s++) ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), 17);
    ASSERT_TRUE(edit_consistent(&ed));
    /* A MaxEv preset doesn't follow the level. */
    gen3_edit_set_preset(&ed, Gen3PresetRandIvMaxEv);
    gen3_edit_set_level(&ed, 40);
    for(int s = 0; s < Gen3StatCount; s++) ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), 85);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_loaded_evs_kept) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_preset(&ed, Gen3PresetMaxIvLevelEv);
    Gen3Pkm in;
    golden_pkm(&in);
    static const uint8_t evs[6] = {252, 252, 6, 0, 0, 0};
    for(int s = 0; s < Gen3StatCount; s++) gen3_pkm_set(&in, Gen3FieldEvHp + s, evs[s]);
    gen3_pkm_fix_checksum(&in);
    gen3_edit_load(&ed, &in);
    ASSERT_TRUE(!ed.preset_applied);
    ASSERT_EQ(ed.preset, Gen3PresetMaxIvLevelEv);
    gen3_edit_set_level(&ed, 40);
    ASSERT_EQ(G(Level), 40);
    for(int s = 0; s < Gen3StatCount; s++) {
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), evs[s]);
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldIvHp + s), 31 - s); /* golden IVs */
    }
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_nickname_default) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 29);
    gen3_edit_set_nickname(&ed, "LEAFY");
    gen3_edit_set_nickname(&ed, "");
    static const uint8_t want[10] = {0xC8, 0xC3, 0xBE, 0xC9, 0xCC, 0xBB, 0xC8, 0xB6, 0xFF, 0xFF};
    ASSERT_MEM_EQ(ed.pkm.raw + 0x08, want, 10);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_set_move_pp) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_pkm_set(&ed.pkm, Gen3FieldPpBonuses, 0xFF);
    ASSERT_TRUE(gen3_edit_set_move(&ed, 2, 76));
    ASSERT_EQ(G(Move3), 76);
    ASSERT_EQ(G(Pp3), 10);
    ASSERT_EQ(G(PpBonuses), 0xCF);
    ASSERT_TRUE(moves_are(&ed, 33, 45, 76, 0));
    ASSERT_TRUE(pp_are(&ed, 35, 40, 10, 0));
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_set_move_packs) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_pkm_set(&ed.pkm, Gen3FieldPpBonuses, 0x09); /* slot 0: 1 PP Up, slot 1: 2 */
    ASSERT_TRUE(gen3_edit_set_move(&ed, 0, 0));
    ASSERT_TRUE(moves_are(&ed, 45, 0, 0, 0));
    ASSERT_EQ(G(Pp1), 40);
    ASSERT_EQ(G(PpBonuses), 0x02); /* Growl's PP Ups moved with it */
    ASSERT_TRUE(edit_consistent(&ed));
    ASSERT_TRUE(gen3_edit_set_move(&ed, 3, 22));
    ASSERT_TRUE(moves_are(&ed, 45, 22, 0, 0));
    ASSERT_TRUE(pp_are(&ed, 40, 10, 0, 0));
    ASSERT_EQ(G(PpBonuses), 0x02);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_set_move_rejects) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    Gen3Pkm before = ed.pkm;
    ASSERT_TRUE(!gen3_edit_set_move(&ed, 1, 33));
    ASSERT_TRUE(!gen3_edit_set_move(&ed, 3, 45));
    ASSERT_TRUE(!gen3_edit_set_move(&ed, 4, 22));
    ASSERT_TRUE(!gen3_edit_set_move(&ed, 2, GEN3_MOVE_COUNT));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(gen3_edit_set_move(&ed, 1, 0));
    ASSERT_TRUE(moves_are(&ed, 33, 0, 0, 0));
    before = ed.pkm;
    ASSERT_TRUE(!gen3_edit_set_move(&ed, 0, 0));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_default_moves) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_level(&ed, 30);
    ASSERT_TRUE(gen3_edit_set_move(&ed, 0, 76));
    ASSERT_TRUE(gen3_edit_set_move(&ed, 1, 0));
    gen3_pkm_set(&ed.pkm, Gen3FieldPpBonuses, 0xFF);
    gen3_edit_default_moves(&ed);
    uint16_t want[4];
    gen3_default_moves(1, 30, want);
    ASSERT_TRUE(want[3] != 0); /* four moves at level 30 */
    for(int s = 0; s < 4; s++) {
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldMove1 + s), want[s]);
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldPp1 + s), gen3_move_pp(want[s]));
    }
    ASSERT_EQ(G(PpBonuses), 0);
    ASSERT_TRUE(edit_consistent(&ed));
}

static bool traits_match(const Gen3Traits* got, const Gen3Traits* want, bool unown) {
    return got->nature == want->nature && got->gender == want->gender &&
           got->shiny == want->shiny && got->ability_slot == want->ability_slot &&
           (!unown || got->unown_letter == want->unown_letter);
}

TEST(test_edit_traits_matrix) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 280); /* Ralts: gender ratio 127, two abilities */
    for(uint8_t nature = 0; nature < 25; nature++)
        for(int g = 0; g < 2; g++)
            for(int shiny = 0; shiny < 2; shiny++)
                for(uint8_t slot = 0; slot < 2; slot++) {
                    Gen3Traits want = {
                        nature, g ? Gen3GenderFemale : Gen3GenderMale, shiny != 0, slot, 0};
                    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
                    Gen3Traits got;
                    gen3_traits_of(&ed.pkm, &got);
                    ASSERT_TRUE(traits_match(&got, &want, false));
                    ASSERT_EQ(G(AbilityBit), G(Pid) & 1);
                    ASSERT_TRUE(edit_consistent(&ed));
                }
}

TEST(test_edit_traits_static_gender) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 29); /* female only */
    Gen3Traits want;
    gen3_traits_of(&ed.pkm, &want);
    want.gender = Gen3GenderMale;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    Gen3Traits got;
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_EQ(got.gender, Gen3GenderFemale);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_ability_bit_single_ability) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7); /* Bulbasaur: one ability */
    bool odd = false;
    for(int i = 0; i < 100; i++) {
        Gen3Gender gender = (i / 25) & 1 ? Gen3GenderFemale : Gen3GenderMale;
        Gen3Traits want = {i % 25, gender, false, i & 1, 0}; /* slot 1 asked half the time */
        ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
        ASSERT_EQ(G(AbilityBit), 0);
        ASSERT_EQ(G(Pid) % 25, i % 25);
        if(G(Pid) & 1) odd = true;
        ASSERT_TRUE(edit_consistent(&ed));
    }
    ASSERT_TRUE(odd);
}

TEST(test_edit_unown_letters) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 201);
    for(uint8_t letter = 0; letter < 28; letter++) {
        Gen3Traits want;
        gen3_traits_of(&ed.pkm, &want);
        want.unown_letter = letter;
        ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
        ASSERT_EQ(gen3_unown_letter(G(Pid)), letter);
        Gen3Traits got;
        gen3_traits_of(&ed.pkm, &got);
        ASSERT_TRUE(traits_match(&got, &want, true));
        ASSERT_TRUE(edit_consistent(&ed));
    }
}

/* Asking for the traits it already has (after normalising the want as gen3_pid_find does)
 * keeps all 100 bytes: no new PID, no recalculation. */
TEST(test_edit_traits_same_keeps_pid) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7); /* Bulbasaur: gendered, one ability, not Unown */
    Gen3Pkm before = ed.pkm;
    Gen3Traits want;
    gen3_traits_of(&ed.pkm, &want);
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    /* Ignored: the slot of a one-ability species, the letter of a non-Unown */
    want.ability_slot = !want.ability_slot;
    want.unown_letter = (want.unown_letter + 1) % GEN3_UNOWN_LETTER_COUNT;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    /* An out-of-range want is still refused, untouched */
    want.nature = GEN3_NATURE_COUNT;
    ASSERT_TRUE(!gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    /* The gender of a gendered species is not ignored: the other one is a real change */
    gen3_traits_of(&ed.pkm, &want);
    want.gender = want.gender == Gen3GenderMale ? Gen3GenderFemale : Gen3GenderMale;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_TRUE(G(Pid) != gen3_pkm_get(&before, Gen3FieldPid));
    Gen3Traits got;
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_EQ(got.gender, want.gender);
    ASSERT_EQ(got.nature, want.nature);
    ASSERT_TRUE(edit_consistent(&ed));

    /* Ignored: the gender of a female-only species */
    gen3_edit_set_species(&ed, 29);
    before = ed.pkm;
    gen3_traits_of(&ed.pkm, &want);
    ASSERT_EQ(want.gender, Gen3GenderFemale);
    want.gender = Gen3GenderMale;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);

    /* Unown (genderless): the gender is ignored, the letter is not */
    gen3_edit_set_species(&ed, GEN3_NATIONAL_UNOWN);
    before = ed.pkm;
    gen3_traits_of(&ed.pkm, &want);
    want.gender = Gen3GenderMale;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    want.unown_letter = (want.unown_letter + 1) % GEN3_UNOWN_LETTER_COUNT;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_TRUE(G(Pid) != gen3_pkm_get(&before, Gen3FieldPid));
    ASSERT_EQ(gen3_unown_letter(G(Pid)), want.unown_letter);
    ASSERT_TRUE(edit_consistent(&ed));

    /* A loaded two-ability Pokemon whose stored slot isn't pid & 1, with HP below MaxHp: the
     * same traits keep the PID, the slot and the HP. */
    gen3_edit_set_species(&ed, 280); /* Ralts */
    Gen3Pkm in = ed.pkm;
    uint32_t bit = !(gen3_pkm_get(&in, Gen3FieldPid) & 1);
    gen3_pkm_set(&in, Gen3FieldAbilityBit, bit);
    gen3_pkm_set(&in, Gen3FieldHp, 1);
    gen3_pkm_fix_checksum(&in);
    gen3_edit_load(&ed, &in);
    ASSERT_EQ(G(AbilityBit), bit);
    ASSERT_EQ(G(Hp), 1);
    before = ed.pkm;
    gen3_traits_of(&ed.pkm, &want);
    ASSERT_EQ(want.ability_slot, bit);
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent_hp(&ed, false));
    /* The other slot is a real change */
    want.ability_slot = !bit;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_EQ(G(AbilityBit), !bit);
    ASSERT_EQ(G(AbilityBit), G(Pid) & 1);
    ASSERT_TRUE(edit_consistent(&ed));

    /* An unknown species (copied raw by gen3_edit_load) is still refused, untouched */
    in = ed.pkm;
    gen3_pkm_set(&in, Gen3FieldSpecies, 260); /* a "?" placeholder */
    gen3_edit_load(&ed, &in);
    before = ed.pkm;
    gen3_traits_of(&ed.pkm, &want);
    ASSERT_TRUE(!gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
}

TEST(test_edit_traits_preserve_other_bytes) {
    Gen3Pkm in;
    golden_pkm(&in);
    gen3_pkm_set(&in, Gen3FieldRibbons, 0x80ABCDEF);
    gen3_pkm_set(&in, Gen3FieldMarkings, 0x0F);
    for(int c = 0; c < 6; c++) gen3_pkm_set(&in, Gen3FieldCoolness + c, c + 1);
    gen3_pkm_set(&in, Gen3FieldPpBonuses, 0xE4);
    gen3_pkm_set(&in, Gen3FieldPokerus, 0x41);
    gen3_pkm_set(&in, Gen3FieldMetLocation, 0x10);
    gen3_pkm_set(&in, Gen3FieldMetLevel, 3);
    gen3_pkm_set(&in, Gen3FieldMetGame, 1);
    gen3_pkm_set(&in, Gen3FieldBall, 2);
    gen3_pkm_set(&in, Gen3FieldOtGender, 1);
    for(int s = 0; s < Gen3StatCount; s++) gen3_pkm_set(&in, Gen3FieldEvHp + s, 10 * (s + 1));
    gen3_pkm_set(&in, Gen3FieldHeldItem, 200);
    gen3_pkm_set(&in, Gen3FieldGrowthUnknown, 0xBEEF);
    gen3_pkm_set(&in, Gen3FieldFiller, 0x1234);
    gen3_pkm_fix_checksum(&in);

    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_load(&ed, &in);
    Gen3Pkm before = ed.pkm;
    Gen3Traits want;
    gen3_traits_of(&ed.pkm, &want);
    ASSERT_TRUE(want.nature != 4);
    want.nature = 4;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_EQ(G(Pid) % 25, 4);
    /* The new PID re-orders the substructures (seed 7 guarantees it), so encrypt must use it. */
    ASSERT_TRUE(G(Pid) % 24 != gen3_pkm_get(&before, Gen3FieldPid) % 24);
    for(int i = 0; i < GEN3_PARTY_SIZE; i++) {
        if(i <= 0x03 || i == 0x1C || i == 0x1D || (i >= 0x56 && i <= 0x63)) continue;
        if(i == 0x4B) {
            ASSERT_EQ(ed.pkm.raw[i] & 0x7F, before.raw[i] & 0x7F);
            continue;
        }
        if(ed.pkm.raw[i] != before.raw[i]) printf("  byte 0x%02X changed\n", i);
        ASSERT_EQ(ed.pkm.raw[i], before.raw[i]);
    }
    uint8_t game[GEN3_PARTY_SIZE];
    gen3_pkm_encrypt(&ed.pkm, game);
    Gen3Pkm back;
    ASSERT_TRUE(gen3_pkm_decrypt(game, sizeof(game), &back));
    ASSERT_MEM_EQ(back.raw, ed.pkm.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_ot_id_keeps_shiny) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 280); /* Ralts: gendered, two abilities */
    Gen3Traits want = {10, Gen3GenderFemale, true, 1, 0};
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    gen3_edit_set_ot_id(&ed, 12345, 54321);
    ASSERT_EQ(G(OtId), (54321u << 16) | 12345u);
    Gen3Traits got;
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_TRUE(traits_match(&got, &want, false));
    ASSERT_TRUE(edit_consistent(&ed));

    want.shiny = false;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    gen3_edit_set_ot_id(&ed, 22222, 0);
    ASSERT_EQ(G(OtId), 22222);
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_TRUE(traits_match(&got, &want, false));
    ASSERT_TRUE(edit_consistent(&ed));

    /* No change in shininess: the PID stays. */
    uint32_t pid = G(Pid);
    ASSERT_TRUE(!gen3_is_shiny((7u << 16) | 7u, pid));
    gen3_edit_set_ot_id(&ed, 7, 7);
    ASSERT_EQ(G(Pid), pid);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_ball_item_pokerus) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    ASSERT_EQ(GEN3_EDIT_BALL_MAX, 12);
    ASSERT_STR_EQ(gen3_item_name(GEN3_EDIT_BALL_MAX), "PREMIER BALL");
    gen3_edit_set_ball(&ed, GEN3_EDIT_BALL_MAX);
    ASSERT_EQ(G(Ball), GEN3_EDIT_BALL_MAX);
    gen3_edit_set_ball(&ed, 0);
    gen3_edit_set_ball(&ed, GEN3_EDIT_BALL_MAX + 1);
    ASSERT_EQ(G(Ball), GEN3_EDIT_BALL_MAX);
    gen3_edit_set_item(&ed, 200);
    ASSERT_EQ(G(HeldItem), 200);
    /* Refused: mail (121..132), a key item, an unused id, out of range. */
    static const uint16_t refused[] = {121, 132, 259, 120, GEN3_ITEM_COUNT};
    for(unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        ASSERT_TRUE(!gen3_item_holdable(refused[i]));
        gen3_edit_set_item(&ed, refused[i]);
        ASSERT_EQ(G(HeldItem), 200);
    }
    gen3_edit_set_item(&ed, 0);
    ASSERT_EQ(G(HeldItem), 0);
    gen3_edit_set_pokerus(&ed, 0x41);
    ASSERT_EQ(G(Pokerus), 0x41);
    ASSERT_STR_EQ(gen3_pokerus_status(0x41), "Infected");
    ASSERT_STR_EQ(gen3_pokerus_status(0x40), "Cured");
    ASSERT_STR_EQ(gen3_pokerus_status(0), "Clean");
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_load_keeps_bytes) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    Gen3Pkm in;
    golden_pkm(&in);
    gen3_edit_load(&ed, &in);
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!ed.preset_applied);
    ASSERT_TRUE(edit_consistent(&ed));

    /* Unmodelled bytes are kept verbatim; the party stats are recomputed (Atk 999 -> 12). */
    in.raw[0x13] |= 0xF0; /* flags byte, unused bits */
    gen3_pkm_set(&in, Gen3FieldLanguage, 3);
    gen3_pkm_set(&in, Gen3FieldMarkings, 0x0F);
    gen3_pkm_set(&in, Gen3FieldFiller, 0x1234);
    gen3_pkm_set(&in, Gen3FieldGrowthUnknown, 0xBEEF);
    for(int c = 0; c < 6; c++) gen3_pkm_set(&in, Gen3FieldCoolness + c, c + 1);
    gen3_pkm_set(&in, Gen3FieldRibbons, 0xFFFFFFFF);
    gen3_pkm_set(&in, Gen3FieldMetLocation, 0x10);
    gen3_pkm_set(&in, Gen3FieldMetLevel, 3);
    gen3_pkm_set(&in, Gen3FieldMetGame, 1);
    gen3_pkm_set(&in, Gen3FieldBall, 2);
    gen3_pkm_set(&in, Gen3FieldOtGender, 1);
    gen3_pkm_set(&in, Gen3FieldPokerus, 0x41);
    gen3_pkm_set(&in, Gen3FieldPpBonuses, 0xE4);
    gen3_pkm_set(&in, Gen3FieldFriendship, 123);
    gen3_pkm_set(&in, Gen3FieldHeldItem, 200);
    gen3_pkm_set(&in, Gen3FieldStatus, 0x08);
    gen3_pkm_set(&in, Gen3FieldAtk, 999);
    gen3_pkm_fix_checksum(&in);
    gen3_edit_load(&ed, &in);
    Gen3Pkm want = in;
    uint16_t st[Gen3StatCount];
    gen3_calc_stats(&in, st);
    ASSERT_EQ(st[Gen3StatAtk], 12);
    gen3_pkm_set(&want, Gen3FieldAtk, st[Gen3StatAtk]);
    ASSERT_MEM_EQ(ed.pkm.raw, want.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_load_normalises) {
    Gen3Pkm in;
    golden_pkm(&in);
    /* Golden moves are {1, 43, 0, 0}: add a gap, a duplicate and PP Ups (1, 0, 2, 3). */
    static const uint16_t mv[4] = {1, 0, 1, 43};
    static const uint8_t pp[4] = {35, 0, 20, 30};
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(&in, Gen3FieldMove1 + s, mv[s]);
        gen3_pkm_set(&in, Gen3FieldPp1 + s, pp[s]);
    }
    gen3_pkm_set(&in, Gen3FieldPpBonuses, 0xE1);
    gen3_pkm_set(&in, Gen3FieldAbilityBit, 1); /* Treecko has one ability */
    gen3_pkm_set(&in, Gen3FieldHp, 999);
    gen3_pkm_set(&in, Gen3FieldLevel, 77);
    gen3_pkm_fix_checksum(&in);

    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_load(&ed, &in);
    /* The duplicate is dropped with its PP and PP Ups; Leer carries its PP and 3 PP Ups. */
    Gen3Pkm want;
    golden_pkm(&want);
    gen3_pkm_set(&want, Gen3FieldPpBonuses, 0x0D);
    gen3_pkm_fix_checksum(&want);
    ASSERT_MEM_EQ(ed.pkm.raw, want.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_load_clamps_exp) {
    static const uint32_t exp_in[3] = {0, 0xFFFFFFFF, 2000000};
    static const uint32_t exp_out[3] = {1, 1059860, 1059860};
    static const uint8_t level_out[3] = {1, 100, 100};
    for(int i = 0; i < 3; i++) {
        Gen3Pkm in;
        golden_pkm(&in);
        gen3_pkm_set(&in, Gen3FieldExp, exp_in[i]);
        gen3_pkm_fix_checksum(&in);
        Gen3Editor ed;
        gen3_edit_init(&ed, 7);
        gen3_edit_load(&ed, &in);
        ASSERT_EQ(G(Exp), exp_out[i]);
        ASSERT_EQ(G(Level), level_out[i]);
        /* The current HP (20) is kept, clamped to the new MaxHp (12 at level 1). */
        ASSERT_EQ(G(Hp), G(MaxHp) < 20 ? G(MaxHp) : 20);
        ASSERT_TRUE(edit_consistent_hp(&ed, false));
    }
}

TEST(test_edit_load_keeps_current_hp) {
    Gen3Pkm in;
    golden_pkm(&in);
    gen3_pkm_set(&in, Gen3FieldHp, 7);
    gen3_pkm_set(&in, Gen3FieldStatus, 0x08);
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_load(&ed, &in);
    ASSERT_EQ(G(Hp), 7);
    ASSERT_EQ(G(MaxHp), 20);
    ASSERT_EQ(G(Status), 0x08);
    ASSERT_TRUE(edit_consistent_hp(&ed, false));
    gen3_edit_set_nickname(&ed, "LEAFY"); /* any edit recalculates: Hp = MaxHp */
    ASSERT_EQ(G(Hp), 20);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_two_ability_bit_kept) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 280); /* Ralts: Synchronize / Trace */
    Gen3Pkm in = ed.pkm;
    uint32_t bit = !(gen3_pkm_get(&in, Gen3FieldPid) & 1); /* stored slot != pid & 1 */
    gen3_pkm_set(&in, Gen3FieldAbilityBit, bit);
    gen3_pkm_fix_checksum(&in);
    gen3_edit_load(&ed, &in);
    ASSERT_EQ(G(AbilityBit), bit);
    gen3_edit_set_nickname(&ed, "LEAFY");
    ASSERT_EQ(G(AbilityBit), bit);
    gen3_edit_set_item(&ed, 200);
    ASSERT_EQ(G(AbilityBit), bit);
    gen3_edit_set_level(&ed, 30);
    ASSERT_EQ(G(AbilityBit), bit);
    gen3_edit_set_ball(&ed, 2);
    ASSERT_EQ(G(AbilityBit), bit);
    ASSERT_TRUE(gen3_edit_set_move(&ed, 3, 0));
    ASSERT_EQ(G(AbilityBit), bit);
    /* set_ot_id without a shininess flip keeps the PID and the slot. */
    uint32_t pid = G(Pid);
    ASSERT_TRUE(!gen3_is_shiny((7u << 16) | 7u, pid));
    gen3_edit_set_ot_id(&ed, 7, 7);
    ASSERT_EQ(G(Pid), pid);
    ASSERT_EQ(G(AbilityBit), bit);
    ASSERT_TRUE(edit_consistent(&ed));
    /* With a flip (the old PID is shiny for this OT) it finds a new PID for the same slot. */
    uint16_t tid = (pid >> 16) ^ (pid & 0xFFFF);
    gen3_edit_set_ot_id(&ed, tid, 0);
    ASSERT_TRUE(G(Pid) != pid);
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), G(Pid)));
    ASSERT_EQ(G(AbilityBit), bit);
    ASSERT_TRUE(edit_consistent(&ed));
}

static bool info_is(const Gen3NormaliseInfo* i, bool md, bool dr, bool gp, bool ec, bool af) {
    return i->moves_defaulted == md && i->duplicates_removed == dr && i->gaps_packed == gp &&
           i->exp_clamped == ec && i->ability_fixed == af;
}

static void set_moves(Gen3Pkm* p, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    const uint16_t mv[4] = {a, b, c, d};
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(p, Gen3FieldMove1 + s, mv[s]);
        gen3_pkm_set(p, Gen3FieldPp1 + s, mv[s] ? gen3_move_pp(mv[s]) : 0);
    }
}

TEST(test_normalise_flags) {
    Gen3Pkm p;
    Gen3NormaliseInfo ni;
    golden_pkm(&p);
    gen3_pkm_set(&p, Gen3FieldHp, 999); /* party stats are fixed silently */
    gen3_pkm_set(&p, Gen3FieldLevel, 77);
    memset(&ni, 0xFF, sizeof(ni));
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, false, false, false, false));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_pkm(&p);
    set_moves(&p, 1, 0, 43, 0);
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, false, true, false, false));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_pkm(&p);
    set_moves(&p, 1, 43, 1, 0); /* the dropped duplicate leaves no gap before a move */
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, true, false, false, false));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_pkm(&p);
    set_moves(&p, 1, 1, 43, 0); /* gaps_packed reports the input, which had no gap */
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, true, false, false, false));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_pkm(&p);
    set_moves(&p, 0, 0, 0, 0);
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, true, false, false, false, false));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_pkm(&p);
    gen3_pkm_set(&p, Gen3FieldExp, 0);
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, false, false, true, false));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldExp), 1);

    golden_pkm(&p);
    gen3_pkm_set(&p, Gen3FieldAbilityBit, 1); /* Treecko has one ability */
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, false, false, false, true));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    /* A two-ability species keeps its stored slot, even when it isn't pid & 1. */
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 280);
    p = ed.pkm;
    uint32_t bit = !(gen3_pkm_get(&p, Gen3FieldPid) & 1);
    gen3_pkm_set(&p, Gen3FieldAbilityBit, bit);
    ASSERT_TRUE(gen3_normalise(&p, NULL));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldAbilityBit), bit);
    ASSERT_EQ(gen3_pkm_checksum(&p), gen3_pkm_get(&p, Gen3FieldChecksum));

    /* Unknown species (0, a "?" placeholder 260, past the table 412): false, untouched. */
    static const uint16_t bad[3] = {0, 260, 412};
    for(int i = 0; i < 3; i++) {
        golden_pkm(&p);
        gen3_pkm_set(&p, Gen3FieldSpecies, bad[i]);
        set_moves(&p, 1, 0, 1, 43);
        Gen3Pkm before = p;
        memset(&ni, 0xFF, sizeof(ni));
        ASSERT_TRUE(!gen3_normalise(&p, &ni));
        ASSERT_TRUE(info_is(&ni, false, false, false, false, false));
        ASSERT_MEM_EQ(p.raw, before.raw, GEN3_PARTY_SIZE);
    }
}

TEST(test_edit_load_no_moves) {
    Gen3Pkm in;
    golden_pkm(&in);
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(&in, Gen3FieldMove1 + s, 0);
        gen3_pkm_set(&in, Gen3FieldPp1 + s, 0);
    }
    gen3_pkm_set(&in, Gen3FieldPpBonuses, 0xFF);
    gen3_pkm_fix_checksum(&in);
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_load(&ed, &in);
    /* Treecko L5's default moveset with full PP is the golden {1, 43} / {35, 30}. */
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

int main(void) {
    RUN(test_edit_init_defaults);
    RUN(test_edit_species_resets_keeps_ot);
    RUN(test_edit_species_stores_internal);
    RUN(test_edit_species_same_is_noop);
    RUN(test_edit_species_skips_shiny_pid);
    RUN(test_edit_species_fateful);
    RUN(test_edit_all_species);
    RUN(test_edit_level);
    RUN(test_edit_level_clamped);
    RUN(test_edit_presets);
    RUN(test_edit_level_ev_follows_level);
    RUN(test_edit_loaded_evs_kept);
    RUN(test_edit_nickname_default);
    RUN(test_edit_set_move_pp);
    RUN(test_edit_set_move_packs);
    RUN(test_edit_set_move_rejects);
    RUN(test_edit_default_moves);
    RUN(test_edit_traits_matrix);
    RUN(test_edit_traits_static_gender);
    RUN(test_edit_ability_bit_single_ability);
    RUN(test_edit_unown_letters);
    RUN(test_edit_traits_preserve_other_bytes);
    RUN(test_edit_traits_same_keeps_pid);
    RUN(test_edit_ot_id_keeps_shiny);
    RUN(test_edit_ball_item_pokerus);
    RUN(test_edit_load_keeps_bytes);
    RUN(test_edit_load_normalises);
    RUN(test_edit_load_clamps_exp);
    RUN(test_edit_load_keeps_current_hp);
    RUN(test_edit_load_no_moves);
    RUN(test_edit_two_ability_bit_kept);
    RUN(test_normalise_flags);
    TEST_EXIT();
}
