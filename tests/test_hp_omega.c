#include "test.h"
#include "fake_fat.h"
#include "fake_gba.h"
#include "helper_cmd.h"
#include <stdbool.h>
#include <stdlib.h>
#include <hp_core.h>
#include <hp_omega.h>
#include <hp_sha256.h>

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

/* A boot sector whose FAT size, doubled, wraps 32 bits, or whose FAT is too small for the clusters
 * the volume claims, doesn't mount */
TEST(test_omega_mount_bad_fat_size) {
    static const uint32_t bad[] = {0x80000100u, 0xFFFFFFFFu, 0x10000000u, 1u, 10u};
    for(uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        make_card(2048, 8, 0);
        fake_fat_poke(&card, 2048u, 36u, &bad[i], 4u);
        ASSERT_EQ(hp_omega_mount(&om), HpOmegaNotFat32);
        ASSERT_TRUE(!om.mounted);
        fake_fat_free(&card);
    }
    /* A root cluster outside the volume */
    make_card(2048, 8, 0);
    const uint32_t far = 0x00FFFFFFu;
    fake_fat_poke(&card, 2048u, 44u, &far, 4u);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaNotFat32);
    fake_fat_free(&card);
    /* The honest card still mounts, with its FAT exactly as large as it needs */
    make_card(2048, 8, 0);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
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

TEST(test_omega_chain_out_of_range) {
    make_card(2048, 8, 0);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    /* total sectors 65536 - 2048, less the reserved sectors and both FATs */
    ASSERT_EQ(om.clusters, (65536u - 2048u - (om.data - om.part)) / 8u);
    ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER/Pokemon - Unbound.sav"), HpOmegaOk);
    const uint32_t first = om.files[0].first;
    const uint32_t bad[] = {om.clusters + 5u, 0x0FFFFFF7u, 1u, om.clusters + 2u};
    for(uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fake_fat_set_next(&card, first, bad[i]);
        om.fat_cached = UINT32_MAX; /* the FAT block read before is stale */
        ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER/Pokemon - Unbound.sav"), HpOmegaOk);
        const uint32_t reads = gba.omega_reads;
        ASSERT_EQ(hp_omega_read(&om, 0, 4096u, 1, out), HpOmegaSdError);
        ASSERT_TRUE(gba.omega_reads - reads <= 1u); /* the FAT block at most, never the data */
    }
    /* The last cluster of the volume is fine */
    fake_fat_set_next(&card, first, om.clusters + 1u);
    om.fat_cached = UINT32_MAX;
    ASSERT_EQ(hp_omega_open(&om, 0, "/SAVER/Pokemon - Unbound.sav"), HpOmegaOk);
    ASSERT_EQ(hp_omega_read(&om, 0, 4096u, 1, out), HpOmegaOk);
    fake_fat_free(&card);
}

TEST(test_omega_lfn_missing_seq) {
    make_card(2048, 8, 0);
    const uint32_t dir = fake_fat_mkdir(&card, fake_fat_root(&card), "D");
    /* 30 characters: 3 entries, and the one with sequence 2 is gone */
    fake_fat_lfn_entry(&card, dir, "Thirty characters in this name", 2);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    ASSERT_EQ(hp_omega_open(&om, 0, "/D/Thirty characters in this name"), HpOmegaNotFound);
    ASSERT_EQ(hp_omega_open(&om, 0, "/D/LFNTEST.DAT"), HpOmegaOk);
    fake_fat_free(&card);
}

TEST(test_omega_lfn_bad_sum) {
    make_card(2048, 8, 0);
    const uint32_t dir = fake_fat_mkdir(&card, fake_fat_root(&card), "D");
    fake_fat_lfn_entry(&card, dir, "Thirty characters in this name", 0);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    ASSERT_EQ(hp_omega_open(&om, 0, "/D/Thirty characters in this name"), HpOmegaOk);
    /* The first LFN entry is directory entry 2; its checksum byte no longer fits the 8.3 entry */
    const uint8_t wrong = 0x5Au;
    fake_fat_poke(&card, fake_fat_file_lba(&card, dir, 0), 2u * 32u + 13u, &wrong, 1u);
    ASSERT_EQ(hp_omega_open(&om, 0, "/D/Thirty characters in this name"), HpOmegaNotFound);
    ASSERT_EQ(hp_omega_open(&om, 0, "/D/LFNTEST.DAT"), HpOmegaOk);
    fake_fat_free(&card);
}

