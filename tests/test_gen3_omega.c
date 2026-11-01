#include "test.h"
#include "fake_fat.h"
#include "fake_gba.h"
#include "fake_helper.h"
#include "fake_store.h"
#include "fake_ub_save.h"
#include "helper_cmd.h"
#include "ub_bulk_file.h"
#include <stdbool.h>
#include <stdlib.h>
#include <hp_core.h>
#include <hp_omega.h>
#include <hp_sha256.h>
#include <src/gen3/gen3_omega.h>
#include <src/gen3/gen3_rules.h>
#include <src/gen3/gen3_save.h>
#include <src/gen3/gen3_text.h>

/* The Omega jobs (gen3_omega.h) against the helper's real core on the fake GBA, with an EZ-Flash
 * Omega's card (fake_fat.h) and a RAM store. The default card: /ROMS/Pokemon - Unbound.gba
 * (3 MiB, fragmented), /SAVER/Pokemon - Unbound.sav (an Unbound save: slot A, counter 20, trainer
 * RED 12345/54321, a party of 2, box 3 slot 5 filled), and /SAVER/Recently play.txt naming the
 * ROM. The Omega's kernel header: title and code empty; the DE flag word reads 0x8000 (open bus). */

#define MIB 0x100000u
#define ROM_LEN (3u * MIB)
#define STAMP "20261101-120000"
#define BACKUP "backups/UNBOUND_12345_" STAMP ".sav"
#define ROM_PATH "/ROMS/Pokemon - Unbound.gba"
#define FOREVER 10000000u

static FakeGba gba;
static FakeHelper fh;
static Gen3Hlink pump;
static Gen3Hcli cli;
static FakeStore store;
static Gen3Store api;
static Gen3Omega om;
static FakeFat card;
static uint8_t buf[GEN3_CART_BUF_SIZE];
static uint8_t save_img[FAKE_UB_LEN + 16u];
static uint8_t* rom;
static uint8_t rom_sha[32];
static uint8_t hdr[0xC0];
static volatile bool cancel;

/* ---- Observers ---- */

typedef struct {
    uint32_t calls[256];
    uint32_t unplug_sha_at; /* the cable out for good at this SHA256 call (1 = the first) */
} Hook;

static Hook hook;

static void hook_cb(void* ctx, uint8_t code, uint16_t arg0) {
    (void)arg0;
    Hook* k = ctx;
    k->calls[code]++;
    if(k->unplug_sha_at && code == GEN3_HCMD_OMEGA_SHA256 && k->calls[code] == k->unplug_sha_at) {
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), FOREVER);
    }
}

typedef struct {
    uint32_t calls[Gen3CartStepRomHash + 1];
    uint32_t last_done[Gen3CartStepRomHash + 1];
    Gen3CartStep cancel_step;
    uint32_t cancel_at;
} Prog;

static Prog prog;

static void prog_cb(void* ctx, Gen3CartStep step, uint32_t done, uint32_t total) {
    (void)total;
    Prog* p = ctx;
    p->calls[step]++;
    p->last_done[step] = done;
    if(p->cancel_step == step && done >= p->cancel_at) cancel = true;
}

/* ---- Fixtures ---- */

static void make_save(void) {
    uint8_t nick[10], name[8], rec[100], box[58];
    fake_ub_erase(save_img);
    fake_ub_build(save_img, 0, 20, 0);
    gen3_text_encode("RED", name, 8, 7);
    fake_ub_put_trainer(save_img, 0, name, 0, 12345, 54321);
    gen3_text_encode("SPROUT", nick, 10, 10);
    fake_ub_party_rec(rec, 1, 12, nick, 0);
    fake_ub_put_party(save_img, 0, 0, rec);
    fake_ub_party_rec(rec, 1000, 45, nick, 0);
    fake_ub_put_party(save_img, 0, 1, rec);
    fake_ub_set_party_count(save_img, 0, 2);
    fake_ub_box_rec(box, 1265, gen3_exp_for_level(0, 30), nick, 0);
    fake_ub_put_box(save_img, 0, 3, 5, box);
}

