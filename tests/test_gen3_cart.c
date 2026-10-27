#include "test.h"
#include "bulk_file.h"
#include "fake_gba.h"
#include "fake_helper.h"
#include "fake_save.h"
#include "fake_store.h"
#include "gen3_golden.h"
#include "helper_cmd.h"
#include <stdbool.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_hlink.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_save.h>

/* The cart jobs through the client and the real packet pump against the helper's real core on the
 * fake GBA (fake_helper), with a RAM store. The default cart: English Ruby (AXVE version 0), 1 MiB
 * of ROM with FLASH1M_V103, an MX chip. Its save: slot A with counter 4 and id 0 at its first
 * sector, slot B absent; trainer BRENDAN 12345/54321; a party of 3 (the golden Treecko and two
 * edits of it); box 0 named BOX 1; the golden Treecko at box 1 slot 19. The store holds 64 MiB. */

#define MIB 0x100000u
#define KIB 1024u
#define SECTOR_LEN 4096u
#define WORK_LEN (14u * SECTOR_LEN)
#define MX 0x09C2u
#define FOREVER 10000000u /* an unplug longer than any test */
#define LIB_OFF 0xB0000u /* FLASH1M_V103 in the ROM */
#define LIB_STRING "FLASH1M_V103"
#define STAMP "20261025-143005"
#define BACKUP_NAME "backups/AXVE_12345_" STAMP ".sav"
#define ROM_NAME "roms/AXVE_v0.gba"
#define PART_NAME ROM_NAME ".part"
#define REGION GEN3_CART_ROM_REGION
#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

static FakeGba gba;
static FakeHelper fh;
static Gen3Hlink pump;
static Gen3Hcli cli;
static FakeStore store;
static Gen3Store api;
static Gen3Cart cart;
static uint8_t buf[GEN3_CART_BUF_SIZE];
static FakeSaveSpec spec;
static uint8_t img[FAKE_SAVE_LEN];
static uint8_t img2[FAKE_SAVE_LEN];
static uint8_t hdr[0xC0];
static uint8_t party[3][100];
static uint8_t chunk[SECTOR_LEN];
static Gen3Editor ed;
static volatile bool cancel;

/* ---- Observers ---- */

/* The progress the jobs report, and what a test does at a step */
typedef struct {
    uint32_t calls[Gen3CartStepRomVerify + 1];
    uint32_t last_done[Gen3CartStepRomVerify + 1];
    uint32_t last_total[Gen3CartStepRomVerify + 1];
    Gen3CartStep cancel_step; /* None: never */
    uint32_t cancel_at; /* cancel once done reaches this */
    Gen3CartStep unplug_step; /* None: never */
    uint32_t unplug_at; /* the cable out for good once done reaches this */
} Prog;

static Prog prog;

static void prog_cb(void* ctx, Gen3CartStep step, uint32_t done, uint32_t total) {
    Prog* p = ctx;
    p->calls[step]++;
    p->last_done[step] = done;
    p->last_total[step] = total;
    if(p->cancel_step != Gen3CartStepNone && step == p->cancel_step && done >= p->cancel_at) {
        cancel = true;
    }
    if(p->unplug_step != Gen3CartStepNone && step == p->unplug_step && done >= p->unplug_at) {
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), FOREVER);
        p->unplug_step = Gen3CartStepNone;
    }
}

/* The fake helper's command hook: counts each code; after READ_SECTOR of flip_sector, flips a
 * byte of that sector in the flash; when unplug_code runs, the cable out for unplug_len
 * transfers; when slow_code runs, the helper busy slow_us longer; after the glitch_after_crc-th
 * ROM_CRC32, a ROM glitch at glitch_off. */
typedef struct {
    uint32_t calls[256];
    bool flip;
    uint8_t flip_sector;
    uint8_t unplug_code;
    uint32_t unplug_len;
    uint8_t slow_code;
    uint32_t slow_us;
    uint32_t glitch_after_crc;
    uint32_t glitch_off;
} Hook;

static Hook hook;

static void hook_cb(void* ctx, uint8_t code, uint16_t arg0) {
    Hook* k = ctx;
    k->calls[code]++;
    if(k->slow_code && code == k->slow_code) {
        k->slow_code = 0;
        fh.busy_end_us += k->slow_us;
    }
    if(k->flip && code == GEN3_HCMD_READ_SECTOR && arg0 == k->flip_sector) {
        k->flip = false;
        fake_gba_flash(&gba)[k->flip_sector * SECTOR_LEN + 0x20] ^= 0x01;
    }
    if(k->unplug_code && code == k->unplug_code) {
        k->unplug_code = 0;
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), k->unplug_len);
    }
    if(k->glitch_after_crc && code == GEN3_HCMD_ROM_CRC32 &&
       k->calls[code] == k->glitch_after_crc) {
        k->glitch_after_crc = 0;
        fake_gba_rom_glitch_once(&gba, k->glitch_off);
    }
}

/* A transport around the fake helper's that counts syncs and exchanges, and can answer fake_code
 * itself, without the helper, with fake_status and fake_n payload words (fail code 7, offset 0) */
typedef struct {
    Gen3HTransport inner;
    uint32_t syncs;
    uint32_t exchanges;
    uint8_t fake_code;
    uint8_t fake_status;
    uint16_t fake_n;
} Tap;

static Tap tap;

