#include <src/gen3/gen3_omega.h>

#include <stdio.h>
#include <string.h>

#include <src/gen3/gen3_hlink.h>
#include <src/gen3/gen3_save.h>
#include <src/gen3/gen3_ub_data.h>

#define OM_SECTOR GEN3_UB_SECTOR_SIZE
#define OM_BLOCKS 8u /* 512-byte blocks a 4 KB read takes */
#define OM_FIND_STEPS 4u
#define OM_HEX 64u /* a SHA-256 in hex */
#define OM_SUFFIX_MAX 9u /* backups/..._<stamp>-9.sav at most */

const uint8_t gen3_omega_unbound_sha[32] = {
    0x7A, 0xA2, 0x5B, 0xBF, 0x56, 0x8F, 0x7C, 0xFC, 0xF6, 0xEE, 0x1C, 0xF2, 0xE9, 0xE6, 0xFF, 0x63,
    0x73, 0x50, 0xB3, 0xD0, 0x70, 0x5C, 0x23, 0x75, 0xCA, 0xBB, 0x6B, 0xAA, 0x7D, 0x97, 0x39, 0xF7,
};

static const uint8_t* om_expected = gen3_omega_unbound_sha;

#ifdef GEN3_TESTING
void gen3_omega_set_expected_sha(const uint8_t* sha) {
    om_expected = sha ? sha : gen3_omega_unbound_sha;
}
#endif

void gen3_omega_init(
    Gen3Omega* o,
    Gen3Hcli* cli,
    const Gen3Store* store,
    uint8_t* buf,
    const Gen3CartProgress* p) {
    memset(o, 0, sizeof(*o));
    o->cli = cli;
    o->store = *store;
    o->buf = buf;
    if(p) o->progress = *p;
}

static void om_step(Gen3Omega* o, Gen3CartStep step, uint32_t done, uint32_t total) {
    if(o->progress.cb) o->progress.cb(o->progress.ctx, step, done, total);
}

static bool om_cancelled(const Gen3Omega* o) {
    return o->progress.cancel && *o->progress.cancel;
}

/* A client result as the job's. A path the card doesn't have is NoGame. */
static Gen3CartResult om_link(const Gen3Omega* o, Gen3HcliResult r) {
    switch(r) {
    case Gen3HcliOk:
        return Gen3CartOk;
    case Gen3HcliLinkLost:
        return Gen3CartLinkLost;
    case Gen3HcliBusyTimeout:
        return Gen3CartBusyTimeout;
    case Gen3HcliStatusError:
        if(o->cli->last_status == GEN3_HST_UNSUPPORTED &&
           o->cli->last_fail_code == GEN3_HOMEGA_NOT_FOUND) {
            return Gen3CartNoGame;
        }
        return Gen3CartHelperError;
    case Gen3HcliBadResponse:
        break;
    }
    return Gen3CartHelperError;
}

static char om_upper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

/* The game file and its save from Recently play.txt's text (n bytes): false outside the rules */
static bool om_paths(Gen3Omega* o, const uint8_t* text, uint32_t n) {
    uint32_t len = 0;
    while(len < n && text[len] != '\r' && text[len] != '\n') len++;
    while(len && text[len - 1u] == ' ') len--;
    if(len < 6u || len >= GEN3_HCLI_OMEGA_PATH_MAX || text[0] != '/') return false;
    for(uint32_t i = 0; i < len; i++) {
        if(text[i] < 0x20u || text[i] > 0x7Eu) return false;
    }
    static const char ext[] = ".GBA";
    for(uint32_t i = 0; i < 4u; i++) {
        if(om_upper((char)text[len - 4u + i]) != ext[i]) return false;
    }
    memcpy(o->rom_path, text, len);
    o->rom_path[len] = 0;
    const char* base = strrchr(o->rom_path, '/') + 1;
    const uint32_t base_len = (uint32_t)(len - (uint32_t)(base - o->rom_path) - 4u);
    static const char saver[] = "/SAVER/";
    if(base_len == 0 || sizeof(saver) - 1u + base_len + 4u >= GEN3_HCLI_OMEGA_PATH_MAX) return false;
    memcpy(o->save_path, saver, sizeof(saver) - 1u);
    memcpy(o->save_path + sizeof(saver) - 1u, base, base_len);
    memcpy(o->save_path + sizeof(saver) - 1u + base_len, ".sav", 5);
    return true;
}

