#include <stdio.h>
#include <string.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_hlink.h>
#include <src/gen3/gen3_text.h>

#define CART_ROM_BASE 0x08000000u /* the cart ROM on the GBA's bus (multiboot-and-save.md §3.1) */
#define CART_HEADER_LEN sizeof(((Gen3Cart*)0)->header)
#define CART_INFOS (GEN3_SAVE_SLOT_SECTORS * 2u) /* sectors 0..27: the two slots */
/* A backup name already taken gets -2 .. -99 */
#define CART_NAME_TRIES 99u
/* The longest backup name around its stamp: backups/XXXX_nosave_<stamp>-99.sav */
#define CART_BACKUP_NAME_FIXED (sizeof(GEN3_CART_BACKUP_DIR "/XXXX_nosave_-99.sav") - 1u)
#define CART_STAMP_MAX (GEN3_CART_NAME_MAX - 1u - CART_BACKUP_NAME_FIXED)
#define CART_ROM_FORMAT GEN3_CART_ROM_DIR "/%s_v%u.gba"

_Static_assert(CART_STAMP_MAX >= sizeof("YYYYMMDD-HHMMSS") - 1u, "a stamp fits a backup name");
_Static_assert(
    GEN3_CART_ROM_REGION % GEN3_CART_BUF_SIZE == 0,
    "a region is whole ROM_READs of one buffer");
_Static_assert(GEN3_CART_BUF_SIZE == GEN3_SAVE_SECTOR_SIZE, "the buffer holds a sector");
_Static_assert(sizeof(((Gen3HCartInfo*)0)->header) == CART_HEADER_LEN, "the header copy");