static Gen3HlinkFail
    tap_exchange(void* ctx, const Gen3HlinkReq* req, uint8_t* status, uint16_t* resp_words) {
    Tap* t = ctx;
    t->exchanges++;
    if(t->fake_code && req->code == t->fake_code) {
        memset(req->resp, 0, 2u * t->fake_n);
        req->resp[0] = 7;
        *status = t->fake_status;
        *resp_words = t->fake_n;
        return Gen3HlinkFailNone;
    }
    return t->inner.exchange(t->inner.ctx, req, status, resp_words);
}

static bool tap_sync(void* ctx, uint8_t need, uint32_t timeout_us) {
    Tap* t = ctx;
    t->syncs++;
    return t->inner.sync(t->inner.ctx, need, timeout_us);
}

/* ---- Fixtures ---- */

static uint8_t* flash(void) {
    return fake_gba_flash(&gba);
}

static uint8_t phys(uint8_t slot, uint8_t id) {
    return (uint8_t)fake_save_phys(&spec, slot, id);
}

/* The default save in img; the party edits go through the editor */
static bool default_save(void) {
    spec = (FakeSaveSpec){
        .game = FakeGameRS,
        .slot_present = {true, false},
        .counter = {4, 0},
        .first = {0, 0},
        .seed = 0x5EED0009u,
        .no_mons = true,
    };
    fake_save_build(img, &spec);
    fake_save_put_trainer(img, &spec, 0, "BRENDAN", 0, 12345, 54321);
    golden_game(party[0]);
    Gen3Pkm p;
    if(!gen3_save_mon_to_pkm(party[0], 100, &p)) return false;
    if(!gen3_edit_init(&ed, 1) || !gen3_edit_load(&ed, &p)) return false;
    gen3_edit_set_level(&ed, 30);
    gen3_save_pkm_to_mon(&ed.pkm, 100, party[1]);
    if(!gen3_edit_set_nickname(&ed, "LEAFY")) return false;
    gen3_save_pkm_to_mon(&ed.pkm, 100, party[2]);
    fake_save_put_party(img, &spec, 0, 3, (const uint8_t(*)[100])party);
    fake_save_put_box_name(img, &spec, 0, 0, "BOX 1");
    fake_save_put_box_mon(img, &spec, 0, 1, 19, party[0]);
    return true;
}

/* Inserts a cart with img on its save chip; a fresh helper, transport, client, store and cart. */
static void insert(const char* code, uint32_t rom_size, FakeSaveKind kind) {
    fake_gba_init(&gba);
    helper_cmd_header(hdr, gba.own, "POKEMON RUBY", code, 0);
    fake_gba_rom(&gba, hdr, rom_size);
    fake_gba_rom_string(&gba, LIB_OFF, LIB_STRING);
    fake_gba_save(&gba, kind, kind == FakeSaveFlash ? MX : 0);
    memcpy(flash(), img, sizeof(img));
    fake_helper_init(&fh, &gba);
    memset(&hook, 0, sizeof(hook));
    fake_helper_on_command(&fh, hook_cb, &hook);
    memset(&tap, 0, sizeof(tap));
    tap.inner = fake_helper_transport(&fh, &pump);
    const Gen3HTransport t = {
        .ctx = &tap,
        .exchange = tap_exchange,
        .sync = tap_sync,
    };
    gen3_hcli_init(&cli, &t);
    fake_store_init(&store, 64u * MIB);
    api = fake_store_api(&store);
    memset(&prog, 0, sizeof(prog));
    cancel = false;
    const Gen3CartProgress p = {
        .cb = prog_cb,
        .ctx = &prog,
        .cancel = &cancel,
    };
    gen3_cart_init(&cart, &cli, &api, buf, &p);
}

static bool setup(void) {
    if(!default_save()) return false;
    insert("AXVE", MIB, FakeSaveFlash);
    return true;
}

static bool connected(void) {
    return setup() && gen3_cart_connect(&cart) == Gen3CartOk;
}

static bool backed_up(void) {
    return connected() && gen3_cart_backup(&cart, STAMP) == Gen3CartOk;
}

static const uint8_t* file(const char* name, uint32_t* size) {
    *size = 0;
    return fake_store_file(&store, name, size);
}

/* The working copy holds the given sectors in id order */
static bool work_is(const uint8_t* flash_img, const FakeSaveSpec* s, uint8_t slot) {
    uint32_t size;
    const uint8_t* w = file(GEN3_CART_WORK_NAME, &size);
    if(!w || size != WORK_LEN) return false;
    for(uint8_t k = 0; k < 14; k++) {
        const uint32_t at = fake_save_phys(s, slot, k) * SECTOR_LEN;
        if(memcmp(w + k * SECTOR_LEN, flash_img + at, SECTOR_LEN) != 0) return false;
    }
    return true;
}

/* Every sector of the working copy but id k equals slot A's */
static bool work_differs_only_in(uint8_t only) {
    uint32_t size;
    const uint8_t* w = file(GEN3_CART_WORK_NAME, &size);
    if(!w || size != WORK_LEN) return false;
    for(uint8_t k = 0; k < 14; k++) {
        const bool same =
            memcmp(w + k * SECTOR_LEN, flash() + phys(0, k) * SECTOR_LEN, SECTOR_LEN) == 0;
        if(same == (k == only)) return false;
    }
    return true;
}

/* The .sav named holds the fake flash as it is, and no temporary backup is left */
static bool sav_is_flash(const char* name) {
    uint32_t size;
    const uint8_t* s = file(name, &size);
    return s && size == FAKE_SAVE_LEN && memcmp(s, flash(), FAKE_SAVE_LEN) == 0 &&
           fake_store_count(&store, ".tmp") == 0;
}