/* The cache line's part after the hash, for the game file: " <size> <cluster> <date> <time> <path>" */
static void om_key(Gen3Omega* o) {
    snprintf(
        o->line,
        sizeof(o->line),
        " %lu %lu %u %u %s",
        (unsigned long)o->rom.size,
        (unsigned long)o->rom.first_cluster,
        (unsigned)o->rom.date,
        (unsigned)o->rom.time,
        o->rom_path);
}

static int om_hexval(uint8_t c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* The cache file into buf, *len bytes; 0 if absent, unreadable or too long */
static uint32_t om_cache_read(Gen3Omega* o) {
    const Gen3Store* st = &o->store;
    uint32_t size = 0;
    if(!st->size(st->ctx, GEN3_OMEGA_CACHE, &size) || size == 0 || size > GEN3_CART_BUF_SIZE) {
        return 0;
    }
    void* f = st->open(st->ctx, GEN3_OMEGA_CACHE, Gen3StoreRead);
    if(!f) return 0;
    const bool ok = st->read(st->ctx, f, 0, o->buf, size);
    st->close(st->ctx, f);
    return ok ? size : 0;
}

/* The cache's hash for the game file, if it has a line for it (o->line holds its key) */
static bool om_cache_find(Gen3Omega* o) {
    const uint32_t n = om_cache_read(o);
    const uint32_t klen = (uint32_t)strlen(o->line);
    for(uint32_t at = 0; at < n;) {
        uint32_t end = at;
        while(end < n && o->buf[end] != '\n') end++;
        if(end - at == OM_HEX + klen && memcmp(o->buf + at + OM_HEX, o->line, klen) == 0) {
            bool ok = true;
            for(uint32_t i = 0; i < 32u && ok; i++) {
                const int hi = om_hexval(o->buf[at + 2u * i]), lo = om_hexval(o->buf[at + 2u * i + 1u]);
                ok = hi >= 0 && lo >= 0;
                if(ok) o->sha[i] = (uint8_t)(hi << 4 | lo);
            }
            if(ok) return true;
        }
        at = end + 1u;
    }
    return false;
}

/* The cache with this game file's line: the other paths' lines kept, if they fit */
static void om_cache_put(Gen3Omega* o) {
    uint32_t n = om_cache_read(o);
    /* drop lines for the same path: their key ends with " <path>" */
    const uint32_t plen = (uint32_t)strlen(o->rom_path);
    uint32_t keep = 0;
    for(uint32_t at = 0; at < n;) {
        uint32_t end = at;
        while(end < n && o->buf[end] != '\n') end++;
        const bool same = end - at > plen && o->buf[end - plen - 1u] == ' ' &&
                          memcmp(o->buf + end - plen, o->rom_path, plen) == 0;
        if(!same && end < n) {
            memmove(o->buf + keep, o->buf + at, end - at + 1u);
            keep += end - at + 1u;
        }
        at = end + 1u;
    }
    const uint32_t klen = (uint32_t)strlen(o->line);
    if(keep + OM_HEX + klen + 1u > GEN3_CART_BUF_SIZE) keep = 0;
    for(uint32_t i = 0; i < 32u; i++) {
        static const char hex[] = "0123456789abcdef";
        o->buf[keep + 2u * i] = (uint8_t)hex[o->sha[i] >> 4];
        o->buf[keep + 2u * i + 1u] = (uint8_t)hex[o->sha[i] & 15u];
    }
    memcpy(o->buf + keep + OM_HEX, o->line, klen);
    n = keep + OM_HEX + klen;
    o->buf[n++] = '\n';
    const Gen3Store* st = &o->store;
    void* f = st->open(st->ctx, GEN3_OMEGA_CACHE, Gen3StoreCreate);
    if(!f) return;
    const bool ok = st->write(st->ctx, f, 0, o->buf, n);
    if(!st->close(st->ctx, f) || !ok) st->remove(st->ctx, GEN3_OMEGA_CACHE);
}

/* The game file's SHA-256 on the GBA, step by step */
static Gen3CartResult om_hash(Gen3Omega* o) {
    const uint32_t size = o->rom.size;
    const uint32_t steps = (size + GEN3_OMEGA_HASH_STEP - 1u) / GEN3_OMEGA_HASH_STEP;
    om_step(o, Gen3CartStepRomHash, 0, steps);
    for(uint32_t k = 0; k < steps; k++) {
        const uint32_t off = k * GEN3_OMEGA_HASH_STEP;
        const uint32_t len = size - off < GEN3_OMEGA_HASH_STEP ? size - off : GEN3_OMEGA_HASH_STEP;
        const uint8_t flags =
            (uint8_t)((k == 0 ? GEN3_HSHA_INIT : 0u) | (k + 1u == steps ? GEN3_HSHA_FINAL : 0u));
        const Gen3CartResult r = om_link(
            o, gen3_hcli_omega_sha256(o->cli, GEN3_OMEGA_SLOT_ROM, flags, off, len, o->sha));
        if(r != Gen3CartOk) return r;
        om_step(o, Gen3CartStepRomHash, k + 1u, steps);
        if(k + 1u < steps && om_cancelled(o)) return Gen3CartCancelled;
    }
    return Gen3CartOk;
}

/* An Unbound signature in the footer of the save's sector s, read through its 512-byte block 7 */
static Gen3CartResult om_unbound_footer(Gen3Omega* o, uint32_t s, bool* yes) {
    const Gen3CartResult r = om_link(
        o, gen3_hcli_omega_read(o->cli, GEN3_OMEGA_SLOT_SAVE, s * OM_SECTOR + 0xE00u, 1, o->buf));
    if(r != Gen3CartOk) return r;
    const uint8_t* f = o->buf + 0x1F8u;
    const uint32_t sig = (uint32_t)f[0] | (uint32_t)f[1] << 8 | (uint32_t)f[2] << 16 | (uint32_t)f[3] << 24;
    *yes = sig == GEN3_UB_SIG_A || sig == GEN3_UB_SIG_B;
    return Gen3CartOk;
}

/* The edits made on the backup being replaced go with it */
static void om_drop_edits(Gen3Omega* o) {
    o->work_ok = false;
    o->dirty = 0;
    memset(o->changed, 0, sizeof(o->changed));
}

Gen3CartResult gen3_omega_connect(Gen3Omega* o) {
    o->connected = false;
    o->flag_read = false;
    o->mounted = false;
    memset(&o->info, 0, sizeof(o->info));
    o->rom_path[0] = 0;
    o->save_path[0] = 0;
    o->hashed = o->verified = o->from_cache = false;
    o->backup_current = false;
    om_drop_edits(o);
    om_step(o, Gen3CartStepOmegaFind, 0, OM_FIND_STEPS);
    uint8_t flag[2];
    Gen3CartResult r = om_link(o, gen3_hcli_rom_read(o->cli, GEN3_OMEGA_FLAG_ADDR, 1, flag));
    if(r != Gen3CartOk) return r;
    o->flag = (uint16_t)(flag[0] | flag[1] << 8);
    o->flag_read = true;
    if(o->flag == GEN3_OMEGA_FLAG_PENDING) return Gen3CartOmegaPending;
    r = om_link(o, gen3_hcli_omega_mount(o->cli, &o->info));
    if(r != Gen3CartOk) return r;
    o->mounted = true;
    om_step(o, Gen3CartStepOmegaFind, 1, OM_FIND_STEPS);
    if(om_cancelled(o)) return Gen3CartCancelled;

    Gen3HOmegaFile recent;
    r = om_link(o, gen3_hcli_omega_open(o->cli, GEN3_OMEGA_SLOT_RECENT, GEN3_OMEGA_RECENT, &recent));
    if(r != Gen3CartOk) return r;
    if(recent.size == 0) return Gen3CartNoGame;
    r = om_link(o, gen3_hcli_omega_read(o->cli, GEN3_OMEGA_SLOT_RECENT, 0, 1, o->buf));
    if(r != Gen3CartOk) return r;
    if(!om_paths(o, o->buf, recent.size < 512u ? recent.size : 512u)) return Gen3CartNoGame;
    om_step(o, Gen3CartStepOmegaFind, 2, OM_FIND_STEPS);

    r = om_link(o, gen3_hcli_omega_open(o->cli, GEN3_OMEGA_SLOT_SAVE, o->save_path, &o->save));
    if(r != Gen3CartOk) return r;
    if(o->save.size != GEN3_UB_SAVE_SIZE) return Gen3CartNoGame;
    /* Unbound's save, in either slot: another game's, or one never saved, is NoGame */
    bool ub = false;
    r = om_unbound_footer(o, 0, &ub);
    if(r == Gen3CartOk && !ub) r = om_unbound_footer(o, GEN3_UB_SLOT_SECTORS, &ub);
    if(r != Gen3CartOk) return r;
    if(!ub) return Gen3CartNoGame;
    r = om_link(o, gen3_hcli_omega_open(o->cli, GEN3_OMEGA_SLOT_ROM, o->rom_path, &o->rom));
    if(r != Gen3CartOk) return r;
    om_step(o, Gen3CartStepOmegaFind, 3, OM_FIND_STEPS);
    if(om_cancelled(o)) return Gen3CartCancelled;

    om_key(o);
    if(om_cache_find(o)) {
        o->from_cache = true;
        o->hashed = true;
    } else if(o->rom.size) {
        r = om_hash(o);
        if(r != Gen3CartOk) return r;
        o->hashed = true;
        om_cache_put(o);
    }
    o->verified = o->hashed && memcmp(o->sha, om_expected, 32) == 0;
    om_step(o, Gen3CartStepOmegaFind, OM_FIND_STEPS, OM_FIND_STEPS);
    o->connected = true;
    return Gen3CartOk;
}

/* ---- Backup ---- */

/* A failed store write: SdFull if the card is nearly full, else SdError */
static Gen3CartResult om_sd_fail(const Gen3Omega* o) {
    return o->store.free_bytes(o->store.ctx) < OM_SECTOR ? Gen3CartSdFull : Gen3CartSdError;
}

static Gen3CartResult om_backup_read(Gen3Omega* o) {
    const Gen3Store* st = &o->store;
    void* f = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreCreate);
    if(!f) return Gen3CartSdError;
    Gen3CartResult r = Gen3CartOk;
    for(uint32_t s = 0; s < GEN3_UB_SECTORS && r == Gen3CartOk; s++) {
        r = om_link(
            o, gen3_hcli_omega_read(o->cli, GEN3_OMEGA_SLOT_SAVE, s * OM_SECTOR, OM_BLOCKS, o->buf));
        if(r != Gen3CartOk) break;
        o->crc[s] = gen3_crc32(o->buf, OM_SECTOR);
        if(s < 2u * GEN3_UB_SLOT_SECTORS) gen3_ub_sector_info(o->buf, &o->infos[s]);
        if(!st->write(st->ctx, f, s * OM_SECTOR, o->buf, OM_SECTOR)) {
            r = om_sd_fail(o);
            break;
        }
        om_step(o, Gen3CartStepBackupRead, s + 1u, GEN3_UB_SECTORS);
        if(om_cancelled(o)) r = Gen3CartCancelled;
    }
    if(!st->close(st->ctx, f) && r == Gen3CartOk) r = Gen3CartSdError;
    return r;
}

