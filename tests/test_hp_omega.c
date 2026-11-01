#include "test.h"
#include "fake_fat.h"
#include "fake_gba.h"
#include <stdbool.h>
#include <stdlib.h>
#include <hp_omega.h>

/* The helper's read-only FAT32 layer over an EZ-Flash Omega's microSD (hp_omega.h), on the fake
 * GBA with a fake card (fake_fat.h). */

#define SAV_LEN 131072u

static FakeGba gba;
static FakeFat card;
static HpOmega om;
static uint8_t data[SAV_LEN];
static uint16_t out[8 * 256];

static void pattern(uint8_t* p, uint32_t n, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 1u;
    for(uint32_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (uint8_t)(x >> 16);
    }
}

/* out's bytes, little-endian halfwords, equal p for n bytes */
static bool out_is(const uint8_t* p, uint32_t n) {
    for(uint32_t i = 0; i < n; i++) {
        if((uint8_t)(out[i / 2u] >> ((i & 1u) * 8u)) != p[i]) return false;
    }
    return true;
}

/* A card with its partition at part, spc sectors a cluster, and SAVER/Pokemon - Unbound.sav */
static uint32_t make_card(uint32_t part, uint8_t spc, uint32_t gap_every) {
    fake_gba_init(&gba);
    fake_fat_init(&card, part, spc, 65536u);
    fake_gba_omega(&gba, &card);
    pattern(data, SAV_LEN, 7);
    const uint32_t saver = fake_fat_mkdir(&card, fake_fat_root(&card), "SAVER");
    fake_fat_add_file(&card, saver, "Pokemon - Unbound.sav", data, SAV_LEN, gap_every);
    hp_omega_init(&om);
    return saver;
}

TEST(test_omega_mount_mbr) {
    make_card(2048, 8, 0);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    ASSERT_TRUE(om.mounted);
    ASSERT_EQ(om.part, 2048);
    ASSERT_EQ(om.spc, 8);
    ASSERT_EQ(om.root, fake_fat_root(&card));
    fake_fat_free(&card);
}

TEST(test_omega_mount_superfloppy) {
    make_card(0, 8, 0);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    ASSERT_EQ(om.part, 0);
    ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER/Pokemon - Unbound.sav"), HpOmegaOk);
    fake_fat_free(&card);
}

TEST(test_omega_mount_no_card) {
    /* A blank card: no MBR or boot sector */
    fake_gba_init(&gba);
    fake_fat_init_blank(&card, 4096u);
    fake_gba_omega(&gba, &card);
    hp_omega_init(&om);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaNoFs);
    ASSERT_TRUE(!om.mounted);
    fake_fat_free(&card);
    /* The card never answers */
    make_card(2048, 8, 0);
    fake_gba_omega_fail_after(&gba, 0);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaSdError);
    fake_fat_free(&card);
    /* No Omega at all */
    fake_gba_init(&gba);
    hp_omega_init(&om);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaSdError);
}

TEST(test_omega_mount_exfat) {
    make_card(2048, 8, 0);
    fake_fat_poke(&card, 2048u, 3u, "EXFAT   ", 8u);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaNotFat32);
    fake_fat_free(&card);
}

TEST(test_omega_open_lfn) {
    make_card(2048, 8, 0);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER/Pokemon - Unbound.sav"), HpOmegaOk);
    ASSERT_TRUE(om.files[0].used);
    ASSERT_EQ(om.files[0].size, SAV_LEN);
    ASSERT_TRUE(om.files[0].first >= 2u);
    ASSERT_EQ(hp_omega_open(&om, 1, "/saver/POKEMON - UNBOUND.SAV"), HpOmegaOk);
    ASSERT_EQ(om.files[1].first, om.files[0].first);
    fake_fat_free(&card);
}

TEST(test_omega_open_missing) {
    make_card(2048, 8, 0);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER/Pokemon - Ruby.sav"), HpOmegaNotFound);
    ASSERT_TRUE(!om.files[0].used);
    ASSERT_EQ(hp_omega_open(&om, 0, "/NOPE/Pokemon - Unbound.sav"), HpOmegaNotFound);
    ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER"), HpOmegaIsDir);
    ASSERT_EQ(hp_omega_open(&om, 0, "SAVER/Pokemon - Unbound.sav"), HpOmegaBadArg);
    ASSERT_EQ(hp_omega_open(&om, 3, "/SAVER/Pokemon - Unbound.sav"), HpOmegaBadArg);
    fake_fat_free(&card);
}

TEST(test_omega_open_before_mount) {
    make_card(2048, 8, 0);
    ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER/Pokemon - Unbound.sav"), HpOmegaBadArg);
    ASSERT_EQ(hp_omega_read(&om, 0, 0, 1, out), HpOmegaBadArg);
    fake_fat_free(&card);
}