/* The fake ROM's byte at off (fake_gba.h): the header, the library string, else the pattern */
static uint8_t rom_byte(uint32_t off) {
    if(off < sizeof(hdr)) return hdr[off];
    if(off >= LIB_OFF && off - LIB_OFF < strlen(LIB_STRING))
        return (uint8_t)LIB_STRING[off - LIB_OFF];
    const uint16_t hw = (uint16_t)(((off & ~1u) * 31u) >> 3);
    return (off & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

static bool is_rom(const uint8_t* data, uint32_t len) {
    for(uint32_t off = 0; off < len; off++) {
        if(data[off] != rom_byte(off)) return false;
    }
    return true;
}

static uint32_t calls(uint8_t code) {
    return hook.calls[code];
}

/* A Pokemon put: the golden Treecko at level 50, through the editor */
static bool level50(Gen3Pkm* out) {
    Gen3Pkm p;
    if(!gen3_save_mon_to_pkm(party[0], 100, &p) || !gen3_edit_load(&ed, &p)) return false;
    gen3_edit_set_level(&ed, 50);
    *out = ed.pkm;
    return true;
}

/* ---- Connect ---- */

TEST(test_job_connect) {
    ASSERT_TRUE(connected());
    const Gen3CartId* id = gen3_cart_id(&cart);
    ASSERT_TRUE(id->can_write);
    ASSERT_STR_EQ(id->code, "AXVE");
    ASSERT_EQ(id->why, Gen3CartRefuseNone);
    ASSERT_EQ(id->flash_id, MX);
    ASSERT_EQ(gen3_cart_rom_size(&cart), MIB);
    ASSERT_EQ(calls(GEN3_HCMD_HELLO), 1);
    ASSERT_EQ(calls(GEN3_HCMD_CART_INFO), 1);
    ASSERT_EQ(calls(GEN3_HCMD_SAVE_PROBE), 1);
    ASSERT_TRUE(prog.calls[Gen3CartStepIdentify] > 0);
    /* nothing backed up yet: nothing to browse or write */
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    Gen3CartRefuse why = Gen3CartRefuseNone;
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "");
    Gen3SaveTrainer t;
    ASSERT_FALSE(gen3_cart_trainer(&cart, &t));
}

/* A lost command can keep the helper busy past one connect's sync: the next connect keeps trying
 * for that command's BUSY budget. A 16 MiB ROM: CART_INFO's scan takes about 7 s. */
TEST(test_job_connect_waits_out_busy) {
    ASSERT_TRUE(default_save());
    insert("AXVE", 16u * MIB, FakeSaveFlash);
    hook.unplug_code = GEN3_HCMD_CART_INFO;
    hook.unplug_len = 600;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartLinkLost);
    ASSERT_TRUE(fh.busy);
    tap.syncs = 0;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_TRUE(tap.syncs >= 2);
    ASSERT_STR_EQ(gen3_cart_id(&cart)->code, "AXVE");
    ASSERT_EQ(calls(GEN3_HCMD_CART_INFO), 2);
}

/* After a lost command, no job sends anything until a connect */
TEST(test_job_link_needs_connect) {
    ASSERT_TRUE(backed_up());
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh) + 3000, FOREVER);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    fake_helper_replug(&fh);
    const uint32_t before = fake_helper_transfers(&fh);
    const uint32_t exchanges = tap.exchanges;
    bool dropped = true;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    ASSERT_EQ(fake_helper_transfers(&fh), before);
    ASSERT_EQ(tap.exchanges, exchanges);
    /* nothing about the backup changed meanwhile */
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
    ASSERT_FALSE(dropped);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    /* a backup lost midway */
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh) + 3000, FOREVER);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
}

/* The helper busy past a read's budget: BusyTimeout, and the next connect waits for it */
TEST(test_job_busy_timeout) {
    ASSERT_TRUE(connected());
    hook.slow_code = GEN3_HCMD_READ_SECTOR;
    hook.slow_us = 10000000u;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartBusyTimeout);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    tap.syncs = 0;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_TRUE(tap.syncs >= 2);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
}

/* A helper that answers HELLO for another protocol: no job talks to it */
TEST(test_job_connect_wrong_helper) {
    ASSERT_TRUE(connected());
    tap.fake_code = GEN3_HCMD_HELLO;
    tap.fake_status = GEN3_HST_OK;
    tap.fake_n = 4;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartHelperError);
    tap.fake_code = 0;
    const uint32_t exchanges = tap.exchanges;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(tap.exchanges, exchanges);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
}

/* Back while connecting: between commands */
TEST(test_job_connect_cancel) {
    ASSERT_TRUE(setup());
    prog.cancel_step = Gen3CartStepIdentify;
    prog.cancel_at = 1;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartCancelled);
    ASSERT_EQ(calls(GEN3_HCMD_HELLO), 1);
    ASSERT_EQ(calls(GEN3_HCMD_CART_INFO), 0);
    cancel = false;
    prog.cancel_step = Gen3CartStepNone;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
}