TEST(test_omega_dir_loop) {
    make_card(2048, 8, 0);
    /* The root's one cluster is full of entries that match nothing, and its chain leads back to it */
    uint8_t junk[512];
    memset(junk, 'A', sizeof(junk));
    const uint32_t root = fake_fat_root(&card);
    for(uint32_t s = 0; s < 8u; s++) {
        fake_fat_poke(&card, fake_fat_file_lba(&card, root, s * 512u), 0, junk, sizeof(junk));
    }
    fake_fat_set_next(&card, root, root);
    ASSERT_EQ(hp_omega_mount(&om), HpOmegaOk);
    const uint32_t reads = gba.omega_reads;
    ASSERT_EQ(hp_omega_open(&om, 0, "/nothing.sav"), HpOmegaNotFound);
    ASSERT_TRUE(gba.omega_reads - reads <= 4097u);
    fake_fat_free(&card);
}

/* ---- The Omega commands (hp_core.h) ---- */

#define MIB 0x100000u
#define TXT_LEN 70000u

static Hp hp;
static uint16_t resp[HELPER_CMD_RESP_MAX];
static const uint16_t* const p = resp + 2;

/* payload words i and i + 1 as a 32-bit number */
static uint32_t w32(uint32_t i) {
    return (uint32_t)p[i] | (uint32_t)p[i + 1u] << 16;
}
static uint8_t hdr[0xC0];

/* An Omega at power-on: the kernel's header, title and code empty; the card as make_card's, plus
 * /ROMS/game.gba of TXT_LEN bytes */
static void omega_cart_ex(uint8_t spc, uint32_t gap_every) {
    static const char zero4[4] = {0};
    make_card(2048, spc, gap_every);
    const uint32_t roms = fake_fat_mkdir(&card, fake_fat_root(&card), "ROMS");
    fake_fat_add_file(&card, roms, "game.gba", data, TXT_LEN, 2);
    helper_cmd_header(hdr, gba.own, "", zero4, 0);
    fake_gba_rom(&gba, hdr, MIB);
    hp_init(&hp);
}

static void omega_cart(void) {
    omega_cart_ex(8, 0);
}

static uint16_t run(uint8_t seq, uint8_t code, const uint16_t* args, uint16_t n) {
    return helper_cmd_run(&hp, seq, code, args, n, resp);
}

static uint16_t open_path(uint8_t seq, uint8_t slot, const char* path) {
    uint16_t a[50];
    memset(a, 0, sizeof(a));
    a[0] = slot;
    a[1] = (uint16_t)strlen(path);
    for(uint32_t i = 0; path[i]; i++) a[2u + i / 2u] |= (uint16_t)((uint8_t)path[i] << ((i & 1u) * 8u));
    return run(seq, HP_CMD_OMEGA_OPEN, a, 50);
}

/* resp's payload bytes, little-endian, equal q for n bytes */
static bool payload_is(const uint8_t* q, uint32_t n) {
    for(uint32_t i = 0; i < n; i++) {
        if((uint8_t)(p[i / 2u] >> ((i & 1u) * 8u)) != q[i]) return false;
    }
    return true;
}