static Gen3CartResult om_backup_verify(Gen3Omega* o) {
    Gen3CartResult r = Gen3CartOk;
    for(uint32_t s = 0; s < GEN3_UB_SECTORS && r == Gen3CartOk; s++) {
        uint32_t crc = 0;
        r = om_link(
            o, gen3_hcli_omega_crc32(o->cli, GEN3_OMEGA_SLOT_SAVE, s * OM_SECTOR, OM_SECTOR, &crc));
        if(r == Gen3CartOk && crc != o->crc[s]) r = Gen3CartVerifyFailed;
        om_step(o, Gen3CartStepBackupVerify, s + 1u, 2u * GEN3_UB_SECTORS);
    }
    if(r != Gen3CartOk) return r;
    const Gen3Store* st = &o->store;
    void* f = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    for(uint32_t s = 0; s < GEN3_UB_SECTORS && r == Gen3CartOk; s++) {
        if(!st->read(st->ctx, f, s * OM_SECTOR, o->buf, OM_SECTOR)) {
            r = Gen3CartSdError;
        } else if(gen3_crc32(o->buf, OM_SECTOR) != o->crc[s]) {
            r = Gen3CartVerifyFailed;
        }
        om_step(o, Gen3CartStepBackupVerify, GEN3_UB_SECTORS + s + 1u, 2u * GEN3_UB_SECTORS);
    }
    st->close(st->ctx, f);
    return r;
}

