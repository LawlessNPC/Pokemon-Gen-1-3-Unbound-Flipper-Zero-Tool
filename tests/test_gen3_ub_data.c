#include "test.h"
#include "ub_bulk_file.h"
#include <stdbool.h>
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

TEST(test_ub_read_failure) {
    ASSERT_TRUE(ub_bulk_file_open());
    ub_bulk_file_failing = true;
    char name[11];
    ASSERT_TRUE(!gen3_ub_species_name(1, name));
    ASSERT_STR_EQ(name, "#1");
    ASSERT_EQ(gen3_ub_growth(1, NULL), -1);
    ASSERT_TRUE(!gen3_ub_data_open(fail_read, NULL));
    ASSERT_TRUE(!gen3_ub_data_ok());
    gen3_ub_data_close();
    ASSERT_TRUE(!gen3_ub_species_name(1, name));
}

int main(void) {
    RUN(test_ub_data_header);
    RUN(test_ub_names);
    RUN(test_ub_growth_ndex);
    RUN(test_ub_read_failure);
    TEST_EXIT();
}
