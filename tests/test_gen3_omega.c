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
#include <src/gen3/gen3_ub_mon.h>

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
    uint32_t unplug_write_at; /* ... or once this OMEGA_WRITE_SECTOR has run */
    uint32_t poke_write_at; /* once this OMEGA_WRITE_SECTOR has run, byte poke_off of the save */
    uint32_t poke_off; /* on the card is changed, as a write that landed there too would */
} Hook;

static Hook hook;

static void hook_cb(void* ctx, uint8_t code, uint16_t arg0) {
    (void)arg0;
    Hook* k = ctx;
    k->calls[code]++;
    if(k->unplug_sha_at && code == GEN3_HCMD_OMEGA_SHA256 && k->calls[code] == k->unplug_sha_at) {
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), FOREVER);
    }
    if(code != GEN3_HCMD_OMEGA_WRITE_SECTOR) return;
    if(k->unplug_write_at && k->calls[code] == k->unplug_write_at) {
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), FOREVER);
    }
    if(k->poke_write_at && k->calls[code] == k->poke_write_at) {
        const uint32_t lba = fake_fat_file_lba(&card, om.save.first_cluster, k->poke_off);
        uint8_t b[512];
        fake_fat_block(&card, lba, b);
        b[k->poke_off % 512u] ^= 0x10u;
        fake_fat_poke(&card, lba, k->poke_off % 512u, &b[k->poke_off % 512u], 1);
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

/* The default save in slot `slot` (the other one erased), with that counter and rotation */
static void make_save_at(uint8_t slot, uint32_t counter, uint8_t rotate) {
    uint8_t nick[10], name[8], rec[100], box[58];
    fake_ub_erase(save_img);
    fake_ub_build(save_img, slot, counter, rotate);
    gen3_text_encode("RED", name, 8, 7);
    fake_ub_put_trainer(save_img, slot, name, 0, 12345, 54321);
    gen3_text_encode("SPROUT", nick, 10, 10);
    fake_ub_party_rec(rec, 1, 12, nick, 0);
    fake_ub_put_party(save_img, slot, 0, rec);
    fake_ub_party_rec(rec, 1000, 45, nick, 0);
    fake_ub_put_party(save_img, slot, 1, rec);
    fake_ub_set_party_count(save_img, slot, 2);
    fake_ub_box_rec(box, 1265, gen3_exp_for_level(0, 30), nick, 0);
    fake_ub_put_box(save_img, slot, 3, 5, box);
}

static void make_save(void) {
    make_save_at(0, 20, 0);
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

/* ---- The working copy and edits ---- */

static bool backed_up(void) {
    return setup() && gen3_omega_connect(&om) == Gen3CartOk &&
           gen3_omega_backup(&om, STAMP) == Gen3CartOk;
}

static uint32_t bits(uint32_t v) {
    uint32_t n = 0;
    for(; v; v &= v - 1u) n++;
    return n;
}

TEST(test_omega_work_copy) {
    ASSERT_TRUE(backed_up());
    uint32_t size = 0;
    const uint8_t* w = fake_store_file(&store, GEN3_OMEGA_WORK, &size);
    ASSERT_TRUE(w != NULL);
    ASSERT_EQ(size, FAKE_UB_LEN);
    ASSERT_MEM_EQ(w, save_img, FAKE_UB_LEN);
    ASSERT_TRUE(om.work_ok);
    ASSERT_EQ(gen3_omega_changes(&om), 0);
    ASSERT_EQ(om.dirty, 0);
    ASSERT_TRUE(!gen3_omega_mon_changed(&om, 3, 5));
}

TEST(test_omega_work_copy_fails) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    /* the backup's 32 writes succeed, the working copy's first does not */
    fake_store_fail_write_after(&store, 32);
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    ASSERT_TRUE(gen3_omega_browsable(&om));
    ASSERT_TRUE(!om.work_ok);
    ASSERT_TRUE(absent(GEN3_OMEGA_WORK));
    Gen3UbMon m;
    ASSERT_EQ(gen3_omega_mon_get(&om, 3, 5, &m), Gen3CartMonBadArg);
}

TEST(test_omega_put_box) {
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_EQ(gen3_omega_mon_get(&om, 3, 5, &m), Gen3CartMonOk);
    ASSERT_TRUE(!m.party);
    ASSERT_EQ(gen3_ub_mon_species(&m), 1265);
    gen3_ub_mon_set_friendship(&m, 77);
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 5, &m, false), Gen3CartMonOk);
    Gen3UbMon back;
    ASSERT_EQ(gen3_omega_mon_get(&om, 3, 5, &back), Gen3CartMonOk);
    ASSERT_MEM_EQ(back.raw, m.raw, 58);
    ASSERT_EQ(gen3_ub_mon_friendship(&back), 77);
    ASSERT_TRUE(gen3_omega_mon_changed(&om, 3, 5));
    ASSERT_TRUE(!gen3_omega_mon_changed(&om, 3, 4));
    ASSERT_TRUE(!gen3_omega_mon_changed(&om, GEN3_SAVE_PARTY, 5));
    ASSERT_EQ(gen3_omega_changes(&om), 1);
    Gen3UbPieces pc;
    ASSERT_TRUE(gen3_ub_box_pieces(&om.slots, 3, 5, &pc));
    uint32_t want = 0;
    for(uint8_t k = 0; k < pc.n; k++) want |= 1u << pc.sector[k];
    ASSERT_EQ(om.dirty, want);
    /* the backup is unchanged, and the working copy differs only in the record */
    uint32_t size = 0;
    const uint8_t* b = fake_store_file(&store, BACKUP, &size);
    ASSERT_MEM_EQ(b, save_img, FAKE_UB_LEN);
    const uint8_t* w = fake_store_file(&store, GEN3_OMEGA_WORK, &size);
    uint32_t diff = 0;
    for(uint32_t i = 0; i < FAKE_UB_LEN; i++) diff += w[i] != save_img[i];
    ASSERT_EQ(diff, 1);
    /* the summary shows the working copy */
    Gen3UbSummary sm;
    ASSERT_EQ(gen3_omega_summary(&om, 3, 5, &sm), Gen3CartMonOk);
    ASSERT_EQ(sm.species, 1265);
}

