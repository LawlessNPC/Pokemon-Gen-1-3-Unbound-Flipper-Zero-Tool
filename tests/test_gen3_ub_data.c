#include "test.h"
#include "ub_bulk_file.h"
#include <stdbool.h>
#include <string.h>
#include <src/gen3/gen3_ub_data.h>

/* Unbound's species names, growth rates and Dex numbers (gen3_ub_data.h) from the committed
 * files/unbound_bulk.bin, which tools/unbound_tables.py makes from PUSE's data */

static bool fail_read(void* ctx, uint32_t offset, void* buf, uint32_t len) {
    (void)ctx;
    (void)offset;
    (void)buf;
    (void)len;
    return false;
}

TEST(test_ub_data_header) {
    ASSERT_TRUE(ub_bulk_file_open());
    ASSERT_TRUE(gen3_ub_data_ok());
}

TEST(test_ub_names) {
    ASSERT_TRUE(ub_bulk_file_open());
    char name[11];
    ASSERT_TRUE(gen3_ub_species_name(1, name));
    ASSERT_STR_EQ(name, "BULBASAUR");
    ASSERT_TRUE(gen3_ub_species_name(1267, name));
    ASSERT_STR_EQ(name, "GENGAR");
    ASSERT_TRUE(!gen3_ub_species_name(1268, name));
    ASSERT_STR_EQ(name, "#1268");
    ASSERT_TRUE(!gen3_ub_species_name(0, name));
    ASSERT_STR_EQ(name, "#0");
}

TEST(test_ub_growth_ndex) {
    ASSERT_TRUE(ub_bulk_file_open());
    ASSERT_EQ(gen3_ub_growth(1, NULL), 3);
    ASSERT_EQ(gen3_ub_national(1), 1);
    ASSERT_EQ(gen3_ub_national(1000), 783);
    ASSERT_EQ(gen3_ub_growth(1268, NULL), -1);
    ASSERT_EQ(gen3_ub_national(1268), 0);
}

TEST(test_ub_moves) {
    ASSERT_TRUE(ub_bulk_file_open());
    char name[13];
    ASSERT_TRUE(gen3_ub_move_name(1, name));
    ASSERT_STR_EQ(name, "POUND");
    ASSERT_EQ(gen3_ub_move_pp(1), 35);
    ASSERT_TRUE(gen3_ub_move_name(2, name));
    ASSERT_STR_EQ(name, "KARATE CHOP");
    ASSERT_EQ(gen3_ub_move_pp(2), 25);
    ASSERT_TRUE(gen3_ub_move_name(922, name));
    ASSERT_TRUE(!gen3_ub_move_name(0, name));
    ASSERT_TRUE(!gen3_ub_move_name(923, name));
    ASSERT_EQ(gen3_ub_move_pp(0), 0);
    ASSERT_EQ(gen3_ub_move_pp(923), 0);
}

TEST(test_ub_items) {
    ASSERT_TRUE(ub_bulk_file_open());
    char name[18];
    ASSERT_TRUE(gen3_ub_item_name(4, name));
    ASSERT_TRUE(name[0] != 0);
    ASSERT_TRUE(gen3_ub_item_name(1, name));
    ASSERT_STR_EQ(name, "MASTER BALL");
    ASSERT_TRUE(!gen3_ub_item_name(0, name));
    ASSERT_TRUE(!gen3_ub_item_name(729, name));
    ASSERT_TRUE(!gen3_ub_item_holdable(87)); /* a key-pocket item */
    ASSERT_TRUE(gen3_ub_item_holdable(1)); /* a ball */
    ASSERT_TRUE(gen3_ub_item_holdable(118)); /* a berry */
    ASSERT_TRUE(!gen3_ub_item_holdable(289)); /* a TM */
    ASSERT_TRUE(!gen3_ub_item_holdable(0));
    ASSERT_TRUE(!gen3_ub_item_holdable(729));
}

TEST(test_ub_abilities) {
    ASSERT_TRUE(ub_bulk_file_open());
    uint8_t ab[3];
    ASSERT_TRUE(gen3_ub_species_abilities(1, ab));
    ASSERT_EQ(ab[0], 65);
    ASSERT_EQ(ab[1], 0);
    ASSERT_EQ(ab[2], 34);
    ASSERT_TRUE(!gen3_ub_species_abilities(1268, ab));
    char name[16];
    ASSERT_TRUE(gen3_ub_ability_name(65, name));
    ASSERT_STR_EQ(name, "OVERGROW");
    ASSERT_TRUE(!gen3_ub_ability_name(0, name));
    ASSERT_TRUE(!gen3_ub_ability_name(255, name));
}