/* Every answer the client can give, as a job's result */
TEST(test_job_client_results) {
    static const struct {
        uint8_t status;
        uint16_t n;
        Gen3CartResult want;
    } cases[] = {
        {GEN3_HST_REFUSED, 2, Gen3CartRefused},
        {GEN3_HST_UNSUPPORTED, 2, Gen3CartHelperError},
        {GEN3_HST_BAD_ARG, 2, Gen3CartHelperError},
        {GEN3_HST_OK, 3, Gen3CartHelperError}, /* a sound response of the wrong length */
    };
    for(uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASSERT_TRUE(connected());
        tap.fake_code = GEN3_HCMD_READ_SECTOR;
        tap.fake_status = cases[i].status;
        tap.fake_n = cases[i].n;
        ASSERT_EQ(gen3_cart_backup(&cart, STAMP), cases[i].want);
        ASSERT_EQ(fake_store_count(&store, ""), 0);
        /* the link is still in step: the next job runs */
        tap.fake_code = 0;
        ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    }
    /* the helper itself refuses a read the Flipper should never send: a save chip it can't read */
    ASSERT_TRUE(default_save());
    insert("AXVE", MIB, FakeSaveSram);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    cart.id.can_read_save = true;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartHelperError);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    FakeGbaCounts n;
    fake_gba_counts(&gba, &n);
    ASSERT_EQ(n.erases + n.programs + n.commits + n.bank_selects, 0);
}

/* ---- Backup ---- */

TEST(test_job_backup_ok) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
    ASSERT_TRUE(work_is(flash(), &spec, 0));
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), BACKUP_NAME);
    Gen3SaveTrainer t;
    ASSERT_TRUE(gen3_cart_trainer(&cart, &t));
    ASSERT_STR_EQ(t.name, "BRENDAN");
    ASSERT_EQ(t.tid, 12345);
    ASSERT_EQ(t.sid, 54321);
    const Gen3SaveSlots* s = gen3_cart_slots(&cart);
    ASSERT_EQ(s->result, Gen3SaveOk);
    ASSERT_EQ(s->active, 0);
    ASSERT_EQ(s->save_counter, 4);
    ASSERT_TRUE(s->strict_ok);
    ASSERT_FALSE(s->padded);
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    Gen3CartRefuse why = Gen3CartRefuseSave;
    ASSERT_TRUE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseNone);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseNone);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    /* read once, then checked on the cart: every sector */
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 32);
    ASSERT_EQ(calls(GEN3_HCMD_SECTOR_CRC32), 32);
    ASSERT_EQ(prog.calls[Gen3CartStepBackupRead], 32);
    ASSERT_EQ(prog.last_done[Gen3CartStepBackupRead], 32);
    ASSERT_EQ(prog.last_total[Gen3CartStepBackupRead], 32);
    ASSERT_TRUE(prog.calls[Gen3CartStepBackupVerify] > 0);
    ASSERT_TRUE(prog.calls[Gen3CartStepAnalyze] > 0);
    /* nothing ever wrote to the chip */
    FakeGbaCounts n;
    fake_gba_counts(&gba, &n);
    ASSERT_EQ(n.erases + n.programs + n.commits + n.chip_erases, 0);
}

TEST(test_job_backup_nosave) {
    ASSERT_TRUE(default_save());
    memset(img, 0xFF, sizeof(img));
    insert("AXVE", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash("backups/AXVE_nosave_" STAMP ".sav"));
    ASSERT_EQ(gen3_cart_slots(&cart)->result, Gen3SaveEmpty);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    uint32_t size;
    ASSERT_TRUE(file(GEN3_CART_WORK_NAME, &size) == NULL);
    Gen3SaveTrainer t;
    ASSERT_FALSE(gen3_cart_trainer(&cart, &t));
    /* a cart with no save can still be written, by a restore, but not by an edit */
    ASSERT_TRUE(gen3_cart_id(&cart)->can_write);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
    Gen3CartRefuse why;
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
    ASSERT_EQ(gen3_cart_party_count(&cart), 0);
    uint8_t mon[100];
    size_t len;
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 0, mon, &len), Gen3CartBadArg);
}

/* A save gone since the last backup: its working copy goes too */
TEST(test_job_backup_save_gone) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    uint32_t size;
    ASSERT_TRUE(file(GEN3_CART_WORK_NAME, &size) != NULL);
    memset(flash(), 0xFF, FAKE_SAVE_LEN);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash("backups/AXVE_nosave_" STAMP ".sav"));
    ASSERT_TRUE(file(GEN3_CART_WORK_NAME, &size) == NULL);
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, 1, 19));
}

TEST(test_job_backup_sd_full_precheck) {
    ASSERT_TRUE(connected());
    fake_store_init(&store, 150000);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdFull);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

/* The pre-check sees 64 MiB, the card fills after 17 sectors: SdFull, not SdError */
TEST(test_job_backup_sd_full_midway) {
    ASSERT_TRUE(connected());
    fake_store_init(&store, 70000);
    fake_store_report_free(&store, 64u * MIB);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdFull);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 0);
    ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 18);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
}

TEST(test_job_backup_sd_write_error) {
    ASSERT_TRUE(connected());
    fake_store_fail_write_after(&store, 5);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

TEST(test_job_backup_readback_mismatch) {
    ASSERT_TRUE(connected());
    fake_store_corrupt_read(&store, GEN3_CART_BACKUP_TMP, 7 * SECTOR_LEN + 9);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartVerifyFailed);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 0);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

/* The cart reads one sector differently the second time */
TEST(test_job_backup_flash_unstable) {
    ASSERT_TRUE(connected());
    hook.flip = true;
    hook.flip_sector = 4;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartVerifyFailed);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

TEST(test_job_backup_link_lost) {
    ASSERT_TRUE(connected());
    fake_helper_unplug_at(&fh, 25000, FOREVER);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_TRUE(calls(GEN3_HCMD_READ_SECTOR) > 1);
}

TEST(test_job_backup_cancel) {
    ASSERT_TRUE(connected());
    prog.cancel_step = Gen3CartStepBackupRead;
    prog.cancel_at = 3;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartCancelled);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 3);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