TEST(test_omega_put_box_straddle) {
    ASSERT_TRUE(backed_up());
    uint8_t box = 0, slot = 0;
    Gen3UbPieces pc;
    bool found = false;
    for(box = 0; box < GEN3_UB_BOXES && !found; box++) {
        for(slot = 0; slot < GEN3_UB_BOX_SLOTS && !found; slot++) {
            found = gen3_ub_box_pieces(&om.slots, box, slot, &pc) && pc.n == 2;
        }
    }
    ASSERT_TRUE(found);
    box--;
    slot--;
    Gen3UbMon m;
    ASSERT_EQ(gen3_omega_mon_get(&om, box, slot, &m), Gen3CartMonEmpty);
    ASSERT_EQ(gen3_omega_owner(&om, &m), true);
    Gen3UbMon n;
    ASSERT_TRUE(gen3_ub_new_box_mon(&n, 1, 5, &m));
    ASSERT_EQ(gen3_omega_mon_put(&om, box, slot, &n, true), Gen3CartMonOk);
    ASSERT_EQ(bits(om.dirty), 2);
    ASSERT_EQ(om.dirty, (1u << pc.sector[0]) | (1u << pc.sector[1]));
    Gen3UbMon back;
    ASSERT_EQ(gen3_omega_mon_get(&om, box, slot, &back), Gen3CartMonOk);
    ASSERT_MEM_EQ(back.raw, n.raw, 58);
    ASSERT_TRUE(gen3_omega_mon_changed(&om, box, slot));
}

TEST(test_omega_put_party) {
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_EQ(gen3_omega_mon_get(&om, GEN3_SAVE_PARTY, 1, &m), Gen3CartMonOk);
    ASSERT_TRUE(m.party);
    ASSERT_EQ(gen3_ub_mon_species(&m), 1000);
    gen3_ub_mon_set_friendship(&m, 9);
    ASSERT_EQ(gen3_omega_mon_put(&om, GEN3_SAVE_PARTY, 1, &m, false), Gen3CartMonOk);
    Gen3UbMon back;
    ASSERT_EQ(gen3_omega_mon_get(&om, GEN3_SAVE_PARTY, 1, &back), Gen3CartMonOk);
    ASSERT_MEM_EQ(back.raw, m.raw, 100);
    ASSERT_TRUE(gen3_omega_mon_changed(&om, GEN3_SAVE_PARTY, 1));
    ASSERT_TRUE(!gen3_omega_mon_changed(&om, GEN3_SAVE_PARTY, 0));
    ASSERT_EQ(om.dirty, 1u << om.slots.phys[1]);
    /* a box record into a party slot, and a party record into a box slot */
    Gen3UbMon bx;
    ASSERT_EQ(gen3_omega_mon_get(&om, 3, 5, &bx), Gen3CartMonOk);
    ASSERT_EQ(gen3_omega_mon_put(&om, GEN3_SAVE_PARTY, 0, &bx, false), Gen3CartMonBadArg);
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 5, &m, false), Gen3CartMonBadArg);
    ASSERT_EQ(gen3_omega_mon_put(&om, 24, 0, &bx, false), Gen3CartMonBadArg);
}

TEST(test_omega_put_new_and_taken) {
    ASSERT_TRUE(backed_up());
    Gen3UbMon owner, occ, n;
    ASSERT_TRUE(gen3_omega_owner(&om, &owner));
    ASSERT_TRUE(gen3_ub_new_box_mon(&n, 1, 5, &owner));
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 6, &n, true), Gen3CartMonOk);
    ASSERT_TRUE(gen3_omega_mon_changed(&om, 3, 6));
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 6, &n, true), Gen3CartMonTaken);
    ASSERT_EQ(gen3_omega_mon_get(&om, 3, 5, &occ), Gen3CartMonOk);
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 5, &n, true), Gen3CartMonTaken);
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 7, &n, false), Gen3CartMonTaken);
    ASSERT_TRUE(!gen3_omega_mon_changed(&om, 3, 7));
    ASSERT_EQ(gen3_omega_changes(&om), 1);
    ASSERT_EQ(gen3_omega_mon_put(&om, GEN3_SAVE_PARTY, 2, &owner, false), Gen3CartMonBadArg);
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 30, &n, true), Gen3CartMonBadArg);
}