const char* gen3_cart_result_text(Gen3CartResult r) {
    /* At most 3 lines of 21 characters: the result screen starts its text at y 14 and may show
     * two buttons */
    static const char* const texts[] = {
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
    return (unsigned)r < sizeof(texts) / sizeof(texts[0]) ? texts[r] : "";
}

/* ---- Progress, results ---- */

static void cart_step(Gen3Cart* c, Gen3CartStep step, uint32_t done, uint32_t total) {
    if(c->progress.cb) c->progress.cb(c->progress.ctx, step, done, total);
}

static bool cart_cancelled(const Gen3Cart* c) {
    return c->progress.cancel && *c->progress.cancel;
}

/* A client call's result as a job's. A lost link, or a helper busy past the command's budget,
 * needs a new connect, which keeps trying for that budget plus a sync: the helper finishes the
 * lost command on its own first. On a flash step any status but REFUSED is a FlashError. */
static Gen3CartResult cart_link(Gen3Cart* c, Gen3HcliResult r, uint32_t busy_us, bool flash) {
    switch(r) {
    case Gen3HcliOk:
        return Gen3CartOk;
    case Gen3HcliLinkLost:
    case Gen3HcliBusyTimeout: {
        const uint32_t relink = busy_us + GEN3_HCLI_SYNC_US;
        c->linked = false;
        if(c->relink_us < relink) c->relink_us = relink;
        return r == Gen3HcliLinkLost ? Gen3CartLinkLost : Gen3CartBusyTimeout;
    }
    case Gen3HcliStatusError:
        /* the status only matters once the call says it came */
        if(c->cli->last_status == GEN3_HST_REFUSED) return Gen3CartRefused;
        return flash ? Gen3CartFlashError : Gen3CartHelperError;
    case Gen3HcliBadResponse:
        break;
    }
    return Gen3CartHelperError;
}

/* A read-class command: GEN3_HLINK_BUSY_US */
static Gen3CartResult cart_read(Gen3Cart* c, Gen3HcliResult r) {
    return cart_link(c, r, GEN3_HLINK_BUSY_US, false);
}

/* A write of `need` bytes failed: the card is full, or failing */
static Gen3CartResult cart_sd_fail(Gen3Cart* c, uint32_t need) {
    return c->store.free_bytes(c->store.ctx) < need ? Gen3CartSdFull : Gen3CartSdError;
}

/* ---- The cart and its backup ---- */

/* The save the backup analyzed has a slot the game loads, with all 14 ids */
static bool cart_loads(const Gen3Cart* c) {
    if(!c->analyzed) return false;
    if(c->slots.result != Gen3SaveOk && c->slots.result != Gen3SaveError) return false;
    for(uint32_t k = 0; k < GEN3_SAVE_SLOT_SECTORS; k++) {
        if(c->slots.phys[k] == GEN3_SAVE_NO_SECTOR) return false;
    }
    return true;
}

/* The identification's reason, or Save when the analyzed save isn't one to write. can_write
 * stays: a restore may still write a cart whose save is empty or damaged. */
static void cart_set_why(Gen3Cart* c) {
    c->id.why = c->cart_why;
    if(c->id.why == Gen3CartRefuseNone && c->analyzed &&
       (!c->slots.strict_ok || c->slots.padded)) {
        c->id.why = Gen3CartRefuseSave;
    }
}

/* The backup no longer describes the cart */
static void cart_drop_backup(Gen3Cart* c) {
    c->backup_current = false;
    c->analyzed = false;
    c->has_trainer = false;
    cart_set_why(c);
}

void gen3_cart_init(
    Gen3Cart* c,
    Gen3Hcli* cli,
    const Gen3Store* store,
    uint8_t* buf,
    const Gen3CartProgress* p) {
    memset(c, 0, sizeof(*c));
    c->cli = cli;
    c->store = *store;
    c->buf = buf;
    if(p) c->progress = *p;
}

/* HELLO. After a lost command the helper finishes it first, BUSY and then sending the response
 * nobody reads, and only then answers READY: each try's sync waits up to GEN3_HCLI_SYNC_US, so it
 * tries until relink_us has passed. */
static Gen3CartResult cart_hello(Gen3Cart* c) {
    const uint32_t tries =
        c->relink_us ? (c->relink_us + GEN3_HCLI_SYNC_US - 1u) / GEN3_HCLI_SYNC_US : 1u;
    Gen3HcliResult r = Gen3HcliLinkLost;
    for(uint32_t t = 0; t < tries; t++) {
        if(cart_cancelled(c)) return Gen3CartCancelled;
        r = gen3_hcli_connect(c->cli, &c->hello);
        if(r == Gen3HcliOk) {
            c->linked = true;
            c->relink_us = 0;
            return Gen3CartOk;
        }
        if(r != Gen3HcliLinkLost && r != Gen3HcliBusyTimeout) break;
    }
    return cart_read(c, r);
}

Gen3CartResult gen3_cart_connect(Gen3Cart* c) {
    cart_step(c, Gen3CartStepIdentify, 0, 3);
    c->linked = false;
    Gen3CartResult r = cart_hello(c);
    if(r != Gen3CartOk) return r;
    cart_step(c, Gen3CartStepIdentify, 1, 3);
    if(cart_cancelled(c)) return Gen3CartCancelled;
    r = cart_link(c, gen3_hcli_cart_info(c->cli, &c->info), GEN3_HCLI_BUSY_SCAN_US, false);
    if(r != Gen3CartOk) return r;
    if(memcmp(c->info.header, c->header, CART_HEADER_LEN) != 0) cart_drop_backup(c);
    cart_step(c, Gen3CartStepIdentify, 2, 3);
    if(cart_cancelled(c)) return Gen3CartCancelled;
    Gen3HProbe probe;
    r = cart_read(c, gen3_hcli_save_probe(c->cli, &probe));
    if(r != Gen3CartOk) return r;
    gen3_cart_identify(&c->info, &probe, &c->id);
    c->cart_why = c->id.why;
    if(probe.flash_id != c->flash_id) cart_drop_backup(c);
    cart_set_why(c);
    cart_step(c, Gen3CartStepIdentify, 3, 3);
    return Gen3CartOk;
}

/* ---- Backup ---- */

/* Every sector into the temporary file, with its CRC; then the cart reads each one again
 * (SECTOR_CRC32), and the card gives back the same bytes: a backup verified three ways, the
 * packet CRC being the first (plan 5 D7). Each sector's footer is kept for gen3_cart_attach. */
static Gen3CartResult cart_backup_read(Gen3Cart* c) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreCreate);
    if(!f) return Gen3CartSdError;
    Gen3CartResult r = Gen3CartOk;
    for(uint8_t s = 0; s < GEN3_SAVE_SECTORS && r == Gen3CartOk; s++) {
        r = cart_read(c, gen3_hcli_read_sector(c->cli, s, c->buf));
        if(r != Gen3CartOk) break;
        c->crc[s] = gen3_crc32(c->buf, GEN3_SAVE_SECTOR_SIZE);
        if(s < CART_INFOS) gen3_save_sector_info(c->id.game, c->buf, &c->infos[s]);
        if(!st->write(st->ctx, f, s * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = cart_sd_fail(c, GEN3_SAVE_SECTOR_SIZE);
            break;
        }
        cart_step(c, Gen3CartStepBackupRead, s + 1u, GEN3_SAVE_SECTORS);
        if(cart_cancelled(c)) r = Gen3CartCancelled;
    }
    if(!st->close(st->ctx, f) && r == Gen3CartOk) r = Gen3CartSdError;
    for(uint8_t s = 0; s < GEN3_SAVE_SECTORS && r == Gen3CartOk; s++) {
        uint32_t crc = 0;
        r = cart_read(c, gen3_hcli_sector_crc32(c->cli, s, &crc));
        if(r == Gen3CartOk && crc != c->crc[s]) r = Gen3CartVerifyFailed;
        cart_step(c, Gen3CartStepBackupVerify, s + 1u, 2u * GEN3_SAVE_SECTORS);
        if(r == Gen3CartOk && cart_cancelled(c)) r = Gen3CartCancelled;
    }
    if(r != Gen3CartOk) return r;
    f = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    for(uint8_t s = 0; s < GEN3_SAVE_SECTORS && r == Gen3CartOk; s++) {
        if(!st->read(st->ctx, f, s * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = Gen3CartSdError;
        } else if(gen3_crc32(c->buf, GEN3_SAVE_SECTOR_SIZE) != c->crc[s]) {
            r = Gen3CartVerifyFailed;
        }
        cart_step(c, Gen3CartStepBackupVerify, GEN3_SAVE_SECTORS + s + 1u, 2u * GEN3_SAVE_SECTORS);
    }
    st->close(st->ctx, f);
    return r;
}

/* The slots, the trainer from the loaded slot's id 0, and Save as the reason not to write a save
 * that isn't strict or is padded */
static Gen3CartResult cart_backup_analyze(Gen3Cart* c) {
    if(!c->id.can_browse) return Gen3CartOk;
    cart_step(c, Gen3CartStepAnalyze, 0, 1);
    gen3_save_analyze(c->infos, &c->slots);
    c->analyzed = true;
    cart_set_why(c);
    const uint8_t sec0 = c->slots.phys[0];
    const bool loads = c->slots.result == Gen3SaveOk || c->slots.result == Gen3SaveError;
    if(loads && sec0 != GEN3_SAVE_NO_SECTOR) {
        const Gen3Store* st = &c->store;
        void* f = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreRead);
        if(!f) return Gen3CartSdError;
        const bool ok =
            st->read(st->ctx, f, sec0 * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE);
        st->close(st->ctx, f);
        if(!ok) return Gen3CartSdError;
        gen3_save_trainer(c->buf, &c->trainer);
        c->has_trainer = true;
    }
    cart_step(c, Gen3CartStepAnalyze, 1, 1);
    return Gen3CartOk;
}