/* Backup only: the Japanese layouts aren't known */
TEST(test_job_backup_japanese) {
    ASSERT_TRUE(default_save());
    insert("AXVJ", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseJapanese);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash("backups/AXVJ_nosave_" STAMP ".sav"));
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    uint32_t size;
    ASSERT_TRUE(file(GEN3_CART_WORK_NAME, &size) == NULL);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseJapanese);
    Gen3SaveTrainer t;
    ASSERT_FALSE(gen3_cart_trainer(&cart, &t));
}

/* A save chip the helper doesn't read: not even one read is sent */
TEST(test_job_backup_refused_sram) {
    ASSERT_TRUE(default_save());
    insert("AXVE", MIB, FakeSaveSram);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSram);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartRefused);
    bool dropped = true;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartRefused);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 0);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 0);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    FakeGbaCounts n;
    fake_gba_counts(&gba, &n);
    ASSERT_EQ(n.erases + n.programs + n.commits + n.bank_selects, 0);
}

/* A save the game loads but the Flipper can't write: Save, and can_write stays (a restore may
 * still write the cart) */
TEST(test_job_backup_not_strict_keeps_can_write) {
    ASSERT_TRUE(default_save());
    fake_save_set_counter(img, phys(0, 13), 2);
    insert("AXVE", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_FALSE(gen3_cart_slots(&cart)->strict_ok);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
    ASSERT_TRUE(gen3_cart_id(&cart)->can_write);
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    Gen3CartRefuse why = Gen3CartRefuseNone;
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
    ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
    /* the reason is the backup's: without one, only the identification's is left */
    fake_store_fail_write_after(&store, 0);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseNone);
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
}

/* The working copy can't be made: the backup goes too */
TEST(test_job_backup_work_copy_fails) {
    ASSERT_TRUE(connected());
    fake_store_fail_write_after(&store, 32);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "");
    ASSERT_FALSE(gen3_cart_browsable(&cart));
}

/* A loaded sector with a non-zero byte past its section: browsable, not writable */
TEST(test_job_backup_padded_not_writable) {
    ASSERT_TRUE(default_save());
    img[phys(0, 0) * SECTOR_LEN + 0x890] = 0x5A;
    insert("AXVE", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_slots(&cart)->strict_ok);
    ASSERT_TRUE(gen3_cart_slots(&cart)->padded);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
    ASSERT_TRUE(gen3_cart_id(&cart)->can_write);
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    Gen3CartRefuse why = Gen3CartRefuseNone;
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
}

/* A backup for a stamp already used gets -2, -3, ... */
TEST(test_job_backup_name_taken) {
    ASSERT_TRUE(backed_up());
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "backups/AXVE_12345_" STAMP "-2.sav");
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "backups/AXVE_12345_" STAMP "-3.sav");
    ASSERT_EQ(fake_store_count(&store, ".sav"), 3);
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
    /* a stamp too long for a name */
    ASSERT_EQ(gen3_cart_backup(&cart, "20261025-143005-1234567"), Gen3CartBadArg);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 3);
}

/* ---- Attach ---- */

/* The helper sent again to the same cart, whose save didn't change: the backup and the edits stay */
TEST(test_job_attach_keeps_changes) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartOk);
    fake_helper_restart(&fh);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    bool dropped = true;
    ASSERT_EQ(gen3_cart_attach(&cart, "20261025-150000", &dropped), Gen3CartOk);
    ASSERT_FALSE(dropped);
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 0));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 1);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 32);
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
    ASSERT_TRUE(work_differs_only_in(1));
}

/* The game saved since the backup: a new backup, the edits dropped */
TEST(test_job_attach_after_game_save) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartOk);
    FakeSaveSpec s2 = spec;
    s2.slot_present[1] = true;
    s2.counter[1] = 5;
    s2.first[1] = 11;
    fake_save_build(img2, &s2);
    fake_save_put_trainer(img2, &s2, 1, "MAY", 1, 22222, 33333);
    memcpy(flash() + 14 * SECTOR_LEN, img2 + 14 * SECTOR_LEN, 14 * SECTOR_LEN);
    bool dropped = false;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
    ASSERT_TRUE(dropped);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 0));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "backups/AXVE_22222_" STAMP ".sav");
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    ASSERT_FALSE(sav_is_flash(BACKUP_NAME));
    const Gen3SaveSlots* s = gen3_cart_slots(&cart);
    ASSERT_EQ(s->active, 1);
    ASSERT_EQ(s->save_counter, 5);
    /* the trainer of the slot the game loads now, from its id 0 at sector 25 */
    Gen3SaveTrainer t;
    ASSERT_TRUE(gen3_cart_trainer(&cart, &t));
    ASSERT_STR_EQ(t.name, "MAY");
    ASSERT_EQ(t.tid, 22222);
    ASSERT_EQ(t.gender, 1);
    /* the working copy is slot B, in id order */
    ASSERT_TRUE(work_is(flash(), &s2, 1));
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
}

/* Only a footer changed (a counter), or only a byte the game's checksum doesn't cover (the full
 * checksum, so the slot is padded now): either is another save */
TEST(test_job_attach_footer_changed) {
    for(uint32_t i = 0; i < 2; i++) {
        ASSERT_TRUE(backed_up());
        Gen3Pkm p;
        ASSERT_TRUE(level50(&p));
        ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
        if(i == 0) {
            fake_save_set_counter(flash(), phys(0, 13), 2);
        } else {
            flash()[phys(0, 0) * SECTOR_LEN + 0x890] = 0x5A;
        }
        bool dropped = false;
        ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
        ASSERT_TRUE(dropped);
        ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
        ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
        ASSERT_EQ(gen3_cart_changes(&cart), 0);
        ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
        ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
        ASSERT_EQ(gen3_cart_slots(&cart)->padded, i == 1);
    }
}