/* Section id of the backup file's loaded slot into buf */
static bool om_section(Gen3Omega* o, const char* name, uint8_t id) {
    const Gen3Store* st = &o->store;
    void* f = st->open(st->ctx, name, Gen3StoreRead);
    if(!f) return false;
    const bool ok = st->read(st->ctx, f, o->slots.phys[id] * OM_SECTOR, o->buf, OM_SECTOR);
    st->close(st->ctx, f);
    return ok;
}

/* GEN3_OMEGA_WORK made from the backup, 4 KB at a time through buf; removed if it fails */
static Gen3CartResult om_work_make(Gen3Omega* o) {
    const Gen3Store* st = &o->store;
    void* src = st->open(st->ctx, o->backup_name, Gen3StoreRead);
    if(!src) return Gen3CartSdError;
    void* dst = st->open(st->ctx, GEN3_OMEGA_WORK, Gen3StoreCreate);
    if(!dst) {
        st->close(st->ctx, src);
        return Gen3CartSdError;
    }
    Gen3CartResult r = Gen3CartOk;
    for(uint32_t s = 0; s < GEN3_UB_SECTORS && r == Gen3CartOk; s++) {
        if(!st->read(st->ctx, src, s * OM_SECTOR, o->buf, OM_SECTOR)) {
            r = Gen3CartSdError;
        } else if(!st->write(st->ctx, dst, s * OM_SECTOR, o->buf, OM_SECTOR)) {
            r = om_sd_fail(o);
        }
    }
    st->close(st->ctx, src);
    if(!st->close(st->ctx, dst) && r == Gen3CartOk) r = Gen3CartSdError;
    if(r != Gen3CartOk) st->remove(st->ctx, GEN3_OMEGA_WORK);
    return r;
}