/* The temporary file becomes backups/<code>_<tid | nosave>_<stamp>.sav, with -2, -3, ... if that
 * name is taken; from now on it is the current backup */
static Gen3CartResult cart_backup_keep(Gen3Cart* c, const char* stamp) {
    const Gen3Store* st = &c->store;
    char tid[8];
    if(c->has_trainer) {
        snprintf(tid, sizeof(tid), "%05u", (unsigned)c->trainer.tid);
    } else {
        snprintf(tid, sizeof(tid), "nosave");
    }
    for(uint32_t n = 1; n <= CART_NAME_TRIES; n++) {
        char more[12] = "";
        if(n > 1) snprintf(more, sizeof(more), "-%u", (unsigned)n);
        snprintf(
            c->backup_name,
            sizeof(c->backup_name),
            GEN3_CART_BACKUP_DIR "/%s_%s_%s%s.sav",
            c->id.code,
            tid,
            stamp,
            more);
        uint32_t size;
        if(st->size(st->ctx, c->backup_name, &size)) continue;
        if(!st->rename(st->ctx, GEN3_CART_BACKUP_TMP, c->backup_name)) break;
        c->backup_current = true;
        memcpy(c->header, c->info.header, CART_HEADER_LEN);
        c->flash_id = c->id.flash_id;
        return Gen3CartOk;
    }
    c->backup_name[0] = '\0';
    return Gen3CartSdError;
}