/* A card with the ROM at rom_name in /ROMS, the save at save_name in /SAVER (save_len bytes), and
 * Recently play.txt holding recent (none if NULL); the helper, client, store and Omega fresh,
 * connected and CART_INFO done */
static bool bench(const char* recent, const char* rom_name, const char* save_name, uint32_t save_len) {
    static const char zero4[4] = {0};
    fake_gba_init(&gba);
    helper_cmd_header(hdr, gba.own, "", zero4, 0);
    fake_gba_rom(&gba, hdr, MIB);
    fake_fat_free(&card);
    fake_fat_init(&card, 2048, 64, 1u << 17);
    const uint32_t roms = fake_fat_mkdir(&card, fake_fat_root(&card), "ROMS");
    const uint32_t saver = fake_fat_mkdir(&card, fake_fat_root(&card), "SAVER");
    fake_fat_add_file(&card, roms, rom_name, rom, ROM_LEN, 5);
    fake_fat_add_file(&card, saver, save_name, save_img, save_len, 0);
    if(recent) fake_fat_add_file(&card, saver, "Recently play.txt", (const uint8_t*)recent, (uint32_t)strlen(recent), 0);
    fake_gba_omega(&gba, &card);
    fake_helper_init(&fh, &gba);
    fake_helper_set_wire(&fh, MlinkWireNormal);
    memset(&hook, 0, sizeof(hook));
    fake_helper_on_command(&fh, hook_cb, &hook);
    const Gen3HTransport t = fake_helper_transport(&fh, &pump);
    gen3_hcli_init(&cli, &t);
    memset(&prog, 0, sizeof(prog));
    cancel = false;
    const Gen3CartProgress p = {.cb = prog_cb, .ctx = &prog, .cancel = &cancel};
    gen3_omega_init(&om, &cli, &api, buf, &p);
    gen3_omega_set_expected_sha(rom_sha);
    Gen3HCartInfo info;
    return gen3_hcli_connect(&cli, NULL) == Gen3HcliOk &&
           gen3_hcli_cart_info(&cli, &info) == Gen3HcliOk;
}

static bool default_bench(void) {
    return bench(ROM_PATH "\r\n/ROMS/Other.gba\r\n", "Pokemon - Unbound.gba", "Pokemon - Unbound.sav", FAKE_UB_LEN);
}

/* A fresh store, then the default bench */
static bool setup(void) {
    fake_store_init(&store, 64u * MIB);
    api = fake_store_api(&store);
    make_save();
    return default_bench();
}

static bool absent(const char* name) {
    uint32_t size = 0;
    return fake_store_file(&store, name, &size) == NULL;
}

static uint32_t lines(const char* name) {
    uint32_t size = 0, n = 0;
    const uint8_t* d = fake_store_file(&store, name, &size);
    for(uint32_t i = 0; d && i < size; i++) n += d[i] == '\n';
    return n;
}

/* ---- Connect ---- */

TEST(test_omega_connect_ok) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_TRUE(om.hashed);
    ASSERT_TRUE(om.verified);
    ASSERT_TRUE(!om.from_cache);
    ASSERT_STR_EQ(om.rom_path, ROM_PATH);
    ASSERT_STR_EQ(om.save_path, "/SAVER/Pokemon - Unbound.sav");
    ASSERT_EQ(om.save.size, FAKE_UB_LEN);
    ASSERT_EQ(om.rom.size, ROM_LEN);
    ASSERT_EQ(om.info.part, 2048);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_SHA256], 3);
    ASSERT_EQ(prog.last_done[Gen3CartStepRomHash], 3);
    ASSERT_EQ(lines(GEN3_OMEGA_CACHE), 1);
    uint32_t size = 0;
    const uint8_t* d = fake_store_file(&store, GEN3_OMEGA_CACHE, &size);
    char hex[65];
    for(uint32_t i = 0; i < 32u; i++) snprintf(hex + 2u * i, 3, "%02x", rom_sha[i]);
    ASSERT_TRUE(d && size > 64u && memcmp(d, hex, 64) == 0);
}