Gen3CartResult gen3_omega_backup(Gen3Omega* o, const char* stamp) {
    if(!o->connected) return Gen3CartBadArg;
    o->backup_current = false;
    om_drop_edits(o);
    o->has_trainer = false;
    const Gen3Store* st = &o->store;
    if(st->free_bytes(st->ctx) < GEN3_UB_SAVE_SIZE + OM_SECTOR) return Gen3CartSdFull;
    Gen3CartResult r = om_backup_read(o);
    if(r == Gen3CartOk) r = om_backup_verify(o);
    if(r == Gen3CartOk) {
        om_step(o, Gen3CartStepAnalyze, 0, 1);
        gen3_ub_slots(o->infos, &o->slots);
        if(o->slots.loaded >= 0) {
            if(!om_section(o, GEN3_CART_BACKUP_TMP, 0)) {
                r = Gen3CartSdError;
            } else {
                gen3_ub_trainer(o->buf, &o->trainer);
                o->has_trainer = true;
            }
        }
    }
    if(r == Gen3CartOk) {
        char tid[8];
        if(o->has_trainer) {
            snprintf(tid, sizeof(tid), "%u", (unsigned)o->trainer.tid);
        } else {
            memcpy(tid, "nosave", 7);
        }
        bool renamed = false;
        for(uint32_t k = 1; k <= OM_SUFFIX_MAX && !renamed; k++) {
            if(k == 1) {
                snprintf(o->backup_name, sizeof(o->backup_name), "%s/UNBOUND_%s_%s.sav", GEN3_CART_BACKUP_DIR, tid, stamp);
            } else {
                snprintf(
                    o->backup_name,
                    sizeof(o->backup_name),
                    "%s/UNBOUND_%s_%s-%lu.sav",
                    GEN3_CART_BACKUP_DIR,
                    tid,
                    stamp,
                    (unsigned long)k);
            }
            renamed = st->rename(st->ctx, GEN3_CART_BACKUP_TMP, o->backup_name);
        }
        if(!renamed) r = Gen3CartSdError;
    }
    if(r != Gen3CartOk) {
        st->remove(st->ctx, GEN3_CART_BACKUP_TMP);
        o->backup_name[0] = 0;
        o->has_trainer = false;
        /* The helper may still be busy with a lost command: a new Connect first */
        if(r == Gen3CartLinkLost || r == Gen3CartBusyTimeout || r == Gen3CartHelperError) {
            o->connected = false;
        }
        return r;
    }
    om_step(o, Gen3CartStepAnalyze, 1, 1);
    o->backup_current = true;
    /* If the copy can't be made the backup stands, but nothing can be edited */
    o->work_ok = om_work_make(o) == Gen3CartOk;
    return Gen3CartOk;
}