/* The working copy anew from the backup: the loaded slot's 14 sectors in id order, no edits */
static Gen3CartResult cart_build_work(Gen3Cart* c) {
    const Gen3Store* st = &c->store;
    c->work_ok = false;
    void* in = st->open(st->ctx, c->backup_name, Gen3StoreRead);
    if(!in) return Gen3CartSdError;
    void* out = st->open(st->ctx, GEN3_CART_WORK_NAME, Gen3StoreCreate);
    if(!out) {
        st->close(st->ctx, in);
        return Gen3CartSdError;
    }
    Gen3CartResult r = Gen3CartOk;
    for(uint32_t k = 0; k < GEN3_SAVE_SLOT_SECTORS && r == Gen3CartOk; k++) {
        const uint32_t at = c->slots.phys[k] * GEN3_SAVE_SECTOR_SIZE;
        if(!st->read(st->ctx, in, at, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = Gen3CartSdError;
        } else if(!st->write(
                      st->ctx, out, k * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = cart_sd_fail(c, GEN3_SAVE_SECTOR_SIZE);
        }
    }
    st->close(st->ctx, in);
    if(!st->close(st->ctx, out) && r == Gen3CartOk) r = Gen3CartSdError;
    if(r != Gen3CartOk) {
        st->remove(st->ctx, GEN3_CART_WORK_NAME);
        return r;
    }
    c->dirty = 0;
    memset(c->changed, 0, sizeof(c->changed));
    c->work_ok = true;
    return Gen3CartOk;
}

Gen3CartResult gen3_cart_backup(Gen3Cart* c, const char* stamp) {
    if(!c->id.can_read_save) return Gen3CartRefused;
    if(!stamp || strlen(stamp) > CART_STAMP_MAX) return Gen3CartBadArg;
    if(!c->linked) return Gen3CartLinkLost;
    const Gen3Store* st = &c->store;
    cart_drop_backup(c);
    if(st->free_bytes(st->ctx) < GEN3_CART_BACKUP_SPACE) return Gen3CartSdFull;
    Gen3CartResult r = cart_backup_read(c);
    if(r == Gen3CartOk) r = cart_backup_analyze(c);
    if(r == Gen3CartOk) r = cart_backup_keep(c, stamp);
    if(r != Gen3CartOk) {
        st->remove(st->ctx, GEN3_CART_BACKUP_TMP);
        cart_drop_backup(c);
        return r;
    }
    /* The edits are gone with the save they were made on; a save the game loads gets a new
     * working copy, and if that fails, so does the backup */
    c->dirty = 0;
    memset(c->changed, 0, sizeof(c->changed));
    c->work_ok = false;
    if(!(c->id.can_browse && cart_loads(c))) {
        st->remove(st->ctx, GEN3_CART_WORK_NAME);
        return Gen3CartOk;
    }
    r = cart_build_work(c);
    if(r != Gen3CartOk) {
        st->remove(st->ctx, c->backup_name);
        c->backup_name[0] = '\0';
        cart_drop_backup(c);
    }
    return r;
}

/* The 28 footers READ_FOOTERS gave are the backup's: id, checksum, signature, counter, and
 * whether the checksum over all 0xF80 bytes holds */
static bool cart_footers_same(const Gen3Cart* c) {
    for(uint32_t s = 0; s < CART_INFOS; s++) {
        const Gen3HFooter* h = &c->footers[s];
        const Gen3SaveSectorInfo* b = &c->infos[s];
        if(h->id != b->f.id || h->checksum != b->f.checksum || h->signature != b->f.signature ||
           h->counter != b->f.counter || (h->full_checksum == h->checksum) != b->full_ok) {
            return false;
        }
    }
    return true;
}

Gen3CartResult gen3_cart_attach(Gen3Cart* c, const char* stamp, bool* dropped) {
    *dropped = false;
    if(!c->linked) return Gen3CartLinkLost;
    /* The connect already found the same header and flash id, or the backup isn't current */
    if(c->backup_current && c->id.can_read_save) {
        const Gen3CartResult r = cart_read(c, gen3_hcli_read_footers(c->cli, c->footers));
        if(r != Gen3CartOk) return r;
        if(cart_footers_same(c)) return Gen3CartOk;
    }
    const bool pending = c->dirty != 0 || gen3_cart_changes(c) > 0;
    const Gen3CartResult r = gen3_cart_backup(c, stamp);
    if(r == Gen3CartOk) *dropped = pending;
    return r;
}

Gen3CartResult gen3_cart_discard(Gen3Cart* c) {
    if(!c->backup_current || !c->id.can_browse || !cart_loads(c)) return Gen3CartBadArg;
    return cart_build_work(c);
}

/* ---- What the backup says ---- */

const Gen3CartId* gen3_cart_id(const Gen3Cart* c) {
    return &c->id;
}

const Gen3SaveSlots* gen3_cart_slots(const Gen3Cart* c) {
    return &c->slots;
}

bool gen3_cart_trainer(const Gen3Cart* c, Gen3SaveTrainer* out) {
    if(!c->has_trainer) return false;
    *out = c->trainer;
    return true;
}

const char* gen3_cart_backup_name(const Gen3Cart* c) {
    return c->backup_current ? c->backup_name : "";
}

bool gen3_cart_backup_current(const Gen3Cart* c) {
    return c->backup_current;
}

bool gen3_cart_browsable(const Gen3Cart* c) {
    return c->id.can_browse && c->backup_current && cart_loads(c) && c->work_ok;
}

bool gen3_cart_writable(const Gen3Cart* c, Gen3CartRefuse* why) {
    const bool ok = c->id.can_write && gen3_cart_browsable(c) && c->slots.strict_ok &&
                    !c->slots.padded;
    if(!why) return ok;
    if(ok) {
        *why = Gen3CartRefuseNone;
    } else {
        *why = c->id.why != Gen3CartRefuseNone ? c->id.why : Gen3CartRefuseSave;
    }
    return ok;
}

/* ---- The working copy ---- */

/* The working copy's bytes of n spans, back to back in data; read or written in one opening */
static bool
    cart_work_io(Gen3Cart* c, bool write, const Gen3SaveSpan* sp, uint8_t n, uint8_t* data) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, GEN3_CART_WORK_NAME, write ? Gen3StoreUpdate : Gen3StoreRead);
    if(!f) return false;
    bool ok = true;
    uint32_t at = 0;
    for(uint8_t i = 0; i < n && ok; i++) {
        const uint32_t off = sp[i].id * GEN3_SAVE_SECTOR_SIZE + sp[i].off;
        ok = write ? st->write(st->ctx, f, off, data + at, sp[i].len) :
                     st->read(st->ctx, f, off, data + at, sp[i].len);
        at += sp[i].len;
    }
    return st->close(st->ctx, f) && ok;
}

