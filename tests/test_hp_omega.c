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
static void omega_cart(void) {
    static const char zero4[4] = {0};
    make_card(2048, 8, 0);
    const uint32_t roms = fake_fat_mkdir(&card, fake_fat_root(&card), "ROMS");
    fake_fat_add_file(&card, roms, "game.gba", data, TXT_LEN, 2);
    helper_cmd_header(hdr, gba.own, "", zero4, 0);
    fake_gba_rom(&gba, hdr, MIB);
    hp_init(&hp);
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
    TEST_EXIT();
}