/* ---- Browsing the backup ---- */

bool gen3_omega_browsable(const Gen3Omega* o) {
    return o->backup_current && o->slots.loaded >= 0;
}

uint8_t gen3_omega_party_count(Gen3Omega* o) {
    if(!gen3_omega_browsable(o) || !om_section(o, o->backup_name, 1)) return 0;
    return gen3_ub_party_count(o->buf);
}

/* The file browsing reads: the working copy, so that edits show, else the backup */
static const char* om_source(const Gen3Omega* o) {
    return o->work_ok ? GEN3_OMEGA_WORK : o->backup_name;
}

Gen3CartMon gen3_omega_summary(Gen3Omega* o, uint8_t box, uint8_t slot, Gen3UbSummary* out) {
    if(!gen3_omega_browsable(o)) return Gen3CartMonBadArg;
    const char* src = om_source(o);
    if(box == GEN3_SAVE_PARTY) {
        if(slot >= GEN3_UB_PARTY_MAX) return Gen3CartMonBadArg;
        if(!om_section(o, src, 1)) return Gen3CartMonSdError;
        if(slot >= gen3_ub_party_count(o->buf)) return Gen3CartMonEmpty;
        gen3_ub_party_summary(o->buf + GEN3_UB_PARTY_OFF(slot), out);
        return out->species ? Gen3CartMonOk : Gen3CartMonEmpty;
    }
    Gen3UbPieces pc;
    if(!gen3_ub_box_pieces(&o->slots, box, slot, &pc)) return Gen3CartMonBadArg;
    uint8_t rec[GEN3_UB_BOX_MON];
    const Gen3Store* st = &o->store;
    void* f = st->open(st->ctx, src, Gen3StoreRead);
    if(!f) return Gen3CartMonSdError;
    bool ok = true;
    uint32_t at = 0;
    for(uint8_t k = 0; k < pc.n && ok; k++) {
        ok = st->read(st->ctx, f, pc.sector[k] * OM_SECTOR + pc.off[k], rec + at, pc.len[k]);
        at += pc.len[k];
    }
    st->close(st->ctx, f);
    if(!ok) return Gen3CartMonSdError;
    gen3_ub_box_summary(rec, gen3_ub_growth, NULL, out);
    return out->species ? Gen3CartMonOk : Gen3CartMonEmpty;
}

/* ---- Editing the working copy ---- */