/* The party count as the save stores it */
static Gen3CartResult cart_party_count(Gen3Cart* c, uint8_t* count) {
    const Gen3SaveSpan sp = gen3_save_party_count_span(c->id.game);
    if(!gen3_cart_browsable(c) || sp.len != 1) return Gen3CartBadArg;
    return cart_work_io(c, false, &sp, 1, count) ? Gen3CartOk : Gen3CartSdError;
}

uint8_t gen3_cart_party_count(Gen3Cart* c) {
    uint8_t n = 0;
    if(cart_party_count(c, &n) != Gen3CartOk) return 0;
    return n < GEN3_SAVE_PARTY_MAX ? n : GEN3_SAVE_PARTY_MAX;
}

bool gen3_cart_box_name(Gen3Cart* c, uint8_t box, char out[9]) {
    out[0] = '\0';
    const Gen3SaveSpan sp = gen3_save_box_name_span(box);
    uint8_t raw[9];
    if(!gen3_cart_browsable(c) || sp.len != sizeof(raw)) return false;
    if(!cart_work_io(c, false, &sp, 1, raw)) return false;
    gen3_text_decode(raw, sizeof(raw), out, 9);
    return true;
}

/* The changed map's bit of a slot gen3_save_mon_spans accepted */
static uint16_t cart_mon_bit(uint8_t box, uint8_t slot) {
    if(box == GEN3_SAVE_PARTY) return slot;
    return (uint16_t)(GEN3_SAVE_PARTY_MAX + box * GEN3_SAVE_BOX_SLOTS + slot);
}