TEST(test_omega_connect_cached) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_TRUE(om.from_cache);
    ASSERT_TRUE(om.verified);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_SHA256], 0);
    ASSERT_EQ(lines(GEN3_OMEGA_CACHE), 1);
    /* another ROM path: hashed, and a second line; the first line stays */
    ASSERT_TRUE(bench("/ROMS/B.gba\n", "B.gba", "B.sav", FAKE_UB_LEN));
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_TRUE(!om.from_cache);
    ASSERT_EQ(lines(GEN3_OMEGA_CACHE), 2);
}

TEST(test_omega_hash_mismatch) {
    ASSERT_TRUE(setup());
    uint8_t other[32];
    memset(other, 0x5A, sizeof(other));
    gen3_omega_set_expected_sha(other);
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_TRUE(om.hashed);
    ASSERT_TRUE(!om.verified);
    ASSERT_EQ(lines(GEN3_OMEGA_CACHE), 1);
}

TEST(test_omega_pending) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_TRUE(om.flag_read);
    ASSERT_EQ(om.flag, 0x8000); /* open bus past the kernel's 1 MiB */
    ASSERT_TRUE(om.mounted);
    /* a later connect that stops at the flag keeps nothing of the one before */
    fake_gba_rom_word(&gba, 0x790000u, 0x0011);
    hook.calls[GEN3_HCMD_OMEGA_MOUNT] = 0;
    gba.omega_reads = 0;
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOmegaPending);
    ASSERT_TRUE(om.flag_read);
    ASSERT_EQ(om.flag, 0x0011);
    ASSERT_TRUE(!om.mounted);
    ASSERT_STR_EQ(om.rom_path, "");
    ASSERT_STR_EQ(om.save_path, "");
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_MOUNT], 0);
    ASSERT_EQ(gba.omega_reads, 0);
}

TEST(test_omega_flag_values) {
    static const uint16_t vals[] = {0xFFFF, 0x0000, 0x0111};
    for(uint32_t i = 0; i < 3u; i++) {
        ASSERT_TRUE(setup());
        fake_gba_rom_word(&gba, 0x790000u, vals[i]);
        ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    }
}

TEST(test_omega_recent_lines) {
    fake_store_init(&store, 64u * MIB);
    api = fake_store_api(&store);
    make_save();
    static const char* const bad[] = {"x.gba\n", "/ROMS/a.txt\n", "", "\r\n/ROMS/Pokemon - Unbound.gba\n"};
    for(uint32_t i = 0; i < 4u; i++) {
        ASSERT_TRUE(bench(bad[i], "Pokemon - Unbound.gba", "Pokemon - Unbound.sav", FAKE_UB_LEN));
        ASSERT_EQ(gen3_omega_connect(&om), Gen3CartNoGame);
    }
    /* no Recently play.txt at all */
    ASSERT_TRUE(bench(NULL, "Pokemon - Unbound.gba", "Pokemon - Unbound.sav", FAKE_UB_LEN));
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartNoGame);
    /* a 96-character path */
    char longp[128];
    memset(longp, 'a', sizeof(longp));
    memcpy(longp, "/ROMS/", 6);
    memcpy(longp + 92, ".gba\n", 6);
    ASSERT_EQ(strlen(longp), 97); /* 96 characters and the newline */
    ASSERT_TRUE(bench(longp, "Pokemon - Unbound.gba", "Pokemon - Unbound.sav", FAKE_UB_LEN));
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartNoGame);
    /* trailing spaces, upper case: /SAVER/A.sav */
    ASSERT_TRUE(bench("/ROMS/A.GBA  \n", "A.GBA", "A.sav", FAKE_UB_LEN));
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_STR_EQ(om.rom_path, "/ROMS/A.GBA");
    ASSERT_STR_EQ(om.save_path, "/SAVER/A.sav");
    /* the save named in Recently play.txt isn't on the card */
    ASSERT_TRUE(bench("/ROMS/A.GBA\n", "A.GBA", "B.sav", FAKE_UB_LEN));
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartNoGame);
}

TEST(test_omega_save_size) {
    fake_store_init(&store, 64u * MIB);
    api = fake_store_api(&store);
    make_save();
    ASSERT_TRUE(bench(ROM_PATH "\n", "Pokemon - Unbound.gba", "Pokemon - Unbound.sav", 65536u));
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartNoGame);
    ASSERT_TRUE(bench(ROM_PATH "\n", "Pokemon - Unbound.gba", "Pokemon - Unbound.sav", FAKE_UB_LEN + 16u));
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartNoGame);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 0);
}