TEST(test_omega_put_refused_unverified) {
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_EQ(gen3_omega_mon_get(&om, 3, 5, &m), Gen3CartMonOk);
    gen3_ub_mon_set_friendship(&m, 77);
    om.verified = false;
    const uint32_t writes = store.writes;
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 5, &m, false), Gen3CartMonBadArg);
    ASSERT_EQ(store.writes, writes);
    ASSERT_EQ(om.dirty, 0);
    ASSERT_EQ(gen3_omega_changes(&om), 0);
}

TEST(test_omega_put_egg) {
    ASSERT_TRUE(setup());
    uint8_t nick[10], rec[100];
    gen3_text_encode("EGG", nick, 10, 10);
    fake_ub_party_rec(rec, 1, 5, nick, 1u << 30);
    fake_ub_put_party(save_img, 0, 0, rec);
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    Gen3UbMon m;
    /* a slot that holds an egg */
    ASSERT_EQ(gen3_omega_mon_get(&om, GEN3_SAVE_PARTY, 0, &m), Gen3CartMonOk);
    ASSERT_EQ(gen3_omega_mon_put(&om, GEN3_SAVE_PARTY, 0, &m, false), Gen3CartMonEgg);
    /* a record with the egg bit, into a normal slot */
    ASSERT_EQ(gen3_omega_mon_get(&om, GEN3_SAVE_PARTY, 1, &m), Gen3CartMonOk);
    m.raw[75] |= 0x40;
    ASSERT_EQ(gen3_omega_mon_put(&om, GEN3_SAVE_PARTY, 1, &m, false), Gen3CartMonEgg);
    Gen3UbMon b;
    ASSERT_EQ(gen3_omega_mon_get(&om, 3, 5, &b), Gen3CartMonOk);
    b.raw[57] |= 0x40;
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 5, &b, false), Gen3CartMonEgg);
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 6, &b, true), Gen3CartMonEgg);
    ASSERT_EQ(om.dirty, 0);
    ASSERT_EQ(gen3_omega_changes(&om), 0);
    uint32_t size = 0;
    ASSERT_MEM_EQ(fake_store_file(&store, GEN3_OMEGA_WORK, &size), save_img, FAKE_UB_LEN);
}

TEST(test_omega_owner) {
    ASSERT_TRUE(setup());
    uint8_t nick[10], rec[100];
    gen3_text_encode("X", nick, 10, 10);
    fake_ub_party_rec(rec, 1, 5, nick, 0);
    rec[4] = 1; /* another OT */
    fake_ub_put_party(save_img, 0, 0, rec);
    fake_ub_party_rec(rec, 1000, 5, nick, 0);
    const uint32_t id = 12345u | 54321u << 16;
    for(uint32_t i = 0; i < 4; i++) rec[4 + i] = (uint8_t)(id >> (8 * i));
    fake_ub_put_party(save_img, 0, 1, rec);
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    Gen3UbMon o;
    ASSERT_TRUE(gen3_omega_owner(&om, &o));
    ASSERT_TRUE(o.party);
    ASSERT_EQ(gen3_ub_mon_species(&o), 1000);
    /* no match: slot 0 */
    fake_ub_party_rec(rec, 1000, 5, nick, 0);
    rec[4] = 2;
    fake_ub_put_party(save_img, 0, 1, rec);
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    ASSERT_TRUE(gen3_omega_owner(&om, &o));
    ASSERT_EQ(gen3_ub_mon_species(&o), 1);
    /* eggs are skipped */
    fake_ub_party_rec(rec, 1, 5, nick, 1u << 30);
    fake_ub_put_party(save_img, 0, 0, rec);
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    ASSERT_TRUE(gen3_omega_owner(&om, &o));
    ASSERT_EQ(gen3_ub_mon_species(&o), 1000);
    fake_ub_party_rec(rec, 1000, 5, nick, 1u << 30);
    fake_ub_put_party(save_img, 0, 1, rec);
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    ASSERT_TRUE(!gen3_omega_owner(&om, &o));
}

TEST(test_omega_discard) {
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_EQ(gen3_omega_mon_get(&om, 3, 5, &m), Gen3CartMonOk);
    gen3_ub_mon_set_friendship(&m, 77);
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 5, &m, false), Gen3CartMonOk);
    ASSERT_EQ(gen3_omega_changes(&om), 1);
    ASSERT_EQ(gen3_omega_discard(&om), Gen3CartOk);
    ASSERT_EQ(gen3_omega_changes(&om), 0);
    ASSERT_EQ(om.dirty, 0);
    ASSERT_TRUE(om.work_ok);
    ASSERT_TRUE(!gen3_omega_mon_changed(&om, 3, 5));
    uint32_t size = 0;
    const uint8_t* w = fake_store_file(&store, GEN3_OMEGA_WORK, &size);
    ASSERT_EQ(size, FAKE_UB_LEN);
    ASSERT_MEM_EQ(w, save_img, FAKE_UB_LEN);
}

TEST(test_omega_edits_dropped_on_reconnect) {
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_EQ(gen3_omega_mon_get(&om, 3, 5, &m), Gen3CartMonOk);
    ASSERT_EQ(gen3_omega_mon_put(&om, 3, 5, &m, false), Gen3CartMonOk);
    ASSERT_TRUE(om.dirty != 0);
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_EQ(om.dirty, 0);
    ASSERT_EQ(gen3_omega_changes(&om), 0);
    ASSERT_TRUE(!om.work_ok);
}