Gen3CartResult gen3_cart_mon_get(
    Gen3Cart* c,
    uint8_t box,
    uint8_t slot,
    uint8_t out[GEN3_PARTY_SIZE],
    size_t* len) {
    Gen3SaveSpan sp[2];
    const uint8_t n = gen3_cart_browsable(c) ? gen3_save_mon_spans(c->id.game, box, slot, sp) : 0;
    if(n == 0) return Gen3CartBadArg;
    if(!cart_work_io(c, false, sp, n, out)) return Gen3CartSdError;
    *len = box == GEN3_SAVE_PARTY ? GEN3_PARTY_SIZE : GEN3_BOX_SIZE;
    return Gen3CartOk;
}

Gen3CartResult gen3_cart_mon_put(Gen3Cart* c, uint8_t box, uint8_t slot, const Gen3Pkm* p) {
    Gen3SaveSpan sp[3];
    uint8_t n = gen3_cart_browsable(c) ? gen3_save_mon_spans(c->id.game, box, slot, sp) : 0;
    if(n == 0) return Gen3CartBadArg;
    /* the Pokemon, then for an append the new count, written in one go */
    uint8_t mon[GEN3_PARTY_SIZE + 1];
    const size_t len = box == GEN3_SAVE_PARTY ? GEN3_PARTY_SIZE : GEN3_BOX_SIZE;
    gen3_save_pkm_to_mon(p, len, mon);
    if(box == GEN3_SAVE_PARTY) {
        uint8_t count = 0;
        const Gen3CartResult r = cart_party_count(c, &count);
        if(r != Gen3CartOk) return r;
        if(slot > count) return Gen3CartBadArg;
        if(slot == count) {
            sp[n++] = gen3_save_party_count_span(c->id.game);
            mon[len] = (uint8_t)(count + 1u);
        }
    }
    if(!cart_work_io(c, true, sp, n, mon)) {
        /* a span may be half written: no edit can be trusted to go to the cart now */
        c->work_ok = false;
        return Gen3CartSdError;
    }
    for(uint8_t i = 0; i < n; i++) c->dirty |= (uint16_t)(1u << sp[i].id);
    const uint16_t bit = cart_mon_bit(box, slot);
    c->changed[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
    return Gen3CartOk;
}

bool gen3_cart_mon_changed(const Gen3Cart* c, uint8_t box, uint8_t slot) {
    if(box == GEN3_SAVE_PARTY ? slot >= GEN3_SAVE_PARTY_MAX :
                                box >= GEN3_SAVE_BOXES || slot >= GEN3_SAVE_BOX_SLOTS) {
        return false;
    }
    const uint16_t bit = cart_mon_bit(box, slot);
    return (c->changed[bit / 8u] >> (bit % 8u)) & 1u;
}

uint16_t gen3_cart_changes(const Gen3Cart* c) {
    uint16_t n = 0;
    for(uint32_t i = 0; i < sizeof(c->changed); i++) {
        for(uint8_t b = c->changed[i]; b; b &= (uint8_t)(b - 1u)) n++;
    }
    return n;
}

/* ---- ROM copy ---- */

uint32_t gen3_cart_rom_size(const Gen3Cart* c) {
    return c->info.rom_size;
}

void gen3_cart_rom_name(const Gen3Cart* c, char out[GEN3_CART_NAME_MAX]) {
    snprintf(out, GEN3_CART_NAME_MAX, CART_ROM_FORMAT, c->id.code, (unsigned)c->id.version);
}

static void cart_part_name(const Gen3Cart* c, char out[GEN3_CART_NAME_MAX]) {
    snprintf(
        out, GEN3_CART_NAME_MAX, CART_ROM_FORMAT ".part", c->id.code, (unsigned)c->id.version);
}

bool gen3_cart_rom_part(Gen3Cart* c, uint32_t* done_bytes) {
    char part[GEN3_CART_NAME_MAX];
    uint32_t size = 0;
    *done_bytes = 0;
    cart_part_name(c, part);
    if(c->info.rom_size == 0 || !c->store.size(c->store.ctx, part, &size)) return false;
    /* a .part longer than the ROM isn't this ROM's */
    if(size > c->info.rom_size) return false;
    *done_bytes = size - size % GEN3_CART_ROM_REGION;
    return *done_bytes > 0;
}

/* Cuts the .part back to len */
static bool cart_cut(Gen3Cart* c, const char* part, uint32_t len) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, part, Gen3StoreUpdate);
    if(!f) return false;
    const bool ok = st->truncate(st->ctx, f, len);
    return st->close(st->ctx, f) && ok;
}