/* The same cart with another save chip: the backup isn't current */
TEST(test_job_connect_other_chip) {
    ASSERT_TRUE(backed_up());
    fake_gba_save(&gba, FakeSaveFlash, 0x1362u);
    fake_helper_restart(&fh);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->flash_id, 0x1362u);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    bool dropped = true;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
    ASSERT_FALSE(dropped);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
}

/* Another cart in the GBA: the backup isn't current from its connect on */
TEST(test_job_attach_other_cart) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartOk);
    /* the same save, but revision 1 of the game */
    helper_cmd_header(hdr, gba.own, "POKEMON RUBY", "AXVE", 1);
    fake_gba_rom(&gba, hdr, MIB);
    fake_gba_rom_string(&gba, LIB_OFF, LIB_STRING);
    fake_helper_restart(&fh);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->version, 1);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
    bool dropped = false;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
    ASSERT_TRUE(dropped);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 0);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
}

/* ---- The working copy ---- */

TEST(test_job_mon_get_put) {
    ASSERT_TRUE(backed_up());
    uint8_t mon[100];
    size_t len = 0;
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 0, mon, &len), Gen3CartOk);
    ASSERT_EQ(len, 100);
    uint8_t golden[100];
    golden_game(golden);
    ASSERT_MEM_EQ(mon, golden, 100);
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 2, mon, &len), Gen3CartOk);
    ASSERT_MEM_EQ(mon, party[2], 100);
    ASSERT_EQ(gen3_cart_mon_get(&cart, 1, 19, mon, &len), Gen3CartOk);
    ASSERT_EQ(len, 80);
    ASSERT_MEM_EQ(mon, golden, 80);
    ASSERT_EQ(gen3_cart_party_count(&cart), 3);
    /* the party edit: id 1 only */
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartOk);
    ASSERT_TRUE(work_differs_only_in(1));
    ASSERT_EQ(cart.dirty, 0x0002);
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 0));
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 1));
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, 0, 0));
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 0, mon, &len), Gen3CartOk);
    uint8_t want[100];
    gen3_save_pkm_to_mon(&p, 100, want);
    ASSERT_MEM_EQ(mon, want, 100);
    Gen3Pkm back;
    ASSERT_TRUE(gen3_save_mon_to_pkm(mon, 100, &back));
    ASSERT_EQ(gen3_pkm_get(&back, Gen3FieldLevel), 50);
    /* box 1 slot 19 straddles ids 5 and 6 */
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    ASSERT_EQ(cart.dirty, 0x0062);
    ASSERT_EQ(gen3_cart_changes(&cart), 2);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, 1, 19));
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, 1, 18));
    ASSERT_EQ(gen3_cart_mon_get(&cart, 1, 19, mon, &len), Gen3CartOk);
    ASSERT_MEM_EQ(mon, want, 80);
    /* the same slot again is one change */
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_changes(&cart), 2);
    /* box 0's first slots are not the party's */
    ASSERT_EQ(gen3_cart_mon_put(&cart, 0, 2, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_changes(&cart), 3);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, 0, 2));
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 2));
    ASSERT_EQ(cart.dirty, 0x0062);
    /* the backup is never written */
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
    /* out of range */
    ASSERT_EQ(gen3_cart_mon_get(&cart, 14, 0, mon, &len), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_mon_get(&cart, 0, 30, mon, &len), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 6, mon, &len), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_mon_put(&cart, 14, 0, &p), Gen3CartBadArg);
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, 14, 0));
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 6));
}

TEST(test_job_party_append) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 3, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_party_count(&cart), 4);
    const Gen3SaveSpan cs = gen3_save_party_count_span(Gen3LinkGameRS);
    uint32_t size;
    const uint8_t* w = file(GEN3_CART_WORK_NAME, &size);
    ASSERT_TRUE(w != NULL);
    ASSERT_EQ(w[cs.id * SECTOR_LEN + cs.off], 4);
    ASSERT_TRUE(work_differs_only_in(1));
    ASSERT_EQ(cart.dirty, 0x0002);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 3));
    /* past the count: refused, nothing changes */
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 5, &p), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_party_count(&cart), 4);
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    /* up to six */
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 4, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 5, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_party_count(&cart), 6);
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 6, &p), Gen3CartBadArg);
}

TEST(test_job_discard) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartOk);
    ASSERT_TRUE(work_is(flash(), &spec, 0));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(cart.dirty, 0);
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, 1, 19));
    ASSERT_TRUE(gen3_cart_browsable(&cart));
}

TEST(test_job_box_name) {
    ASSERT_TRUE(backed_up());
    char name[9];
    ASSERT_TRUE(gen3_cart_box_name(&cart, 0, name));
    ASSERT_STR_EQ(name, "BOX 1");
    ASSERT_FALSE(gen3_cart_box_name(&cart, 14, name));
    ASSERT_STR_EQ(name, "");
}

/* A store failure while putting: the working copy is no longer trusted until a discard */
TEST(test_job_mon_put_sd_error) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    fake_store_fail_write_after(&store, 0);
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartSdError);
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
    uint8_t mon[100];
    size_t len;
    ASSERT_EQ(gen3_cart_mon_get(&cart, 1, 19, mon, &len), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartBadArg);
    fake_store_fail_write_after(&store, 1000);
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
    ASSERT_TRUE(work_is(flash(), &spec, 0));
}