TEST(test_omega_not_unbound) {
    /* the last game's save is FireRed's: NoGame, before any backup or hash */
    fake_store_init(&store, 64u * MIB);
    api = fake_store_api(&store);
    make_save();
    for(uint32_t k = 0; k < 14u; k++) {
        uint8_t* f = save_img + k * 4096u + 0xFF8u;
        f[0] = 0x25;
        f[1] = 0x20;
        f[2] = 0x01;
        f[3] = 0x08;
    }
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartNoGame);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_SHA256], 0);
    ASSERT_TRUE(!om.connected);
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartBadArg);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 0);
    /* an Unbound save never saved yet (erased): NoGame too */
    fake_ub_erase(save_img);
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartNoGame);
    /* Unbound's signature in slot B only: Unbound's */
    make_save();
    fake_ub_build(save_img, 1, 21, 0);
    for(uint32_t k = 0; k < 14u; k++) save_img[k * 4096u + 0xFF8u] ^= 0x40;
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
}

TEST(test_omega_hash_link_lost) {
    ASSERT_TRUE(setup());
    /* On the Normal wire a pulled cable reads as a helper that stays busy (hardware-test-3 N.2) */
    hook.unplug_sha_at = 2;
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartBusyTimeout);
    ASSERT_TRUE(absent(GEN3_OMEGA_CACHE));
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_SHA256], 3);
    ASSERT_TRUE(om.verified);
}

TEST(test_omega_cancel_hash) {
    ASSERT_TRUE(setup());
    prog.cancel_step = Gen3CartStepRomHash;
    prog.cancel_at = 1;
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartCancelled);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_SHA256], 1);
    ASSERT_TRUE(absent(GEN3_OMEGA_CACHE));
}

/* ---- Backup and browsing ---- */

TEST(test_omega_backup_ok) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_TRUE(!gen3_omega_browsable(&om));
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    uint32_t size = 0;
    const uint8_t* d = fake_store_file(&store, BACKUP, &size);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ(size, FAKE_UB_LEN);
    ASSERT_MEM_EQ(d, save_img, FAKE_UB_LEN);
    ASSERT_TRUE(absent(GEN3_CART_BACKUP_TMP));
    ASSERT_STR_EQ(om.backup_name, BACKUP);
    ASSERT_TRUE(gen3_omega_browsable(&om));
    ASSERT_TRUE(om.has_trainer);
    ASSERT_STR_EQ(om.trainer.name, "RED");
    ASSERT_EQ(om.slots.loaded, 0);
    ASSERT_EQ(gen3_omega_party_count(&om), 2);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_CRC32], 32);
    ASSERT_EQ(prog.last_done[Gen3CartStepBackupRead], 32);
    /* a second backup with the same stamp: -2 */
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    ASSERT_STR_EQ(om.backup_name, "backups/UNBOUND_12345_" STAMP "-2.sav");
}

TEST(test_omega_backup_nosave) {
    /* an Unbound save with no slot the game loads (a bad checksum): backed up all the same, named
     * nosave, not browsable */
    fake_store_init(&store, 64u * MIB);
    api = fake_store_api(&store);
    make_save();
    save_img[fake_ub_phys(save_img, 0, 7) * 4096u + 0x10] ^= 0x01;
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    ASSERT_STR_EQ(om.backup_name, "backups/UNBOUND_nosave_" STAMP ".sav");
    ASSERT_TRUE(!gen3_omega_browsable(&om));
    ASSERT_TRUE(!om.has_trainer);
}

TEST(test_omega_backup_readback_mismatch) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    fake_store_corrupt_read(&store, GEN3_CART_BACKUP_TMP, 7u * 4096u + 9u);
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartVerifyFailed);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 0);
    ASSERT_TRUE(absent(GEN3_CART_BACKUP_TMP));
    ASSERT_TRUE(!gen3_omega_browsable(&om));
}