/* ---- The write ---- */

#define STAMP2 "20261101-130000"
#define BACKUP2 "backups/UNBOUND_12345_" STAMP2 ".sav"
#define SEC 4096u
#define WRITE_BLOCKS 120u /* the write test's 8, then the new slot's 14 sectors of 8 */

static uint8_t card_before[FAKE_UB_LEN], card_after[FAKE_UB_LEN], work_before[FAKE_UB_LEN];

/* The save file as the card holds it */
static void card_save(uint8_t* out) {
    for(uint32_t off = 0; off < FAKE_UB_LEN; off += 512u) {
        fake_fat_block(&card, fake_fat_file_lba(&card, om.save.first_cluster, off), out + off);
    }
}

/* The slots of a save image, as the game picks them */
static void image_slots(const uint8_t* img, Gen3UbSlots* s) {
    static Gen3UbSector infos[28];
    for(uint32_t k = 0; k < 28u; k++) gen3_ub_sector_info(img + k * SEC, &infos[k]);
    gen3_ub_slots(infos, s);
}

/* Box record (box, slot) of the image's loaded slot */
static bool
    image_box(const uint8_t* img, const Gen3UbSlots* s, uint8_t box, uint8_t slot, uint8_t* rec) {
    Gen3UbPieces pc;
    if(!gen3_ub_box_pieces(s, box, slot, &pc)) return false;
    uint32_t at = 0;
    for(uint8_t k = 0; k < pc.n; k++) {
        memcpy(rec + at, img + pc.sector[k] * SEC + pc.off[k], pc.len[k]);
        at += pc.len[k];
    }
    return true;
}

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;
}

/* Box 3 slot 5's friendship set to f in the working copy; *m gets the record put */
static bool edit_box3(uint8_t f, Gen3UbMon* m) {
    if(gen3_omega_mon_get(&om, 3, 5, m) != Gen3CartMonOk) return false;
    gen3_ub_mon_set_friendship(m, f);
    return gen3_omega_mon_put(&om, 3, 5, m, false) == Gen3CartMonOk;
}

/* A new Pokemon into the open box slot (box, slot); *m gets the record put */
static bool put_new(uint8_t box, uint8_t slot, Gen3UbMon* m) {
    Gen3UbMon owner;
    return gen3_omega_owner(&om, &owner) && gen3_ub_new_box_mon(m, 1, 5, &owner) &&
           gen3_omega_mon_put(&om, box, slot, m, true) == Gen3CartMonOk;
}

/* A box slot whose record lies across sectors 30 and 31 */
static bool straddle_30_31(uint8_t* box, uint8_t* slot) {
    for(uint8_t b = 0; b < GEN3_UB_BOXES; b++) {
        for(uint8_t k = 0; k < GEN3_UB_BOX_SLOTS; k++) {
            Gen3UbPieces pc;
            if(gen3_ub_box_pieces(&om.slots, b, k, &pc) && pc.n == 2 && pc.sector[0] == 30 &&
               pc.sector[1] == 31) {
                *box = b;
                *slot = k;
                return true;
            }
        }
    }
    return false;
}

