#include "test.h"
#include <src/gen3/gen3_letters.h>
#include <src/gen3/gen3_data.h>

/* Ids 10..15, already sorted by name */
static const char* const names[16] =
    {"", "", "", "", "", "", "", "", "", "", "ACID", "AIR", "BIND", "BITE", "CUT", "ZAP"};
static uint16_t at(size_t i) {
    return (uint16_t)(10 + i);
}
static const char* name_of(uint16_t id) {
    return names[id];
}
static const Gen3NameList list = {6, at, name_of};

TEST(test_letters_distinct_in_order) {
    char out[8];
    ASSERT_EQ(gen3_letters(&list, out, sizeof(out)), 4);
    ASSERT_TRUE(memcmp(out, "ABCZ", 4) == 0);
}

TEST(test_letters_truncates) {
    char out[2];
    ASSERT_EQ(gen3_letters(&list, out, sizeof(out)), 2);
    ASSERT_TRUE(memcmp(out, "AB", 2) == 0);
}

TEST(test_letter_ids) {
    uint16_t out[8];
    ASSERT_EQ(gen3_letter_ids(&list, 'B', out, 8), 2);
    ASSERT_EQ(out[0], 12);
    ASSERT_EQ(out[1], 13);
    ASSERT_EQ(gen3_letter_ids(&list, 'Q', out, 8), 0);
    ASSERT_EQ(gen3_letter_ids(&list, 'A', out, 1), 1);
}

TEST(test_real_moves_and_items) {
    char out[64];
    uint16_t ids[400];
    size_t n;
    size_t total = 0;
    const Gen3NameList moves = {GEN3_MOVE_COUNT - 1, gen3_move_by_name, gen3_move_name};
    const Gen3NameList items = {GEN3_HOLDABLE_COUNT, gen3_holdable_by_name, gen3_item_name};

    n = gen3_letters(&moves, out, sizeof(out));
    ASSERT_TRUE(n > 10 && n < sizeof(out));
    ASSERT_TRUE(n <= GEN3_LETTERS_MAX);
    for(size_t i = 0; i < n; i++) {
        size_t k = gen3_letter_ids(&moves, out[i], ids, 400);
        ASSERT_TRUE(k <= GEN3_LETTER_IDS_MAX);
        total += k;
    }
    ASSERT_EQ(total, GEN3_MOVE_COUNT - 1);

    n = gen3_letters(&items, out, sizeof(out));
    ASSERT_TRUE(n > 10 && n < sizeof(out));
    total = 0;
    ASSERT_TRUE(n <= GEN3_LETTERS_MAX);
    for(size_t i = 0; i < n; i++) {
        size_t k = gen3_letter_ids(&items, out[i], ids, 400);
        ASSERT_TRUE(k <= GEN3_LETTER_IDS_MAX);
        total += k;
    }
    ASSERT_EQ(total, GEN3_HOLDABLE_COUNT);
}

int main(void) {
    RUN(test_letters_distinct_in_order);
    RUN(test_letters_truncates);
    RUN(test_letter_ids);
    RUN(test_real_moves_and_items);
    TEST_EXIT();
}