TEST(test_ub_base) {
    ASSERT_TRUE(ub_bulk_file_open());
    uint8_t b[6];
    ASSERT_TRUE(gen3_ub_base_stats(1, b));
    const uint8_t want[6] = {45, 49, 49, 45, 65, 65};
    for(int k = 0; k < 6; k++) ASSERT_EQ(b[k], want[k]);
    ASSERT_TRUE(!gen3_ub_base_stats(1268, b));
    ASSERT_EQ(gen3_ub_gender_threshold(1), 31);
    ASSERT_EQ(gen3_ub_gender_threshold(1268), -1);
}

TEST(test_ub_levelup) {
    ASSERT_TRUE(ub_bulk_file_open());
    uint16_t m[40];
    ASSERT_EQ(gen3_ub_levelup(1, m, 40), 16);
    ASSERT_EQ(m[0], 22);
    ASSERT_EQ(m[1], 33);
    ASSERT_EQ(m[2], 36);
    ASSERT_EQ(m[3], 38);
    ASSERT_EQ(m[15], 475);
    ASSERT_EQ(gen3_ub_levelup(1, m, 3), 3);
    ASSERT_EQ(gen3_ub_levelup(252, m, 40), 0);
    ASSERT_EQ(gen3_ub_levelup(0, m, 40), 0);
    ASSERT_EQ(gen3_ub_levelup(1268, m, 40), 0);
}

static void check_sorted(Gen3UbSort s, uint16_t want_count) {
    ASSERT_EQ(gen3_ub_sorted_count(s), want_count);
    char prev[18] = "", cur[18];
    for(uint16_t k = 0; k < want_count; k++) {
        const uint16_t id = gen3_ub_sorted(s, k);
        ASSERT_TRUE(id != 0);
        const bool ok = s == Gen3UbSortSpecies ? gen3_ub_species_name(id, cur) :
                        s == Gen3UbSortMoves   ? gen3_ub_move_name(id, cur) :
                                                 gen3_ub_item_name(id, cur);
        ASSERT_TRUE(ok);
        ASSERT_TRUE(strcmp(cur, prev) >= 0);
        strcpy(prev, cur);
    }
    ASSERT_EQ(gen3_ub_sorted(s, want_count), 0);
}

TEST(test_ub_sorted) {
    ASSERT_TRUE(ub_bulk_file_open());
    check_sorted(Gen3UbSortSpecies, 1267);
    check_sorted(Gen3UbSortMoves, 922);
    /* The held-item pick list is the ball, berry and main pockets that have a name */
    const uint16_t n = gen3_ub_sorted_count(Gen3UbSortItems);
    ASSERT_TRUE(n > 100 && n < 729);
    for(uint16_t k = 0; k < n; k++) ASSERT_TRUE(gen3_ub_item_holdable(gen3_ub_sorted(Gen3UbSortItems, k)));
    ASSERT_EQ(gen3_ub_sorted_count((Gen3UbSort)9), 0);
}

TEST(test_ub_read_failure) {
    ASSERT_TRUE(ub_bulk_file_open());
    ub_bulk_file_failing = true;
    char name[11];
    ASSERT_TRUE(!gen3_ub_species_name(1, name));
    ASSERT_STR_EQ(name, "#1");
    ASSERT_EQ(gen3_ub_growth(1, NULL), -1);
    char big[18];
    uint8_t six[6];
    uint16_t mv[4];
    ASSERT_TRUE(!gen3_ub_move_name(1, big));
    ASSERT_STR_EQ(big, "#1");
    ASSERT_EQ(gen3_ub_move_pp(1), 0);
    ASSERT_TRUE(!gen3_ub_item_holdable(1));
    ASSERT_TRUE(!gen3_ub_base_stats(1, six));
    ASSERT_EQ(gen3_ub_gender_threshold(1), -1);
    ASSERT_EQ(gen3_ub_levelup(1, mv, 4), 0);
    ASSERT_EQ(gen3_ub_sorted(Gen3UbSortSpecies, 0), 0);
    ASSERT_TRUE(!gen3_ub_data_open(fail_read, NULL));
    ASSERT_TRUE(!gen3_ub_data_ok());
    gen3_ub_data_close();
    ASSERT_TRUE(!gen3_ub_species_name(1, name));
}

int main(void) {
    RUN(test_ub_data_header);
    RUN(test_ub_names);
    RUN(test_ub_growth_ndex);
    RUN(test_ub_moves);
    RUN(test_ub_items);
    RUN(test_ub_abilities);
    RUN(test_ub_base);
    RUN(test_ub_levelup);
    RUN(test_ub_sorted);
    RUN(test_ub_read_failure);
    TEST_EXIT();
}