TEST(test_omega_write_ok) {
    /* Both signatures in the loaded slot, and a section 4 checksum that isn't the sum of its data
     * (the game doesn't check it as the others; PUSE never recomputes it): each new sector takes
     * its signature from the loaded one, and section 4 its checksum */
    fake_store_init(&store, 64u * MIB);
    api = fake_store_api(&store);
    make_save();
    for(uint8_t id = 2; id < 14u; id += 5u) {
        save_img[fake_ub_phys(save_img, 0, id) * SEC + 0xFF8] = 0x98;
    }
    save_img[fake_ub_phys(save_img, 0, 4) * SEC + 0xFF6] ^= 0x55;
    ASSERT_TRUE(default_bench());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
    ASSERT_EQ(om.slots.loaded, 0);
    ASSERT_EQ(om.infos[om.slots.phys[7]].signature, GEN3_UB_SIG_B);
    Gen3UbMon bx, pt;
    ASSERT_TRUE(edit_box3(77, &bx));
    ASSERT_EQ(gen3_omega_mon_get(&om, GEN3_SAVE_PARTY, 0, &pt), Gen3CartMonOk);
    gen3_ub_mon_set_level(&pt, 13);
    ASSERT_EQ(gen3_omega_mon_put(&om, GEN3_SAVE_PARTY, 0, &pt, false), Gen3CartMonOk);
    ASSERT_EQ(gen3_omega_changes(&om), 2);
    uint32_t size = 0;
    memcpy(work_before, fake_store_file(&store, GEN3_OMEGA_WORK, &size), FAKE_UB_LEN);
    const Gen3UbSlots old = om.slots;
    card_save(card_before);
    const uint64_t digest = fake_fat_digest_except(&card, om.save.first_cluster);
    /* a write is never cancelled */
    cancel = true;
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartOk);

    card_save(card_after);
    Gen3UbSlots s;
    image_slots(card_after, &s);
    ASSERT_EQ(s.loaded, 1);
    ASSERT_EQ(s.counter[1], 21);
    ASSERT_TRUE(s.valid[0]);
    ASSERT_EQ(s.counter[0], 20);
    /* slot N, and the sectors 28..31, as they were */
    ASSERT_MEM_EQ(card_after, card_before, 14u * SEC);
    ASSERT_MEM_EQ(card_after + 28u * SEC, card_before + 28u * SEC, 4u * SEC);
    /* each section of the new slot: the working copy's data, its footer N + 1 */
    for(uint8_t id = 0; id < 14u; id++) {
        const uint8_t* n = card_after + s.phys[id] * SEC;
        const uint8_t* w = work_before + old.phys[id] * SEC;
        ASSERT_MEM_EQ(n, w, 0xFF4u);
        ASSERT_EQ(le16(n + 0xFF4), id);
        ASSERT_EQ(le32(n + 0xFF8), le32(w + 0xFF8));
        ASSERT_EQ(le32(n + 0xFFC), 21);
    }
    /* section 4's checksum: the loaded slot's, as it was; the others' the sum over their length */
    const uint8_t* n4 = card_after + s.phys[4] * SEC;
    ASSERT_EQ(le16(n4 + 0xFF6), le16(card_before + old.phys[4] * SEC + 0xFF6));
    ASSERT_TRUE(le16(n4 + 0xFF6) != gen3_ub_section_sum(n4, 4));
    ASSERT_EQ(le32(card_after + s.phys[7] * SEC + 0xFF8), GEN3_UB_SIG_B);
    ASSERT_EQ(le32(card_after + s.phys[8] * SEC + 0xFF8), GEN3_UB_SIG_A);
    /* the edits, read back from the card */
    uint8_t rec[58];
    ASSERT_TRUE(image_box(card_after, &s, 3, 5, rec));
    ASSERT_MEM_EQ(rec, bx.raw, 58);
    ASSERT_MEM_EQ(card_after + s.phys[1] * SEC + GEN3_UB_PARTY_OFF(0), pt.raw, 100);
    ASSERT_EQ(card_after[s.phys[1] * SEC + GEN3_UB_PARTY_OFF(0) + 84u], 13);
    /* nothing outside the save's own blocks */
    ASSERT_EQ(fake_fat_digest_except(&card, om.save.first_cluster), digest);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_WRITE_SECTOR], 15);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_ARM], 1);
    ASSERT_EQ(gba.omega_writes, WRITE_BLOCKS);

    /* the new backup: the card as written; the old one kept */
    const uint8_t* b = fake_store_file(&store, BACKUP2, &size);
    ASSERT_TRUE(b != NULL);
    ASSERT_MEM_EQ(b, card_after, FAKE_UB_LEN);
    ASSERT_MEM_EQ(fake_store_file(&store, BACKUP, &size), card_before, FAKE_UB_LEN);
    ASSERT_STR_EQ(om.backup_name, BACKUP2);
    ASSERT_TRUE(gen3_omega_browsable(&om));
    ASSERT_EQ(om.slots.loaded, 1);
    ASSERT_EQ(om.slots.counter[1], 21);
    ASSERT_EQ(gen3_omega_changes(&om), 0);
    ASSERT_EQ(om.dirty, 0);
    ASSERT_TRUE(om.work_ok);
    ASSERT_MEM_EQ(fake_store_file(&store, GEN3_OMEGA_WORK, &size), card_after, FAKE_UB_LEN);
    ASSERT_TRUE(om.connected);
    ASSERT_TRUE(absent(GEN3_CART_BACKUP_TMP));

    /* progress: Prepare, Program and Verify per sector, then the new backup as PostBackup */
    ASSERT_TRUE(prog.calls[Gen3CartStepPrepare] > 0);
    ASSERT_EQ(prog.calls[Gen3CartStepProgram], 15);
    ASSERT_EQ(prog.last_done[Gen3CartStepProgram], 15);
    ASSERT_EQ(prog.last_done[Gen3CartStepVerify], 32);
    ASSERT_TRUE(prog.calls[Gen3CartStepPostBackup] > 0);
    ASSERT_EQ(prog.last_done[Gen3CartStepPostBackup], 3u * 32u + 1u);
    ASSERT_EQ(prog.calls[Gen3CartStepBackupRead], 32); /* the first backup's only */

    /* a second write, from the new backup */
    ASSERT_TRUE(edit_box3(78, &bx));
    ASSERT_EQ(gen3_omega_write(&om, "20261101-140000"), Gen3CartOk);
    card_save(card_after);
    image_slots(card_after, &s);
    ASSERT_EQ(s.loaded, 0);
    ASSERT_EQ(s.counter[0], 22);
    ASSERT_TRUE(image_box(card_after, &s, 3, 5, rec));
    ASSERT_MEM_EQ(rec, bx.raw, 58);
}