TEST(test_omega_lfn_split) {
    /* One sector a cluster: SAVER spans many clusters. 40 files with long names first, a deleted
     * entry, then the target, whose LFN run crosses a sector (and cluster) boundary somewhere. */
    fake_gba_init(&gba);
    fake_fat_init(&card, 2048, 1, 65536u);
    fake_gba_omega(&gba, &card);
    const uint32_t saver = fake_fat_mkdir(&card, fake_fat_root(&card), "SAVER");
    uint8_t small[600];
    char name[64];
    for(uint32_t k = 0; k < 40u; k++) {
        snprintf(name, sizeof(name), "Some Game Number %02u (USA) (En,Fr,De).sav", (unsigned)k);
        pattern(small, sizeof(small), k);
        fake_fat_add_file(&card, saver, name, small, sizeof(small), 0);
    }
    fake_fat_add_deleted(&card, saver, "Pokemon - Unbound.sav");
    pattern(data, SAV_LEN, 9);
    fake_fat_add_file(&card, saver, "Pokemon - Unbound.sav", data, SAV_LEN, 0);
    hp_omega_init(&om);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER/Pokemon - Unbound.sav"), HpOmegaOk);
    ASSERT_EQ(om.files[0].size, SAV_LEN);
    ASSERT_EQ(hp_omega_read(&om, 0, 0, 8, out), HpOmegaOk);
    ASSERT_TRUE(out_is(data, 4096));
    ASSERT_EQ(hp_omega_open(&om, 1, "/SAVER/Some Game Number 39 (USA) (En,Fr,De).sav"), HpOmegaOk);
    ASSERT_EQ(om.files[1].size, sizeof(small));
    /* Entries 0 and 1 are "." and "..", and each file takes 4 LFN entries and its 8.3 one: file
     * 2's run is entries 12..16, across the first sector's end (16 entries a sector) */
    ASSERT_EQ(hp_omega_open(&om, 2, "/SAVER/Some Game Number 02 (USA) (En,Fr,De).sav"), HpOmegaOk);
    ASSERT_EQ(hp_omega_read(&om, 2, 0, 2, out), HpOmegaOk);
    pattern(small, sizeof(small), 2);
    ASSERT_TRUE(out_is(small, sizeof(small)));
    fake_fat_free(&card);
}

TEST(test_omega_read_fragmented) {
    make_card(2048, 8, 3);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER/Pokemon - Unbound.sav"), HpOmegaOk);
    static const uint32_t offs[] = {0u, 4096u * 7u, 4096u - 512u, 4096u * 3u, 12288u - 1024u, SAV_LEN - 4096u};
    for(uint32_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        ASSERT_EQ(hp_omega_read(&om, 0, offs[i], 8, out), HpOmegaOk);
        ASSERT_TRUE(out_is(data + offs[i], 4096));
    }
    /* Backwards, then the last block alone */
    ASSERT_EQ(hp_omega_read(&om, 0, 512u, 1, out), HpOmegaOk);
    ASSERT_TRUE(out_is(data + 512u, 512));
    ASSERT_EQ(hp_omega_read(&om, 0, SAV_LEN - 512u, 1, out), HpOmegaOk);
    ASSERT_TRUE(out_is(data + SAV_LEN - 512u, 512));
    fake_fat_free(&card);
}

TEST(test_omega_read_bounds) {
    make_card(2048, 8, 0);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER/Pokemon - Unbound.sav"), HpOmegaOk);
    ASSERT_EQ(hp_omega_read(&om, 0, SAV_LEN, 1, out), HpOmegaBadArg);
    ASSERT_EQ(hp_omega_read(&om, 0, SAV_LEN - 512u, 2, out), HpOmegaBadArg);
    ASSERT_EQ(hp_omega_read(&om, 0, 100u, 1, out), HpOmegaBadArg);
    ASSERT_EQ(hp_omega_read(&om, 0, 0, 0, out), HpOmegaBadArg);
    ASSERT_EQ(hp_omega_read(&om, 0, 0, 9, out), HpOmegaBadArg);
    ASSERT_EQ(hp_omega_read(&om, 1, 0, 1, out), HpOmegaBadArg); /* slot 1 isn't open */
    /* A file whose size isn't a multiple of 512: its last block reads whole */
    const uint32_t saver = fake_fat_mkdir(&card, fake_fat_root(&card), "TXT");
    fake_fat_add_file(&card, saver, "Recently play.txt", data, 548u, 0);
    ASSERT_EQ(hp_omega_open(&om, 1, "/TXT/Recently play.txt"), HpOmegaOk);
    ASSERT_EQ(hp_omega_read(&om, 1, 512u, 1, out), HpOmegaOk);
    ASSERT_TRUE(out_is(data + 512u, 36u));
    ASSERT_EQ(hp_omega_read(&om, 1, 1024u, 1, out), HpOmegaBadArg);
    fake_fat_free(&card);
}

int main(void) {
    RUN(test_omega_mount_mbr);
    RUN(test_omega_mount_superfloppy);
    RUN(test_omega_mount_no_card);
    RUN(test_omega_mount_exfat);
    RUN(test_omega_open_lfn);
    RUN(test_omega_open_missing);
    RUN(test_omega_open_before_mount);
    RUN(test_omega_lfn_split);
    RUN(test_omega_read_fragmented);
    RUN(test_omega_read_bounds);
    TEST_EXIT();
}