/* ---- ROM copy ---- */

TEST(test_job_rom_dump) {
    ASSERT_TRUE(connected());
    char name[GEN3_CART_NAME_MAX];
    gen3_cart_rom_name(&cart, name);
    ASSERT_STR_EQ(name, ROM_NAME);
    uint32_t done = 1;
    ASSERT_FALSE(gen3_cart_rom_part(&cart, &done));
    ASSERT_EQ(done, 0);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartOk);
    uint32_t size;
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_EQ(size, MIB);
    ASSERT_TRUE(is_rom(rom, size));
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 256);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_CRC32), 4);
    ASSERT_EQ(fake_store_count(&store, ".part"), 0);
    ASSERT_EQ(prog.last_done[Gen3CartStepRomRead], MIB);
    ASSERT_EQ(prog.last_total[Gen3CartStepRomRead], MIB);
    ASSERT_EQ(prog.calls[Gen3CartStepRomVerify], 4);
    /* a second copy replaces the first */
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartOk);
    ASSERT_EQ(fake_store_count(&store, ".gba"), 1);
}

TEST(test_job_rom_resume) {
    ASSERT_TRUE(connected());
    prog.cancel_step = Gen3CartStepRomRead;
    prog.cancel_at = 300u * KIB;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartCancelled);
    uint32_t size;
    const uint8_t* part = file(PART_NAME, &size);
    ASSERT_TRUE(part != NULL);
    ASSERT_EQ(size, REGION);
    ASSERT_TRUE(is_rom(part, size));
    uint32_t done = 0;
    ASSERT_TRUE(gen3_cart_rom_part(&cart, &done));
    ASSERT_EQ(done, REGION);
    cancel = false;
    prog.cancel_step = Gen3CartStepNone;
    memset(hook.calls, 0, sizeof(hook.calls));
    ASSERT_EQ(gen3_cart_rom_dump(&cart, true), Gen3CartOk);
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_EQ(size, MIB);
    ASSERT_TRUE(is_rom(rom, size));
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 192);
    ASSERT_EQ(fake_store_count(&store, ".part"), 0);
    ASSERT_FALSE(gen3_cart_rom_part(&cart, &done));
}

/* A resumed copy needs room only for the rest */
TEST(test_job_rom_resume_space) {
    ASSERT_TRUE(connected());
    prog.cancel_step = Gen3CartStepRomRead;
    prog.cancel_at = 300u * KIB;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartCancelled);
    cancel = false;
    prog.cancel_step = Gen3CartStepNone;
    /* the .part's region, then 800,000 bytes free: enough for the other three and a sector */
    store.capacity = REGION + 800000u;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, true), Gen3CartOk);
    uint32_t size;
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_TRUE(is_rom(rom, MIB));
    /* from the start, the same room doesn't do */
    ASSERT_TRUE(api.remove(api.ctx, ROM_NAME));
    store.capacity = 800000u;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartSdFull);
}

/* The card gives a region back differently: read once more, then VerifyFailed */
TEST(test_job_rom_sd_readback_differs) {
    ASSERT_TRUE(connected());
    fake_store_corrupt_read(&store, PART_NAME, REGION + 5u);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartVerifyFailed);
    uint32_t size;
    const uint8_t* part = file(PART_NAME, &size);
    ASSERT_TRUE(part != NULL);
    ASSERT_EQ(size, REGION);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 192);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_CRC32), 3);
}

/* A power cut can leave a region written but not checked: a resume checks the last one again */
TEST(test_job_rom_resume_checks_last_region) {
    ASSERT_TRUE(connected());
    /* two regions, the second one wrong, and a part of a third */
    void* f = api.open(api.ctx, PART_NAME, Gen3StoreCreate);
    ASSERT_TRUE(f != NULL);
    for(uint32_t off = 0; off < 2u * REGION + SECTOR_LEN; off += SECTOR_LEN) {
        for(uint32_t i = 0; i < SECTOR_LEN; i++) chunk[i] = rom_byte(off + i);
        if(off == REGION + 0x10000u) chunk[5] ^= 0x40;
        ASSERT_TRUE(api.write(api.ctx, f, off, chunk, SECTOR_LEN));
    }
    ASSERT_TRUE(api.close(api.ctx, f));
    uint32_t done = 0;
    ASSERT_TRUE(gen3_cart_rom_part(&cart, &done));
    ASSERT_EQ(done, 2u * REGION);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, true), Gen3CartOk);
    uint32_t size;
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_EQ(size, MIB);
    ASSERT_TRUE(is_rom(rom, size));
    /* region 1 read again, region 0 kept */
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 192);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_CRC32), 4);
}

/* A region that comes over wrong once is read again */
TEST(test_job_rom_region_retry) {
    ASSERT_TRUE(connected());
    /* armed now: CART_INFO's scan reads the ROM too */
    fake_gba_rom_glitch_once(&gba, 0x40010);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartOk);
    uint32_t size;
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_EQ(size, MIB);
    ASSERT_TRUE(is_rom(rom, size));
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 320);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_CRC32), 5);
}

/* Wrong twice: VerifyFailed, and the .part keeps the regions before it */
TEST(test_job_rom_region_fails_twice) {
    ASSERT_TRUE(connected());
    fake_gba_rom_glitch_once(&gba, 0x40010);
    /* armed again once region 1's first check has run: its second read is wrong too */
    hook.glitch_after_crc = 2;
    hook.glitch_off = 0x40010;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartVerifyFailed);
    uint32_t size;
    const uint8_t* part = file(PART_NAME, &size);
    ASSERT_TRUE(part != NULL);
    ASSERT_EQ(size, REGION);
    ASSERT_TRUE(is_rom(part, size));
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 192);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_CRC32), 3);
    uint32_t n;
    ASSERT_TRUE(file(ROM_NAME, &n) == NULL);
}