TEST(test_omega_write_rotation) {
    /* (the loaded slot, its rotation): id 0 of the new slot at index (r_loaded + 1) % 14 */
    static const uint8_t cases[][2] = {{0, 5}, {0, 1}, {1, 13}, {1, 0}};
    for(uint32_t i = 0; i < 4u; i++) {
        const uint8_t slot = cases[i][0], rotate = cases[i][1];
        fake_store_init(&store, 64u * MIB);
        api = fake_store_api(&store);
        make_save_at(slot, 20, rotate);
        ASSERT_TRUE(default_bench());
        ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
        ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
        ASSERT_EQ(om.slots.loaded, slot);
        Gen3UbMon m;
        ASSERT_TRUE(edit_box3(77, &m));
        card_save(card_before);
        ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartOk);
        card_save(card_after);
        Gen3UbSlots s;
        image_slots(card_after, &s);
        const uint8_t nw = (uint8_t)(1u - slot);
        ASSERT_EQ(s.loaded, nw);
        ASSERT_EQ(s.counter[nw], 21);
        const uint32_t r_loaded = (14u - rotate) % 14u;
        const uint32_t r = (r_loaded + 1u) % 14u;
        ASSERT_EQ(s.phys[0], nw * 14u + r);
        for(uint32_t k = 0; k < 14u; k++) {
            ASSERT_EQ(le16(card_after + (nw * 14u + k) * SEC + 0xFF4), (k + 14u - r) % 14u);
        }
        ASSERT_MEM_EQ(card_after + slot * 14u * SEC, card_before + slot * 14u * SEC, 14u * SEC);
        uint8_t rec[58];
        ASSERT_TRUE(image_box(card_after, &s, 3, 5, rec));
        ASSERT_MEM_EQ(rec, m.raw, 58);
    }
}

TEST(test_omega_write_changed) {
    /* the game saved after the backup: a byte of the save's sector 20 */
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_TRUE(edit_box3(77, &m));
    const uint32_t off = 20u * SEC + 600u;
    const uint8_t x = 0x00;
    fake_fat_poke(&card, fake_fat_file_lba(&card, om.save.first_cluster, off), off % 512u, &x, 1);
    card_save(card_before);
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartChanged);
    ASSERT_EQ(gba.omega_writes, 0);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_ARM], 0);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_WRITE_SECTOR], 0);
    ASSERT_TRUE(!om.backup_current);
    ASSERT_TRUE(!gen3_omega_browsable(&om));
    ASSERT_EQ(gen3_omega_changes(&om), 0);
    card_save(card_after);
    ASSERT_MEM_EQ(card_after, card_before, FAKE_UB_LEN);
    /* the file isn't the one the connect found: another first cluster. Changed before any CRC. */
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_box3(77, &m));
    om.save.first_cluster++;
    const uint32_t crcs = hook.calls[GEN3_HCMD_OMEGA_CRC32];
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartChanged);
    ASSERT_TRUE(hook.calls[GEN3_HCMD_OMEGA_OPEN] > 0);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_CRC32], crcs);
    ASSERT_EQ(gba.omega_writes, 0);
    ASSERT_TRUE(!om.backup_current);
}

TEST(test_omega_write_refused_unverified) {
    ASSERT_TRUE(backed_up());
    const uint32_t t0 = fake_helper_transfers(&fh);
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartNothing);
    Gen3UbMon m;
    ASSERT_TRUE(edit_box3(77, &m));
    om.verified = false;
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartRefused);
    om.verified = true;
    om.work_ok = false;
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartRefused);
    om.work_ok = true;
    om.backup_current = false;
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartRefused);
    om.backup_current = true;
    om.connected = false;
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartLinkLost);
    om.connected = true;
    ASSERT_EQ(gen3_omega_write(&om, NULL), Gen3CartBadArg);
    ASSERT_EQ(gen3_omega_write(&om, "20261101-120000-12345"), Gen3CartBadArg); /* 21 characters */
    fake_store_report_free(&store, FAKE_UB_LEN);
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartSdFull);
    /* none of them sent anything */
    ASSERT_EQ(fake_helper_transfers(&fh), t0);
    ASSERT_EQ(gba.omega_writes, 0);
    ASSERT_EQ(gen3_omega_changes(&om), 1);
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartOk);
}

TEST(test_omega_write_cut_sweep) {
    /* Each bench starts from a store that has the game file's cache line: no hash */
    static FakeStore warm;
    ASSERT_TRUE(setup());
    ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
    fake_store_copy(&warm, &store);
    for(uint32_t k = 0; k <= WRITE_BLOCKS; k++) {
        fake_store_copy(&store, &warm);
        ASSERT_TRUE(default_bench());
        ASSERT_EQ(gen3_omega_connect(&om), Gen3CartOk);
        ASSERT_TRUE(om.from_cache);
        ASSERT_EQ(gen3_omega_backup(&om, STAMP), Gen3CartOk);
        Gen3UbMon m;
        ASSERT_TRUE(edit_box3(77, &m));
        ASSERT_EQ(om.dirty >> 28, 0); /* the new slot is all a write puts on the card */
        card_save(card_before);
        const uint64_t digest = fake_fat_digest_except(&card, om.save.first_cluster);
        fake_gba_omega_cut_after(&gba, k);
        const Gen3CartResult r = gen3_omega_write(&om, STAMP2);
        card_save(card_after);
        Gen3UbSlots s;
        image_slots(card_after, &s);
        ASSERT_EQ(fake_fat_digest_except(&card, om.save.first_cluster), digest);
        ASSERT_MEM_EQ(card_after + 28u * SEC, card_before + 28u * SEC, 4u * SEC);
        if(k < WRITE_BLOCKS) {
            ASSERT_EQ(r, Gen3CartFlashError);
            /* it stopped at the sector the cut came in */
            ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_WRITE_SECTOR], k / 8u + 1u);
            ASSERT_EQ(s.loaded, 0);
            ASSERT_TRUE(!s.valid[1]);
            ASSERT_EQ(s.counter[0], 20);
            ASSERT_MEM_EQ(card_after, card_before, 14u * SEC);
            /* the edits stay */
            ASSERT_EQ(gen3_omega_changes(&om), 1);
            ASSERT_TRUE(om.connected);
        } else {
            ASSERT_EQ(r, Gen3CartOk);
            ASSERT_EQ(s.loaded, 1);
            ASSERT_EQ(s.counter[1], 21);
            ASSERT_MEM_EQ(card_after, card_before, 14u * SEC);
        }
    }
    fake_store_init(&warm, 0);
}