TEST(test_omega_backup_sd_full) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    fake_store_report_free(&store, FAKE_UB_LEN);
    const uint32_t reads = hook.calls[GEN3_HCMD_OMEGA_READ];
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartSdFull);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_READ], reads);
}

TEST(test_omega_backup_link_lost) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh) + 2000u, FOREVER);
    const Gen3CartResult r = gen3_omega_backup(&om, STAMP);
    ASSERT_TRUE(r == Gen3CartBusyTimeout || r == Gen3CartLinkLost);
    ASSERT_TRUE(!om.connected);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 0);
    /* no command goes to a helper that may still be busy: a new Connect first */
    const uint32_t reads = hook.calls[GEN3_HCMD_OMEGA_READ];
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartBadArg);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_READ], reads);
}

TEST(test_omega_backup_cancel) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    prog.cancel_step = Gen3CartStepBackupRead;
    prog.cancel_at = 5;
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartCancelled);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 0);
    ASSERT_TRUE(absent(GEN3_CART_BACKUP_TMP));
}

TEST(test_omega_summary) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    Gen3UbSummary m;
    ASSERT_EQ(gen3_omega_summary(&om, GEN3_SAVE_PARTY, 0, &m), Gen3CartMonBadArg); /* no backup */
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    ASSERT_EQ(gen3_omega_summary(&om, GEN3_SAVE_PARTY, 0, &m), Gen3CartMonOk);
    ASSERT_EQ(m.species, 1);
    ASSERT_EQ(m.level, 12);
    ASSERT_STR_EQ(m.nickname, "SPROUT");
    ASSERT_EQ(gen3_omega_summary(&om, GEN3_SAVE_PARTY, 1, &m), Gen3CartMonOk);
    ASSERT_EQ(m.species, 1000);
    ASSERT_EQ(m.level, 45);
    ASSERT_EQ(gen3_omega_summary(&om, GEN3_SAVE_PARTY, 2, &m), Gen3CartMonEmpty);
    ASSERT_EQ(gen3_omega_summary(&om, GEN3_SAVE_PARTY, 6, &m), Gen3CartMonBadArg);
    ASSERT_EQ(gen3_omega_summary(&om, 3, 5, &m), Gen3CartMonOk);
    ASSERT_EQ(m.species, 1265);
    ASSERT_EQ(m.level, 30); /* growth 0 for species 1265 in the data file */
    ASSERT_EQ(gen3_omega_summary(&om, 3, 6, &m), Gen3CartMonEmpty);
    ASSERT_EQ(gen3_omega_summary(&om, 24, 0, &m), Gen3CartMonBadArg);
    ASSERT_EQ(gen3_omega_summary(&om, 0, 30, &m), Gen3CartMonBadArg);
    fake_store_fail_read(&store, 0);
    ASSERT_EQ(gen3_omega_summary(&om, 3, 5, &m), Gen3CartMonSdError);
}

int main(void) {
    if(!ub_bulk_file_open()) return 1;
    rom = malloc(ROM_LEN);
    uint32_t x = 99u;
    for(uint32_t i = 0; i < ROM_LEN; i++) rom[i] = (uint8_t)((x = x * 1103515245u + 12345u) >> 16);
    HpSha256 s;
    hp_sha256_init(&s);
    hp_sha256_update(&s, rom, ROM_LEN);
    hp_sha256_final(&s, rom_sha);
    RUN(test_omega_connect_ok);
    RUN(test_omega_connect_cached);
    RUN(test_omega_hash_mismatch);
    RUN(test_omega_pending);
    RUN(test_omega_flag_values);
    RUN(test_omega_recent_lines);
    RUN(test_omega_save_size);
    RUN(test_omega_not_unbound);
    RUN(test_omega_hash_link_lost);
    RUN(test_omega_cancel_hash);
    RUN(test_omega_backup_ok);
    RUN(test_omega_backup_nosave);
    RUN(test_omega_backup_readback_mismatch);
    RUN(test_omega_backup_sd_full);
    RUN(test_omega_backup_link_lost);
    RUN(test_omega_backup_cancel);
    RUN(test_omega_summary);
    fake_fat_free(&card);
    free(rom);
    TEST_EXIT();
}