TEST(test_job_rom_sd_full) {
    ASSERT_TRUE(connected());
    fake_store_init(&store, 500000);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartSdFull);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 0);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
}

/* A lost link keeps only the regions already checked */
TEST(test_job_rom_link_lost) {
    ASSERT_TRUE(connected());
    prog.unplug_step = Gen3CartStepRomRead;
    prog.unplug_at = 600u * KIB;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    uint32_t size;
    const uint8_t* part = file(PART_NAME, &size);
    ASSERT_TRUE(part != NULL);
    ASSERT_EQ(size, 2u * REGION);
    ASSERT_TRUE(is_rom(part, size));
    uint32_t done = 0;
    ASSERT_TRUE(gen3_cart_rom_part(&cart, &done));
    ASSERT_EQ(done, 2u * REGION);
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, true), Gen3CartOk);
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_TRUE(is_rom(rom, MIB));
}

/* ---- Results ---- */

TEST(test_result_texts) {
    static const char* const want[] = {
        [Gen3CartOk] = "Done.",
        [Gen3CartWrittenNoBackup] =
            "Written and verified,\nbut no new backup.\nBack up again first.",
        [Gen3CartNothing] = "No changes to write.",
        [Gen3CartCancelled] = "Stopped.",
        [Gen3CartLinkLost] = "Link lost. The cart\nstill loads its last\ngood save.",
        [Gen3CartBusyTimeout] = "The GBA stopped\nanswering. Cart keeps\nits last good save.",
        [Gen3CartHelperError] = "The helper reported\nan error. See\ngen3_cart.log.",
        [Gen3CartRefused] = "Not allowed for this\ncartridge or save.",
        [Gen3CartChanged] = "The cart changed\nsince the backup.\nReconnect to back up.",
        [Gen3CartSdFull] = "SD card full.",
        [Gen3CartSdError] = "SD card error.",
        [Gen3CartVerifyFailed] = "Check failed: the\ndata read back\ndiffers.",
        [Gen3CartFlashError] = "Flash write failed.\nThe cart still loads\nits last good save.",
        [Gen3CartBadBackup] = "Not a valid backup\nfor this game.",
        [Gen3CartBadArg] = "Invalid request.",
        [Gen3CartNoHelper] = "Helper file missing.\nReinstall Pokemon\nCartridge.",
        [Gen3CartMultibootFailed] =
            "Power the GBA off,\nhold START+SELECT,\npower on, then Retry.",
        [Gen3CartLinkBusy] = "The link is busy.\nClose other link\napps, then Retry.",
    };
    for(int r = Gen3CartOk; r <= Gen3CartLinkBusy; r++) {
        const char* t = gen3_cart_result_text((Gen3CartResult)r);
        ASSERT_TRUE(t != NULL);
        ASSERT_STR_EQ(t, want[r]);
        ASSERT_TRUE(strlen(t) > 0);
        uint32_t lines = 1;
        uint32_t line = 0;
        for(const char* c = t; *c; c++) {
            if(*c == '\n') {
                lines++;
                line = 0;
            } else {
                line++;
                ASSERT_TRUE(line <= 21);
            }
        }
        ASSERT_TRUE(lines <= 3);
    }
    ASSERT_STR_EQ(gen3_cart_result_text((Gen3CartResult)(Gen3CartLinkBusy + 1)), "");
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_job_connect);
    RUN(test_job_connect_waits_out_busy);
    RUN(test_job_link_needs_connect);
    RUN(test_job_busy_timeout);
    RUN(test_job_connect_wrong_helper);
    RUN(test_job_connect_cancel);
    RUN(test_job_client_results);
    RUN(test_job_backup_ok);
    RUN(test_job_backup_nosave);
    RUN(test_job_backup_save_gone);
    RUN(test_job_backup_sd_full_precheck);
    RUN(test_job_backup_sd_full_midway);
    RUN(test_job_backup_sd_write_error);
    RUN(test_job_backup_readback_mismatch);
    RUN(test_job_backup_flash_unstable);
    RUN(test_job_backup_link_lost);
    RUN(test_job_backup_cancel);
    RUN(test_job_backup_japanese);
    RUN(test_job_backup_refused_sram);
    RUN(test_job_backup_not_strict_keeps_can_write);
    RUN(test_job_backup_padded_not_writable);
    RUN(test_job_backup_work_copy_fails);
    RUN(test_job_backup_name_taken);
    RUN(test_job_attach_keeps_changes);
    RUN(test_job_attach_after_game_save);
    RUN(test_job_attach_footer_changed);
    RUN(test_job_connect_other_chip);
    RUN(test_job_attach_other_cart);
    RUN(test_job_mon_get_put);
    RUN(test_job_party_append);
    RUN(test_job_discard);
    RUN(test_job_box_name);
    RUN(test_job_mon_put_sd_error);
    RUN(test_job_rom_dump);
    RUN(test_job_rom_resume);
    RUN(test_job_rom_resume_space);
    RUN(test_job_rom_resume_checks_last_region);
    RUN(test_job_rom_sd_readback_differs);
    RUN(test_job_rom_region_retry);
    RUN(test_job_rom_region_fails_twice);
    RUN(test_job_rom_sd_full);
    RUN(test_job_rom_link_lost);
    RUN(test_result_texts);
    TEST_EXIT();
}