TEST(test_omega_write_link_lost) {
    /* the cable out once the new slot's 4th sector is written: its answer never comes */
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_TRUE(edit_box3(77, &m));
    card_save(card_before);
    hook.unplug_write_at = 5;
    const Gen3CartResult r = gen3_omega_write(&om, STAMP2);
    ASSERT_TRUE(r == Gen3CartLinkLost || r == Gen3CartBusyTimeout);
    ASSERT_TRUE(!om.connected);
    card_save(card_after);
    Gen3UbSlots s;
    image_slots(card_after, &s);
    ASSERT_EQ(s.loaded, 0);
    ASSERT_TRUE(!s.valid[1]);
    ASSERT_MEM_EQ(card_after, card_before, 14u * SEC);
    /* the edits stay until the next Connect; nothing more is sent before one */
    ASSERT_TRUE(gen3_omega_browsable(&om));
    ASSERT_EQ(gen3_omega_changes(&om), 1);
    const uint32_t t0 = fake_helper_transfers(&fh);
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(fake_helper_transfers(&fh), t0);
}

TEST(test_omega_write_shared) {
    /* box 21 lies in shared sector 31 only */
    ASSERT_TRUE(backed_up());
    Gen3UbMon n;
    ASSERT_TRUE(put_new(21, 0, &n));
    ASSERT_EQ(om.dirty, 1u << 31);
    card_save(card_before);
    const uint64_t digest = fake_fat_digest_except(&card, om.save.first_cluster);
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartOk);
    card_save(card_after);
    Gen3UbSlots s;
    image_slots(card_after, &s);
    ASSERT_EQ(s.loaded, 1);
    ASSERT_EQ(s.counter[1], 21);
    uint8_t rec[58];
    ASSERT_TRUE(image_box(card_after, &s, 21, 0, rec));
    ASSERT_MEM_EQ(rec, n.raw, 58);
    ASSERT_MEM_EQ(card_after + 30u * SEC, card_before + 30u * SEC, SEC);
    uint32_t diff = 0;
    for(uint32_t i = 31u * SEC; i < 32u * SEC; i++) diff += card_after[i] != card_before[i];
    ASSERT_TRUE(diff > 0 && diff <= 58u);
    ASSERT_MEM_EQ(card_after, card_before, 14u * SEC);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_WRITE_SECTOR], 16);
    ASSERT_EQ(gba.omega_writes, WRITE_BLOCKS + 8u);
    ASSERT_EQ(prog.last_done[Gen3CartStepProgram], 16);
    ASSERT_EQ(fake_fat_digest_except(&card, om.save.first_cluster), digest);
    ASSERT_EQ(gen3_omega_changes(&om), 0);

    /* a record across sectors 30 and 31: both written */
    ASSERT_TRUE(backed_up());
    uint8_t box = 0, slot = 0;
    ASSERT_TRUE(straddle_30_31(&box, &slot));
    ASSERT_TRUE(put_new(box, slot, &n));
    ASSERT_EQ(om.dirty, 3u << 30);
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartOk);
    card_save(card_after);
    image_slots(card_after, &s);
    ASSERT_EQ(s.loaded, 1);
    ASSERT_TRUE(image_box(card_after, &s, box, slot, rec));
    ASSERT_MEM_EQ(rec, n.raw, 58);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_WRITE_SECTOR], 17);
}

TEST(test_omega_write_cut_shared) {
    static const char want[] = "Written, but boxes\n20-23 may be\nwrong: Restore.";
    ASSERT_STR_EQ(gen3_cart_result_text(Gen3CartWrittenShared), want);
    /* a cut during sector 31: the new slot loads */
    for(uint32_t flip = 0; flip < 2u; flip++) {
        ASSERT_TRUE(backed_up());
        Gen3UbMon n;
        ASSERT_TRUE(put_new(21, 0, &n));
        card_save(card_before);
        if(flip) {
            /* a bit flipped in sector 31's third block: its read-back differs */
            fake_gba_omega_flip_after(&gba, WRITE_BLOCKS + 2u);
        } else {
            fake_gba_omega_cut_after(&gba, WRITE_BLOCKS + 3u);
        }
        ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartWrittenShared);
        card_save(card_after);
        Gen3UbSlots s;
        image_slots(card_after, &s);
        ASSERT_EQ(s.loaded, 1);
        ASSERT_TRUE(s.valid[1]);
        ASSERT_EQ(s.counter[1], 21);
        ASSERT_MEM_EQ(card_after, card_before, 14u * SEC);
        ASSERT_MEM_EQ(card_after + 30u * SEC, card_before + 30u * SEC, SEC);
        /* the backup no longer holds the save the game loads: a new one first */
        ASSERT_TRUE(!om.backup_current);
        ASSERT_EQ(gen3_omega_changes(&om), 0);
        ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_WRITE_SECTOR], 16);
    }
}