/* The helper's ROM_CRC32 of [off, off + len), and the CRC of the same bytes of the .part as the
 * card gives them back */
static Gen3CartResult
    cart_rom_crcs(Gen3Cart* c, const char* part, uint32_t off, uint32_t len, uint32_t crc[2]) {
    cart_step(c, Gen3CartStepRomVerify, off + len, c->info.rom_size);
    const Gen3CartResult r =
        cart_read(c, gen3_hcli_rom_crc32(c->cli, CART_ROM_BASE + off, len, &crc[0]));
    if(r != Gen3CartOk) return r;
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, part, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    bool ok = true;
    uint32_t reg = 0xFFFFFFFFu;
    for(uint32_t pos = 0; pos < len && ok; pos += GEN3_CART_BUF_SIZE) {
        const uint32_t n = len - pos < GEN3_CART_BUF_SIZE ? len - pos : GEN3_CART_BUF_SIZE;
        ok = st->read(st->ctx, f, off + pos, c->buf, n);
        if(ok) reg = gen3_crc32_update(reg, c->buf, n);
    }
    st->close(st->ctx, f);
    crc[1] = ~reg;
    return ok ? Gen3CartOk : Gen3CartSdError;
}

/* One region: ROM_READs appended to the .part, then *same = its ROM_CRC32 and the card's copy
 * both equal the CRC of what came */