#define OM_PARTY_IV 72u /* the IV word, whose bit 30 is the egg flag */
#define OM_BOX_IV 54u

static bool om_egg_bit(const uint8_t* iv_word) {
    return (iv_word[3] >> 6) & 1u;
}

/* Box record (box, slot) of the file name into rec; false if no such slot or unreadable */
static bool om_box_rec(Gen3Omega* o, const char* name, const Gen3UbPieces* pc, uint8_t* rec, bool write) {
    const Gen3Store* st = &o->store;
    void* f = st->open(st->ctx, name, write ? Gen3StoreUpdate : Gen3StoreRead);
    if(!f) return false;
    bool ok = true;
    uint32_t at = 0;
    for(uint8_t k = 0; k < pc->n && ok; k++) {
        const uint32_t off = pc->sector[k] * OM_SECTOR + pc->off[k];
        ok = write ? st->write(st->ctx, f, off, rec + at, pc->len[k])
                   : st->read(st->ctx, f, off, rec + at, pc->len[k]);
        at += pc->len[k];
    }
    if(!st->close(st->ctx, f)) ok = false;
    return ok;
}

static bool om_editable(const Gen3Omega* o) {
    return gen3_omega_browsable(o) && o->work_ok;
}

Gen3CartMon gen3_omega_mon_get(Gen3Omega* o, uint8_t box, uint8_t slot, Gen3UbMon* out) {
    if(!om_editable(o)) return Gen3CartMonBadArg;
    memset(out, 0, sizeof(*out));
    if(box == GEN3_SAVE_PARTY) {
        if(slot >= GEN3_UB_PARTY_MAX) return Gen3CartMonBadArg;
        if(!om_section(o, GEN3_OMEGA_WORK, 1)) return Gen3CartMonSdError;
        if(slot >= gen3_ub_party_count(o->buf)) return Gen3CartMonEmpty;
        out->party = true;
        memcpy(out->raw, o->buf + GEN3_UB_PARTY_OFF(slot), GEN3_UB_PARTY_MON);
    } else {
        Gen3UbPieces pc;
        if(!gen3_ub_box_pieces(&o->slots, box, slot, &pc)) return Gen3CartMonBadArg;
        if(!om_box_rec(o, GEN3_OMEGA_WORK, &pc, out->raw, false)) return Gen3CartMonSdError;
    }
    return gen3_ub_mon_species(out) ? Gen3CartMonOk : Gen3CartMonEmpty;
}

static uint32_t om_bit(uint8_t box, uint8_t slot) {
    return box == GEN3_SAVE_PARTY ? slot : 6u + (uint32_t)box * GEN3_UB_BOX_SLOTS + slot;
}

static void om_mark(Gen3Omega* o, uint8_t box, uint8_t slot, uint32_t sectors) {
    const uint32_t b = om_bit(box, slot);
    o->changed[b / 8u] = (uint8_t)(o->changed[b / 8u] | 1u << (b % 8u));
    o->dirty |= sectors;
}