TEST(test_omega_write_verify_fail) {
    /* a bit flipped in the first block of the new slot's 4th sector */
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_TRUE(edit_box3(77, &m));
    card_save(card_before);
    fake_gba_omega_flip_after(&gba, 8u + 3u * 8u);
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartVerifyFailed);
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_WRITE_SECTOR], 5);
    card_save(card_after);
    Gen3UbSlots s;
    image_slots(card_after, &s);
    ASSERT_EQ(s.loaded, 0);
    ASSERT_EQ(s.counter[0], 20);
    ASSERT_TRUE(!s.valid[1]);
    ASSERT_MEM_EQ(card_after, card_before, 14u * SEC);
    ASSERT_TRUE(om.connected);
    ASSERT_EQ(gen3_omega_changes(&om), 1);
}

TEST(test_omega_write_work_corrupt) {
    /* the working copy reads back wrong in a section with no edit (7): nothing of it is written */
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_TRUE(edit_box3(77, &m));
    ASSERT_TRUE(((om.dirty >> om.slots.phys[7]) & 1u) == 0);
    card_save(card_before);
    fake_store_corrupt_read(&store, GEN3_OMEGA_WORK, om.slots.phys[7] * SEC + 9u);
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartSdError);
    fake_store_corrupt_read(&store, NULL, 0);
    card_save(card_after);
    Gen3UbSlots s;
    image_slots(card_after, &s);
    ASSERT_EQ(s.loaded, 0);
    ASSERT_TRUE(!s.valid[1]);
    ASSERT_MEM_EQ(card_after, card_before, 14u * SEC);
    /* section 7 is at the new slot's index 8 (r = 1): the write test and indices 0..7 went out */
    ASSERT_EQ(hook.calls[GEN3_HCMD_OMEGA_WRITE_SECTOR], 9);
    ASSERT_TRUE(om.connected);
    ASSERT_EQ(gen3_omega_changes(&om), 1);
}

TEST(test_omega_write_no_backup) {
    /* the card is written and verified, but the new backup can't be stored */
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_TRUE(edit_box3(77, &m));
    fake_store_fail_write_after(&store, 0);
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartWrittenNoBackup);
    card_save(card_after);
    Gen3UbSlots s;
    image_slots(card_after, &s);
    ASSERT_EQ(s.loaded, 1);
    ASSERT_EQ(s.counter[1], 21);
    uint8_t rec[58];
    ASSERT_TRUE(image_box(card_after, &s, 3, 5, rec));
    ASSERT_MEM_EQ(rec, m.raw, 58);
    ASSERT_TRUE(!om.backup_current);
    ASSERT_EQ(gen3_omega_changes(&om), 0);
    ASSERT_EQ(prog.last_done[Gen3CartStepVerify], 32);
    ASSERT_TRUE(absent(BACKUP2));
    ASSERT_TRUE(absent(GEN3_CART_BACKUP_TMP));
}

TEST(test_omega_write_verify_misplaced) {
    /* the last WRITE_SECTOR also changed a byte of the loaded slot (sector 5): the Verify step
     * finds it */
    ASSERT_TRUE(backed_up());
    Gen3UbMon m;
    ASSERT_TRUE(edit_box3(77, &m));
    hook.poke_write_at = 15;
    hook.poke_off = 5u * SEC + 100u;
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartVerifyFailed);
    ASSERT_EQ(prog.calls[Gen3CartStepPostBackup], 0);
    ASSERT_TRUE(!om.backup_current);
    ASSERT_STR_EQ(om.backup_name, BACKUP);
    ASSERT_TRUE(absent(BACKUP2));
    /* sector 30 changed, when only 31 was written: WrittenShared */
    ASSERT_TRUE(backed_up());
    Gen3UbMon n;
    ASSERT_TRUE(put_new(21, 0, &n));
    hook.poke_write_at = 16;
    hook.poke_off = 30u * SEC + 100u;
    ASSERT_EQ(gen3_omega_write(&om, STAMP2), Gen3CartWrittenShared);
    ASSERT_TRUE(!om.backup_current);
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
    RUN(test_omega_work_copy);
    RUN(test_omega_work_copy_fails);
    RUN(test_omega_put_box);
    RUN(test_omega_put_box_straddle);
    RUN(test_omega_put_party);
    RUN(test_omega_put_new_and_taken);
    RUN(test_omega_put_refused_unverified);
    RUN(test_omega_put_egg);
    RUN(test_omega_owner);
    RUN(test_omega_discard);
    RUN(test_omega_edits_dropped_on_reconnect);
    RUN(test_omega_write_ok);
    RUN(test_omega_write_rotation);
    RUN(test_omega_write_changed);
    RUN(test_omega_write_refused_unverified);
    RUN(test_omega_write_cut_sweep);
    RUN(test_omega_write_link_lost);
    RUN(test_omega_write_shared);
    RUN(test_omega_write_cut_shared);
    RUN(test_omega_write_verify_fail);
    RUN(test_omega_write_work_corrupt);
    RUN(test_omega_write_no_backup);
    RUN(test_omega_write_verify_misplaced);
    fake_fat_free(&card);
    free(rom);
    TEST_EXIT();
}