TEST(test_cmd_mount_refused_genuine) {
    omega_cart();
    helper_cmd_header(hdr, gba.own, "POKEMON RUBY", "AXVE", 0);
    fake_gba_rom(&gba, hdr, 16u * MIB);
    fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
    ASSERT_EQ(run(1, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    const uint32_t reads = gba.omega_reads;
    ASSERT_EQ(run(2, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_CART);
    ASSERT_EQ(gba.omega_reads, reads); /* not one Omega command went to the cart */
    fake_fat_free(&card);
}

TEST(test_cmd_mount_before_info) {
    omega_cart();
    ASSERT_EQ(run(1, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_CART);
    ASSERT_EQ(gba.omega_reads, 0);
    fake_fat_free(&card);
}

TEST(test_cmd_mount_refused_titled) {
    static const char zero4[4] = {0};
    omega_cart();
    helper_cmd_header(hdr, gba.own, "HOMEBREW", zero4, 0);
    fake_gba_rom(&gba, hdr, MIB);
    ASSERT_EQ(run(1, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    const uint32_t reads = gba.omega_reads;
    ASSERT_EQ(run(2, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_CART);
    ASSERT_EQ(gba.omega_reads, reads);
    /* A code with no title is refused too */
    helper_cmd_header(hdr, gba.own, "", "ABCD", 0);
    fake_gba_rom(&gba, hdr, MIB);
    ASSERT_EQ(run(3, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(run(4, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_CART);
    ASSERT_EQ(gba.omega_reads, reads);
    fake_fat_free(&card);
}

TEST(test_cmd_feed_runs) {
    omega_cart();
    ASSERT_EQ(run(1, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(run(2, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_OK);
    ASSERT_EQ(open_path(3, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_OK);
    const uint32_t reads = gba.omega_reads;
    const uint16_t cr[5] = {0, 0, 0, 4096, 0};
    ASSERT_EQ(run(4, HP_CMD_OMEGA_CRC32, cr, 5), HP_ST_OK);
    ASSERT_EQ(w32(0), ~hp_crc32_bytes(0xFFFFFFFFu, data, 4096));
    /* 8 blocks in one cluster: two runs of 4, and no FAT block needed */
    ASSERT_EQ(gba.omega_reads - reads, 2u);
    fake_fat_free(&card);
}

TEST(test_cmd_read_before_mount) {
    omega_cart();
    ASSERT_EQ(run(1, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(open_path(2, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_OMEGA);
    const uint16_t rd[4] = {0, 0, 0, 1};
    ASSERT_EQ(run(3, HP_CMD_OMEGA_READ, rd, 4), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_OMEGA);
    fake_fat_free(&card);
}

TEST(test_cmd_omega_flow) {
    omega_cart();
    ASSERT_EQ(run(1, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(run(2, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_OK);
    ASSERT_EQ(resp[1], 9);
    ASSERT_EQ(w32(0), 2048);
    ASSERT_EQ(p[2], 8);
    ASSERT_EQ(w32(7), fake_fat_root(&card));
    /* the save: open, read 8 blocks at 4096, CRC-32 of the first 4 KB */
    ASSERT_EQ(open_path(3, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_OK);
    ASSERT_EQ(resp[1], 6);
    ASSERT_EQ(w32(0), SAV_LEN);
    const uint16_t rd[4] = {0, 4096, 0, 8};
    ASSERT_EQ(run(4, HP_CMD_OMEGA_READ, rd, 4), HP_ST_OK);
    ASSERT_EQ(resp[1], 8 * 256);
    ASSERT_TRUE(payload_is(data + 4096, 4096));
    const uint16_t cr[5] = {0, 0, 0, 4096, 0};
    ASSERT_EQ(run(5, HP_CMD_OMEGA_CRC32, cr, 5), HP_ST_OK);
    ASSERT_EQ(w32(0), ~hp_crc32_bytes(0xFFFFFFFFu, data, 4096));
    /* the ROM: SHA-256 of all TXT_LEN bytes in one call, then in two */
    ASSERT_EQ(open_path(6, 2, "/ROMS/game.gba"), HP_ST_OK);
    HpSha256 s;
    uint8_t d[32];
    hp_sha256_init(&s);
    hp_sha256_update(&s, data, TXT_LEN);
    hp_sha256_final(&s, d);
    const uint16_t one[6] = {2, HP_SHA_INIT | HP_SHA_FINAL, 0, 0, (uint16_t)TXT_LEN, (uint16_t)(TXT_LEN >> 16)};
    ASSERT_EQ(run(7, HP_CMD_OMEGA_SHA256, one, 6), HP_ST_OK);
    ASSERT_EQ(resp[1], 16);
    ASSERT_TRUE(payload_is(d, 32));
    const uint16_t first[6] = {2, HP_SHA_INIT, 0, 0, 65536u & 0xFFFFu, 1};
    ASSERT_EQ(run(8, HP_CMD_OMEGA_SHA256, first, 6), HP_ST_OK);
    ASSERT_EQ(resp[1], 0);
    const uint16_t rest[6] = {2, HP_SHA_FINAL, 0, 1, (uint16_t)(TXT_LEN - 65536u), 0};
    ASSERT_EQ(run(9, HP_CMD_OMEGA_SHA256, rest, 6), HP_ST_OK);
    ASSERT_EQ(resp[1], 16);
    ASSERT_TRUE(payload_is(d, 32));
    fake_fat_free(&card);
}

TEST(test_cmd_sha_continuity) {
    omega_cart();
    ASSERT_EQ(run(1, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(run(2, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_OK);
    ASSERT_EQ(open_path(3, 2, "/ROMS/game.gba"), HP_ST_OK);
    /* without INIT before any hash, at the wrong offset, a length not a multiple of 512 short of
     * the end, past the end, and over 1 MiB */
    const uint16_t no_init[6] = {2, 0, 0, 0, 512, 0};
    ASSERT_EQ(run(4, HP_CMD_OMEGA_SHA256, no_init, 6), HP_ST_BAD_ARG);
    const uint16_t first[6] = {2, HP_SHA_INIT, 0, 0, 4096, 0};
    ASSERT_EQ(run(5, HP_CMD_OMEGA_SHA256, first, 6), HP_ST_OK);
    const uint16_t gap[6] = {2, 0, 8192, 0, 512, 0};
    ASSERT_EQ(run(6, HP_CMD_OMEGA_SHA256, gap, 6), HP_ST_BAD_ARG);
    const uint16_t odd[6] = {2, 0, 4096, 0, 100, 0};
    ASSERT_EQ(run(7, HP_CMD_OMEGA_SHA256, odd, 6), HP_ST_BAD_ARG);
    const uint16_t past[6] = {2, 0, 4096, 0, (uint16_t)(TXT_LEN), 0};
    ASSERT_EQ(run(8, HP_CMD_OMEGA_SHA256, past, 6), HP_ST_BAD_ARG);
    const uint16_t big[6] = {2, HP_SHA_INIT, 0, 0, 0, 0x11};
    ASSERT_EQ(run(9, HP_CMD_OMEGA_SHA256, big, 6), HP_ST_BAD_ARG);
    fake_fat_free(&card);
}

TEST(test_cmd_open_notfound) {
    omega_cart();
    ASSERT_EQ(run(1, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(run(2, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_OK);
    ASSERT_EQ(open_path(3, 0, "/SAVER/Pokemon - Ruby.sav"), HP_ST_UNSUPPORTED);
    ASSERT_EQ(p[0], HpOmegaNotFound);
    /* a path length that doesn't match its bytes, or over 95 */
    uint16_t a[50];
    memset(a, 0, sizeof(a));
    a[1] = 96;
    ASSERT_EQ(run(4, HP_CMD_OMEGA_OPEN, a, 50), HP_ST_BAD_ARG);
    a[1] = 0;
    ASSERT_EQ(run(5, HP_CMD_OMEGA_OPEN, a, 50), HP_ST_BAD_ARG);
    /* HELLO ends the mount */
    ASSERT_EQ(run(6, HP_CMD_HELLO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(open_path(7, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_OMEGA);
    fake_fat_free(&card);
}

/* ---- The save file's write (OMEGA_ARM, OMEGA_WRITE_SECTOR) ---- */

#define SECTOR 4096u

static uint8_t img[SAV_LEN]; /* what the tests write: sector k is img + k * SECTOR */
static uint16_t wargs[1u + SECTOR / 2u];
static uint32_t save_first; /* the open save's first cluster */
static uint64_t card_digest; /* fake_fat_digest_except before the writes */

/* CART_INFO, MOUNT and OPEN of the save into slot 0, as seq 1..3 */
static void open_save(void) {
    ASSERT_EQ(run(1, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(run(2, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_OK);
    ASSERT_EQ(open_path(3, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_OK);
    save_first = w32(2);
}

static uint16_t arm_with(uint8_t seq, uint32_t size, uint32_t first) {
    const uint16_t a[5] = {
        0x5752, (uint16_t)size, (uint16_t)(size >> 16), (uint16_t)first, (uint16_t)(first >> 16)};
    return run(seq, HP_CMD_OMEGA_ARM, a, 5);
}

static uint16_t write_sector(uint8_t seq, uint16_t sector, const uint8_t* image) {
    wargs[0] = sector;
    for(uint32_t i = 0; i < SECTOR / 2u; i++) wargs[1u + i] = (uint16_t)(image[2u * i] | image[2u * i + 1u] << 8);
    return run(seq, HP_CMD_OMEGA_WRITE_SECTOR, wargs, 1u + SECTOR / 2u);
}

/* The open save's bytes off .. off + n on the card equal q */
static bool file_is(uint32_t off, const uint8_t* q, uint32_t n) {
    for(uint32_t i = 0; i < n; i += 512u) {
        uint8_t b[512];
        if(!fake_fat_block(&card, fake_fat_file_lba(&card, save_first, off + i), b)) return false;
        if(memcmp(b, q + i, 512) != 0) return false;
    }
    return true;
}

static void armed_save(uint8_t spc, uint32_t gap_every) {
    omega_cart_ex(spc, gap_every);
    pattern(img, SAV_LEN, 99);
    open_save();
    card_digest = fake_fat_digest_except(&card, save_first);
    ASSERT_EQ(arm_with(4, SAV_LEN, save_first), HP_ST_OK);
    ASSERT_EQ(resp[1], 0);
}

TEST(test_cmd_write_ok) {
    armed_save(8, 0);
    ASSERT_EQ(write_sector(5, 5, img + 5u * SECTOR), HP_ST_OK);
    ASSERT_EQ(resp[1], 2);
    ASSERT_EQ(w32(0), ~hp_crc32_bytes(0xFFFFFFFFu, img + 5u * SECTOR, SECTOR));
    ASSERT_EQ(gba.omega_writes, 8);
    ASSERT_TRUE(file_is(5u * SECTOR, img + 5u * SECTOR, SECTOR));
    /* its neighbours are as they were */
    ASSERT_TRUE(file_is(4u * SECTOR, data + 4u * SECTOR, SECTOR));
    ASSERT_TRUE(file_is(6u * SECTOR, data + 6u * SECTOR, SECTOR));
    ASSERT_EQ(fake_fat_digest_except(&card, save_first), card_digest);
    fake_fat_free(&card);
}

TEST(test_cmd_write_unarmed) {
    omega_cart();
    open_save();
    ASSERT_EQ(write_sector(4, 5, img), HP_ST_NOT_ARMED);
    ASSERT_EQ(gba.omega_writes, 0);
    /* armed, then each of HELLO, CART_INFO, MOUNT, OPEN of slot 0 and DISARM disarms */
    for(uint32_t how = 0; how < 5u; how++) {
        ASSERT_EQ(arm_with(10, SAV_LEN, save_first), HP_ST_OK);
        if(how == 0) {
            ASSERT_EQ(run(11, HP_CMD_HELLO, NULL, 0), HP_ST_OK);
            ASSERT_EQ(run(12, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_OK);
            ASSERT_EQ(open_path(13, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_OK);
        } else if(how == 1) {
            ASSERT_EQ(run(11, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
            ASSERT_EQ(run(12, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_OK);
            ASSERT_EQ(open_path(13, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_OK);
        } else if(how == 2) {
            ASSERT_EQ(run(11, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_OK);
            ASSERT_EQ(open_path(12, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_OK);
            ASSERT_EQ(arm_with(13, SAV_LEN, save_first), HP_ST_OK);
            ASSERT_EQ(run(14, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_OK);
        } else if(how == 3) {
            ASSERT_EQ(open_path(11, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_OK);
        } else {
            /* the save chip's DISARM ends the Omega's arming too */
            ASSERT_EQ(run(11, HP_CMD_DISARM, NULL, 0), HP_ST_OK);
        }
        ASSERT_EQ(write_sector(20, 5, img), HP_ST_NOT_ARMED);
        if(how == 2) ASSERT_EQ(open_path(21, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_OK);
    }
    ASSERT_EQ(gba.omega_writes, 0);
    /* OPEN of another slot does not disarm */
    ASSERT_EQ(arm_with(30, SAV_LEN, save_first), HP_ST_OK);
    ASSERT_EQ(open_path(31, 2, "/ROMS/game.gba"), HP_ST_OK);
    ASSERT_EQ(write_sector(32, 5, img), HP_ST_OK);
    fake_fat_free(&card);
}

TEST(test_cmd_arm_wrong_file) {
    omega_cart();
    ASSERT_EQ(run(1, HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    /* before MOUNT, and mounted with slot 0 not open */
    ASSERT_EQ(arm_with(2, SAV_LEN, 5), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_OMEGA);
    ASSERT_EQ(run(3, HP_CMD_OMEGA_MOUNT, NULL, 0), HP_ST_OK);
    ASSERT_EQ(arm_with(4, SAV_LEN, 5), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_OMEGA);
    /* the save open in slot 1 only, named exactly: a slot 1 file is not the save */
    ASSERT_EQ(open_path(5, 1, "/SAVER/Pokemon - Unbound.sav"), HP_ST_OK);
    save_first = w32(2);
    ASSERT_EQ(arm_with(6, SAV_LEN, save_first), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_OMEGA);
    /* slot 0 open on a file of another size than a save's, named exactly: never armed */
    ASSERT_EQ(open_path(7, 0, "/ROMS/game.gba"), HP_ST_OK);
    ASSERT_EQ(w32(0), TXT_LEN);
    const uint32_t rom_first = w32(2);
    ASSERT_EQ(arm_with(8, TXT_LEN, rom_first), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_OMEGA);
    ASSERT_EQ(write_sector(9, 0, img), HP_ST_NOT_ARMED);
    ASSERT_EQ(open_path(10, 0, "/SAVER/Pokemon - Unbound.sav"), HP_ST_OK);
    ASSERT_EQ(w32(2), save_first);
    /* another size or cluster, and a wrong magic */
    ASSERT_EQ(arm_with(11, SAV_LEN - 512u, save_first), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_OMEGA);
    ASSERT_EQ(arm_with(12, SAV_LEN, save_first + 1u), HP_ST_REFUSED);
    ASSERT_EQ(arm_with(13, SAV_LEN + 1u, save_first), HP_ST_REFUSED);
    const uint16_t magic[5] = {0x5753, (uint16_t)SAV_LEN, (uint16_t)(SAV_LEN >> 16), (uint16_t)save_first, 0};
    ASSERT_EQ(run(14, HP_CMD_OMEGA_ARM, magic, 5), HP_ST_BAD_ARG);
    /* a good arming, then a refused one, leaves nothing armed */
    ASSERT_EQ(arm_with(15, SAV_LEN, save_first), HP_ST_OK);
    ASSERT_EQ(arm_with(16, SAV_LEN, save_first + 1u), HP_ST_REFUSED);
    ASSERT_EQ(write_sector(17, 0, img), HP_ST_NOT_ARMED);
    ASSERT_EQ(gba.omega_writes, 0);
    fake_fat_free(&card);
}

TEST(test_cmd_write_range) {
    armed_save(8, 0);
    ASSERT_EQ(write_sector(5, 32, img), HP_ST_BAD_ARG);
    ASSERT_EQ(write_sector(6, 0xFFFF, img), HP_ST_BAD_ARG);
    ASSERT_EQ(gba.omega_writes, 0);
    fake_fat_free(&card);
}

TEST(test_cmd_write_fragmented) {
    armed_save(1, 1);
    for(uint16_t s = 0; s < 32u; s++) {
        ASSERT_EQ(write_sector((uint8_t)(10 + s), s, img + s * SECTOR), HP_ST_OK);
        ASSERT_EQ(w32(0), ~hp_crc32_bytes(0xFFFFFFFFu, img + s * SECTOR, SECTOR));
    }
    ASSERT_EQ(gba.omega_writes, 256);
    ASSERT_TRUE(file_is(0, img, SAV_LEN));
    ASSERT_EQ(fake_fat_digest_except(&card, save_first), card_digest);
    fake_fat_free(&card);
}

TEST(test_cmd_write_bad_chain) {
    armed_save(8, 0);
    fake_fat_set_next(&card, save_first, card.clusters + 9u);
    const uint64_t digest = fake_fat_digest_except(&card, save_first);
    ASSERT_EQ(write_sector(5, 20, img), HP_ST_UNSUPPORTED);
    ASSERT_EQ(p[0], HpOmegaSdError);
    ASSERT_EQ(gba.omega_writes, 0);
    ASSERT_EQ(fake_fat_digest_except(&card, save_first), digest);
    /* a chain that ends early (a free entry after the first cluster) refuses sector 1, whose one
     * cluster (4 KB clusters here) it no longer reaches */
    fake_fat_set_next(&card, save_first, 0); /* a free entry */
    ASSERT_EQ(write_sector(6, 1, img), HP_ST_UNSUPPORTED);
    ASSERT_EQ(gba.omega_writes, 0);
    fake_fat_free(&card);
}

TEST(test_cmd_write_bad_chain_small_clusters) {
    /* A sector spans 8 clusters; the last is outside the volume. No block of it is written. */
    armed_save(1, 0);
    fake_fat_set_next(&card, save_first + 8u * 3u + 6u, card.clusters + 9u);
    const uint64_t whole = fake_fat_digest_except(&card, 0);
    ASSERT_EQ(write_sector(5, 3, img + 3u * SECTOR), HP_ST_UNSUPPORTED);
    ASSERT_EQ(gba.omega_writes, 0);
    ASSERT_EQ(fake_fat_digest_except(&card, 0), whole);
    fake_fat_free(&card);
}

TEST(test_cmd_write_flip) {
    armed_save(8, 0);
    fake_gba_omega_flip_after(&gba, 0);
    ASSERT_EQ(write_sector(5, 5, img + 5u * SECTOR), HP_ST_VERIFY_FAIL);
    ASSERT_EQ(gba.omega_writes, 8);
    /* a flip in the last block is found too */
    fake_gba_omega_flip_after(&gba, 7);
    ASSERT_EQ(write_sector(6, 5, img + 5u * SECTOR), HP_ST_VERIFY_FAIL);
    fake_fat_free(&card);
}

TEST(test_cmd_write_cut) {
    armed_save(8, 0);
    fake_gba_omega_cut_after(&gba, 3);
    ASSERT_EQ(write_sector(5, 5, img + 5u * SECTOR), HP_ST_UNSUPPORTED);
    ASSERT_EQ(p[0], HpOmegaSdError);
    ASSERT_EQ(gba.omega_writes, 3);
    ASSERT_EQ(fake_fat_digest_except(&card, save_first), card_digest);
    fake_fat_free(&card);
}

TEST(test_cmd_write_resend) {
    armed_save(8, 0);
    ASSERT_EQ(write_sector(5, 3, img + 3u * SECTOR), HP_ST_OK);
    const uint32_t crc = w32(0);
    ASSERT_EQ(gba.omega_writes, 8);
    /* the same packet again, as when the response went missing: the stored response, no write */
    ASSERT_EQ(write_sector(5, 3, img + 3u * SECTOR), HP_ST_OK);
    ASSERT_EQ(w32(0), crc);
    ASSERT_EQ(gba.omega_writes, 8);
    /* a new seq is a new request */
    ASSERT_EQ(write_sector(6, 3, img + 3u * SECTOR), HP_ST_OK);
    ASSERT_EQ(gba.omega_writes, 16);
    fake_fat_free(&card);
}

int main(void) {
    RUN(test_omega_mount_mbr);
    RUN(test_omega_mount_superfloppy);
    RUN(test_omega_mount_no_card);
    RUN(test_omega_mount_exfat);
    RUN(test_omega_mount_bad_fat_size);
    RUN(test_omega_open_lfn);
    RUN(test_omega_open_missing);
    RUN(test_omega_open_before_mount);
    RUN(test_omega_lfn_split);
    RUN(test_omega_read_fragmented);
    RUN(test_omega_read_bounds);
    RUN(test_omega_chain_out_of_range);
    RUN(test_omega_lfn_missing_seq);
    RUN(test_omega_lfn_bad_sum);
    RUN(test_omega_dir_loop);
    RUN(test_cmd_mount_refused_genuine);
    RUN(test_cmd_mount_refused_titled);
    RUN(test_cmd_feed_runs);
    RUN(test_cmd_mount_before_info);
    RUN(test_cmd_read_before_mount);
    RUN(test_cmd_omega_flow);
    RUN(test_cmd_sha_continuity);
    RUN(test_cmd_open_notfound);
    RUN(test_cmd_write_ok);
    RUN(test_cmd_write_unarmed);
    RUN(test_cmd_arm_wrong_file);
    RUN(test_cmd_write_range);
    RUN(test_cmd_write_fragmented);
    RUN(test_cmd_write_bad_chain);
    RUN(test_cmd_write_bad_chain_small_clusters);
    RUN(test_cmd_write_flip);
    RUN(test_cmd_write_cut);
    RUN(test_cmd_write_resend);
    TEST_EXIT();
}