Gen3CartMon gen3_omega_mon_put(
    Gen3Omega* o,
    uint8_t box,
    uint8_t slot,
    const Gen3UbMon* m,
    bool new_mon) {
    if(!om_editable(o) || !o->verified) return Gen3CartMonBadArg;
    const Gen3Store* st = &o->store;
    if(box == GEN3_SAVE_PARTY) {
        if(!m->party || slot >= GEN3_UB_PARTY_MAX) return Gen3CartMonBadArg;
        if(!om_section(o, GEN3_OMEGA_WORK, 1)) return Gen3CartMonSdError;
        if(slot >= gen3_ub_party_count(o->buf)) return Gen3CartMonBadArg;
        if(om_egg_bit(o->buf + GEN3_UB_PARTY_OFF(slot) + OM_PARTY_IV) ||
           om_egg_bit(m->raw + OM_PARTY_IV)) {
            return Gen3CartMonEgg;
        }
        void* f = st->open(st->ctx, GEN3_OMEGA_WORK, Gen3StoreUpdate);
        bool ok = f != NULL;
        if(ok) {
            ok = st->write(
                st->ctx,
                f,
                o->slots.phys[1] * OM_SECTOR + GEN3_UB_PARTY_OFF(slot),
                m->raw,
                GEN3_UB_PARTY_MON);
            if(!st->close(st->ctx, f)) ok = false;
        }
        if(!ok) {
            o->work_ok = false;
            return Gen3CartMonSdError;
        }
        om_mark(o, box, slot, 1u << o->slots.phys[1]);
        return Gen3CartMonOk;
    }
    Gen3UbPieces pc;
    if(m->party || !gen3_ub_box_pieces(&o->slots, box, slot, &pc)) return Gen3CartMonBadArg;
    uint8_t cur[GEN3_UB_BOX_MON];
    if(!om_box_rec(o, GEN3_OMEGA_WORK, &pc, cur, false)) return Gen3CartMonSdError;
    const bool empty = (cur[28] | cur[29]) == 0;
    if(empty != new_mon) return Gen3CartMonTaken;
    if(om_egg_bit(cur + OM_BOX_IV) || om_egg_bit(m->raw + OM_BOX_IV)) return Gen3CartMonEgg;
    uint8_t rec[GEN3_UB_BOX_MON];
    memcpy(rec, m->raw, sizeof(rec));
    if(!om_box_rec(o, GEN3_OMEGA_WORK, &pc, rec, true)) {
        o->work_ok = false;
        return Gen3CartMonSdError;
    }
    uint32_t sectors = 0;
    for(uint8_t k = 0; k < pc.n; k++) sectors |= 1u << pc.sector[k];
    om_mark(o, box, slot, sectors);
    return Gen3CartMonOk;
}

bool gen3_omega_mon_changed(const Gen3Omega* o, uint8_t box, uint8_t slot) {
    if(box == GEN3_SAVE_PARTY ? slot >= GEN3_UB_PARTY_MAX
                              : box >= GEN3_UB_BOXES || slot >= GEN3_UB_BOX_SLOTS) {
        return false;
    }
    const uint32_t b = om_bit(box, slot);
    return (o->changed[b / 8u] >> (b % 8u)) & 1u;
}

uint16_t gen3_omega_changes(const Gen3Omega* o) {
    uint16_t n = 0;
    for(uint32_t i = 0; i < sizeof(o->changed); i++) {
        for(uint8_t v = o->changed[i]; v; v &= (uint8_t)(v - 1u)) n++;
    }
    return n;
}

Gen3CartResult gen3_omega_discard(Gen3Omega* o) {
    if(!o->backup_current) return Gen3CartBadArg;
    om_drop_edits(o);
    const Gen3CartResult r = om_work_make(o);
    o->work_ok = r == Gen3CartOk;
    return r;
}

bool gen3_omega_owner(Gen3Omega* o, Gen3UbMon* out) {
    if(!gen3_omega_browsable(o) || !om_section(o, om_source(o), 1)) return false;
    const uint8_t n = gen3_ub_party_count(o->buf);
    const uint32_t trainer = (uint32_t)o->trainer.tid | (uint32_t)o->trainer.sid << 16;
    int first = -1, pick = -1;
    for(uint8_t i = 0; i < n && pick < 0; i++) {
        const uint8_t* rec = o->buf + GEN3_UB_PARTY_OFF(i);
        if(om_egg_bit(rec + OM_PARTY_IV)) continue;
        if(first < 0) first = i;
        const uint32_t otid = (uint32_t)rec[4] | (uint32_t)rec[5] << 8 | (uint32_t)rec[6] << 16 |
                              (uint32_t)rec[7] << 24;
        if(o->has_trainer && otid == trainer) pick = i;
    }
    if(pick < 0) pick = first;
    if(pick < 0) return false;
    memset(out, 0, sizeof(*out));
    out->party = true;
    memcpy(out->raw, o->buf + GEN3_UB_PARTY_OFF(pick), GEN3_UB_PARTY_MON);
    return true;
}