static Gen3CartResult
    cart_rom_region(Gen3Cart* c, const char* part, uint32_t off, uint32_t len, bool* same) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, part, Gen3StoreUpdate);
    if(!f) return Gen3CartSdError;
    Gen3CartResult r = Gen3CartOk;
    uint32_t reg = 0xFFFFFFFFu;
    for(uint32_t pos = 0; pos < len; pos += GEN3_CART_BUF_SIZE) {
        const uint32_t n = len - pos < GEN3_CART_BUF_SIZE ? len - pos : GEN3_CART_BUF_SIZE;
        if(cart_cancelled(c)) {
            r = Gen3CartCancelled;
            break;
        }
        r = cart_read(
            c, gen3_hcli_rom_read(c->cli, CART_ROM_BASE + off + pos, (uint16_t)(n / 2u), c->buf));
        if(r != Gen3CartOk) break;
        reg = gen3_crc32_update(reg, c->buf, n);
        if(!st->write(st->ctx, f, off + pos, c->buf, n)) {
            r = cart_sd_fail(c, n);
            break;
        }
        cart_step(c, Gen3CartStepRomRead, off + pos + n, c->info.rom_size);
    }
    if(!st->close(st->ctx, f) && r == Gen3CartOk) r = Gen3CartSdError;
    if(r != Gen3CartOk) return r;
    uint32_t crc[2];
    r = cart_rom_crcs(c, part, off, len, crc);
    *same = r == Gen3CartOk && crc[0] == ~reg && crc[1] == ~reg;
    return r;
}

/* The regions from start on, each read once more if it differs; the .part keeps only checked
 * ones */
static Gen3CartResult cart_rom_regions(Gen3Cart* c, const char* part, uint32_t start) {
    const uint32_t rom = c->info.rom_size;
    for(uint32_t off = start; off < rom; off += GEN3_CART_ROM_REGION) {
        const uint32_t len = rom - off < GEN3_CART_ROM_REGION ? rom - off : GEN3_CART_ROM_REGION;
        for(uint32_t attempt = 0;; attempt++) {
            bool same = false;
            const Gen3CartResult r = cart_rom_region(c, part, off, len, &same);
            if(r == Gen3CartOk && same) break;
            const bool cut = cart_cut(c, part, off);
            if(r != Gen3CartOk) return r;
            if(!cut) return Gen3CartSdError;
            if(attempt > 0) return Gen3CartVerifyFailed;
        }
    }
    return Gen3CartOk;
}

Gen3CartResult gen3_cart_rom_dump(Gen3Cart* c, bool resume) {
    const uint32_t rom = c->info.rom_size;
    if(rom == 0) return Gen3CartRefused;
    if(!c->linked) return Gen3CartLinkLost;
    const Gen3Store* st = &c->store;
    char part[GEN3_CART_NAME_MAX];
    cart_part_name(c, part);
    uint32_t start = 0;
    if(resume) gen3_cart_rom_part(c, &start); /* 0 without a .part to resume */
    if(st->free_bytes(st->ctx) < (uint64_t)(rom - start) + GEN3_CART_BUF_SIZE) {
        return Gen3CartSdFull;
    }
    void* f = st->open(st->ctx, part, start ? Gen3StoreUpdate : Gen3StoreCreate);
    if(!f) return Gen3CartSdError;
    bool ok = start == 0 || st->truncate(st->ctx, f, start);
    if(!st->close(st->ctx, f) || !ok) return Gen3CartSdError;
    if(start) {
        /* A power cut can leave the last region written but not checked: check it again */
        const uint32_t last = (start - 1u) / GEN3_CART_ROM_REGION * GEN3_CART_ROM_REGION;
        uint32_t crc[2];
        const Gen3CartResult r = cart_rom_crcs(c, part, last, start - last, crc);
        if(r != Gen3CartOk) return r;
        if(crc[0] != crc[1]) {
            if(!cart_cut(c, part, last)) return Gen3CartSdError;
            start = last;
        }
    }
    const Gen3CartResult r = cart_rom_regions(c, part, start);
    if(r != Gen3CartOk) return r;
    char name[GEN3_CART_NAME_MAX];
    gen3_cart_rom_name(c, name);
    st->remove(st->ctx, name);
    return st->rename(st->ctx, part, name) ? Gen3CartOk : Gen3CartSdError;
}
