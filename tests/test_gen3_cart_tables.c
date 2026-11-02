#include "test.h"

/* The Cartridge app's reader for files/gen3_cart_tables.bin, built here against the committed
 * file. The reader is gen3_data.c itself compiled in cart mode (the Makefile leaves the normal
 * gen3_data.c out of this suite), and gen3_data_gen.c, compiled normally, supplies the in-code
 * tables every entry is compared with. */
#define POKEMON_CART_APP
#include "../src/gen3/gen3_data.c"
#undef POKEMON_CART_APP

#define TABLES_PATH "../files/gen3_cart_tables.bin"
#define TABLES_SIZE (16 + 8030)

static uint8_t file_bytes[TABLES_SIZE + 8];
static uint32_t file_len;

typedef struct {
    const uint8_t* data;
    uint32_t len;
} Mem;

static bool mem_read(void* ctx, uint32_t offset, void* buf, uint32_t len) {
    const Mem* m = ctx;
    if(offset > m->len || len > m->len - offset) return false;
    memcpy(buf, m->data + offset, len);
    return true;
}

static bool load_file(void) {
    FILE* f = fopen(TABLES_PATH, "rb");
    if(!f) {
        printf("cannot open %s\n", TABLES_PATH);
        return false;
    }
    file_len = (uint32_t)fread(file_bytes, 1, sizeof(file_bytes), f);
    fclose(f);
    return file_len == TABLES_SIZE;
}

static bool open_mem(const uint8_t* data, uint32_t len) {
    Mem m = {data, len};
    gen3_data_tables_free();
    return gen3_data_tables_open(mem_read, &m);
}

TEST(test_cart_tables_not_loaded) {
    gen3_data_tables_free();
    ASSERT_TRUE(!gen3_data_tables_ready());
    ASSERT_TRUE(gen3_species(1) == NULL);
    ASSERT_EQ(gen3_national_from_internal(277), 0);
    ASSERT_EQ(gen3_internal_from_national(1), 0);
    ASSERT_TRUE(strcmp(gen3_ability_name(1), "???") == 0);
    ASSERT_TRUE(!gen3_species_two_abilities(1));
    ASSERT_TRUE(gen3_species_gender_fixed(1));
}

TEST(test_cart_tables_file_size) {
    ASSERT_TRUE(load_file());
}

TEST(test_cart_tables_match_code) {
    ASSERT_TRUE(load_file());
    ASSERT_TRUE(open_mem(file_bytes, file_len));
    ASSERT_TRUE(gen3_data_tables_ready());
    for(uint16_t n = 0; n <= GEN3_NATIONAL_COUNT + 1; n++) {
        const Gen3SpeciesInfo* got = gen3_species(n);
        if(n < 1 || n > GEN3_NATIONAL_COUNT) {
            ASSERT_TRUE(got == NULL);
            continue;
        }
        ASSERT_TRUE(got != NULL);
        ASSERT_TRUE(memcmp(got, &gen3_species_info_tab[n], sizeof(Gen3SpeciesInfo)) == 0);
        ASSERT_EQ(gen3_internal_from_national(n), gen3_species_info_tab[n].internal);
        ASSERT_EQ(gen3_species_two_abilities(n), gen3_species_info_tab[n].ability[1] != 0);
    }
    for(uint32_t i = 0; i < GEN3_INTERNAL_COUNT + 3; i++) {
        uint16_t want = i < GEN3_INTERNAL_COUNT ? gen3_internal_to_national_tab[i] : 0;
        ASSERT_EQ(gen3_national_from_internal((uint16_t)i), want);
    }
    for(uint32_t a = 0; a < GEN3_ABILITY_COUNT + 3; a++) {
        const char* want = a < GEN3_ABILITY_COUNT ? gen3_ability_name_tab[a] : "???";
        ASSERT_TRUE(strcmp(gen3_ability_name((uint8_t)a), want) == 0);
    }
    /* The pointers stay put for as long as the tables are loaded */
    ASSERT_TRUE(gen3_species(25) == gen3_species(25));
    gen3_data_tables_free();
    ASSERT_TRUE(!gen3_data_tables_ready());
    ASSERT_TRUE(gen3_species(25) == NULL);
}

TEST(test_cart_tables_wrong_crc) {
    ASSERT_TRUE(load_file());
    uint8_t bad[TABLES_SIZE];
    memcpy(bad, file_bytes, TABLES_SIZE);
    bad[16 + 100] ^= 1; /* payload */
    ASSERT_TRUE(!open_mem(bad, TABLES_SIZE));
    ASSERT_TRUE(!gen3_data_tables_ready());
    ASSERT_TRUE(gen3_species(1) == NULL);
    memcpy(bad, file_bytes, TABLES_SIZE);
    bad[12] ^= 1; /* the stored CRC */
    ASSERT_TRUE(!open_mem(bad, TABLES_SIZE));
}

TEST(test_cart_tables_short_or_long) {
    ASSERT_TRUE(load_file());
    ASSERT_TRUE(!open_mem(file_bytes, TABLES_SIZE - 1));
    ASSERT_TRUE(!open_mem(file_bytes, 15));
    ASSERT_TRUE(!open_mem(file_bytes, 0));
    ASSERT_TRUE(!open_mem(file_bytes, TABLES_SIZE + 1)); /* trailing byte */
    ASSERT_TRUE(!gen3_data_tables_ready());
}

TEST(test_cart_tables_wrong_version) {
    ASSERT_TRUE(load_file());
    uint8_t bad[TABLES_SIZE];
    memcpy(bad, file_bytes, TABLES_SIZE);
    bad[4] = 2;
    ASSERT_TRUE(!open_mem(bad, TABLES_SIZE));
    memcpy(bad, file_bytes, TABLES_SIZE);
    bad[0] = 'X'; /* magic */
    ASSERT_TRUE(!open_mem(bad, TABLES_SIZE));
    memcpy(bad, file_bytes, TABLES_SIZE);
    bad[8] ^= 1; /* a count */
    ASSERT_TRUE(!open_mem(bad, TABLES_SIZE));
    ASSERT_TRUE(!gen3_data_tables_ready());
}

TEST(test_cart_tables_failing_source) {
    ASSERT_TRUE(!gen3_data_tables_open(NULL, NULL));
    gen3_data_tables_free();
}

int main(void) {
    RUN(test_cart_tables_not_loaded);
    RUN(test_cart_tables_file_size);
    RUN(test_cart_tables_match_code);
    RUN(test_cart_tables_wrong_crc);
    RUN(test_cart_tables_short_or_long);
    RUN(test_cart_tables_wrong_version);
    RUN(test_cart_tables_failing_source);
    gen3_data_tables_free();
    TEST_EXIT();
}
