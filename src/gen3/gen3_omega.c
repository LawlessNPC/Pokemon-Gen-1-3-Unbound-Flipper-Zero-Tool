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

/* Physical sector s of the file name (a backup or the working copy) into buf */
static bool om_file_sector(Gen3Omega* o, const char* name, uint32_t s) {
    const Gen3Store* st = &o->store;
    void* f = st->open(st->ctx, name, Gen3StoreRead);
    if(!f) return false;
    const bool ok = st->read(st->ctx, f, s * OM_SECTOR, o->buf, OM_SECTOR);
    st->close(st->ctx, f);
    return ok;
}

/* Section id of the file's loaded slot into buf */
static bool om_section(Gen3Omega* o, const char* name, uint8_t id) {
    return om_file_sector(o, name, o->slots.phys[id]);
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

/* ---- Writing the card ---- */

#define OM_PREP_STEPS (GEN3_UB_SECTORS + 2u) /* the OPEN, a CRC a sector, the ARM */
#define OM_POST_STEPS (3u * GEN3_UB_SECTORS + 1u) /* the backup's reads, its two checks, Analyze */
#define OM_SHARED 30u /* the first of the two sectors both slots share */

/* A lost link, or a helper whose state is in doubt, ends the connect: a new Connect first */
static void om_end_link(Gen3Omega* o, Gen3CartResult r) {
    if(r == Gen3CartLinkLost || r == Gen3CartBusyTimeout || r == Gen3CartHelperError) {
        o->connected = false;
    }
}

/* The slot the game doesn't load ends (index 13) in a sound sector no older than the loaded slot:
 * what a save of the game's own leaves when cut short. The game writes a slot from index r
 * (r_loaded + 1, as this write's rotation) up to 13, then 0 .. r - 1; this write goes from index
 * 0 up. So once that save reached index 13, a cut of this write at a sector's end could leave a
 * whole slot, every id once at N + 1, half of each save, and the game would load it. */
static bool om_other_slot_cut(const Gen3Omega* o) {
    const uint32_t loaded = (uint32_t)o->slots.loaded;
    const uint32_t last = (1u - loaded) * GEN3_UB_SLOT_SECTORS + GEN3_UB_SLOT_SECTORS - 1u;
    const Gen3UbSector* f = &o->infos[last];
    return f->sig_ok && f->sum_ok && f->id < GEN3_UB_SLOT_SECTORS &&
           (int32_t)(f->counter - o->slots.counter[loaded]) >= 0;
}

/* OMEGA_CRC32 of the save's sector s */
static Gen3CartResult om_crc(Gen3Omega* o, uint32_t s, uint32_t* crc) {
    return om_link(
        o, gen3_hcli_omega_crc32(o->cli, GEN3_OMEGA_SLOT_SAVE, s * OM_SECTOR, OM_SECTOR, crc));
}

/* The card still holds the backup's save: slot 0 opened again on its path, with the size and
 * first cluster the connect found, and each sector's OMEGA_CRC32 the backup's; then armed for that
 * file. Changed, the backup and the edits dropped, if not. */
static Gen3CartResult om_write_check(Gen3Omega* o) {
    om_step(o, Gen3CartStepPrepare, 0, OM_PREP_STEPS);
    Gen3HOmegaFile f;
    Gen3CartResult r =
        om_link(o, gen3_hcli_omega_open(o->cli, GEN3_OMEGA_SLOT_SAVE, o->save_path, &f));
    if(r == Gen3CartNoGame ||
       (r == Gen3CartOk &&
        (f.size != o->save.size || f.first_cluster != o->save.first_cluster))) {
        r = Gen3CartChanged;
    }
    if(r == Gen3CartOk) om_step(o, Gen3CartStepPrepare, 1, OM_PREP_STEPS);
    for(uint32_t s = 0; s < GEN3_UB_SECTORS && r == Gen3CartOk; s++) {
        uint32_t crc = 0;
        r = om_crc(o, s, &crc);
        if(r == Gen3CartOk && crc != o->crc[s]) r = Gen3CartChanged;
        if(r == Gen3CartOk) om_step(o, Gen3CartStepPrepare, s + 2u, OM_PREP_STEPS);
    }
    if(r == Gen3CartOk) {
        r = om_link(o, gen3_hcli_omega_arm(o->cli, o->save.size, o->save.first_cluster));
    }
    if(r == Gen3CartChanged) {
        o->backup_current = false;
        om_drop_edits(o);
    }
    if(r == Gen3CartOk) om_step(o, Gen3CartStepPrepare, OM_PREP_STEPS, OM_PREP_STEPS);
    return r;
}

/* buf written as sector s of the armed file: Ok only once the helper's read-back CRC is the
 * image's. Any other answer leaves the sector's state unknown. */
static Gen3CartResult om_put_sector(Gen3Omega* o, uint8_t s) {
    const uint32_t want = gen3_crc32(o->buf, OM_SECTOR);
    o->want_crc[s] = want;
    uint32_t got = 0;
    switch(gen3_hcli_omega_write_sector(o->cli, s, o->buf, &got)) {
    case Gen3HcliOk:
        return got == want ? Gen3CartOk : Gen3CartVerifyFailed;
    case Gen3HcliLinkLost:
        return Gen3CartLinkLost;
    case Gen3HcliBusyTimeout:
        return Gen3CartBusyTimeout;
    case Gen3HcliStatusError:
        return o->cli->last_status == GEN3_HST_VERIFY_FAIL ? Gen3CartVerifyFailed :
                                                              Gen3CartFlashError;
    case Gen3HcliBadResponse:
        break;
    }
    /* An answer the client couldn't take: the helper's state is in doubt */
    o->connected = false;
    return Gen3CartFlashError;
}

/* Section id of the new slot into buf: the working copy's data from the loaded slot's sector with
 * that id, and its footer remade with the id, the checksum over the section's length (section 4's
 * copied, as PUSE never recomputes it), the signature the working copy's sector carries (the
 * card's own, or a restored file's) and the new slot's counter. A
 * sector with no edit must still be the backup's; SdError if it isn't, or can't be read. */
static Gen3CartResult om_slot_image(Gen3Omega* o, uint8_t id, uint32_t counter) {
    const uint8_t src = o->slots.phys[id];
    if(!om_file_sector(o, GEN3_OMEGA_WORK, src)) return Gen3CartSdError;
    if(!((o->dirty >> src) & 1u) && gen3_crc32(o->buf, OM_SECTOR) != o->crc[src]) {
        return Gen3CartSdError;
    }
    Gen3UbSector old;
    gen3_ub_sector_info(o->buf, &old);
    const Gen3UbSector f = {
        .id = id,
        .checksum = id == GEN3_UB_OPAQUE_ID ? old.checksum : gen3_ub_section_sum(o->buf, id),
        .signature = old.signature,
        .counter = counter,
    };
    gen3_ub_set_footer(o->buf, &f);
    return Gen3CartOk;
}

/* The write test, then the slot the game doesn't load, a sector at a time: Program 1 .. 15 of
 * total */
static Gen3CartResult om_write_slot(Gen3Omega* o, uint32_t total) {
    const uint32_t loaded = (uint32_t)o->slots.loaded;
    const uint8_t first = (uint8_t)((1u - loaded) * GEN3_UB_SLOT_SECTORS);
    /* The write test: the new slot's first sector, with the bytes the card holds there */
    if(!om_file_sector(o, o->backup_name, first) ||
       gen3_crc32(o->buf, OM_SECTOR) != o->crc[first]) {
        return Gen3CartSdError;
    }
    Gen3CartResult r = om_put_sector(o, first);
    if(r != Gen3CartOk) return r;
    om_step(o, Gen3CartStepProgram, 1, total);
    /* Section 0 one sector further on than in the loaded slot, as the game's own save puts it */
    const uint32_t r_loaded = o->slots.phys[0] - loaded * GEN3_UB_SLOT_SECTORS;
    const uint32_t rot = (r_loaded + 1u) % GEN3_UB_SLOT_SECTORS;
    const uint32_t counter = o->slots.counter[loaded] + 1u;
    for(uint32_t k = 0; k < GEN3_UB_SLOT_SECTORS && r == Gen3CartOk; k++) {
        const uint8_t id = (uint8_t)((k + GEN3_UB_SLOT_SECTORS - rot) % GEN3_UB_SLOT_SECTORS);
        r = om_slot_image(o, id, counter);
        if(r == Gen3CartOk) r = om_put_sector(o, (uint8_t)(first + k));
        if(r == Gen3CartOk) om_step(o, Gen3CartStepProgram, k + 2u, total);
    }
    return r;
}

/* Sectors 30 and 31, those an edit changed, from the working copy */
static Gen3CartResult om_write_shared(Gen3Omega* o, uint32_t total) {
    uint32_t done = 1u + GEN3_UB_SLOT_SECTORS;
    for(uint8_t s = OM_SHARED; s < GEN3_UB_SECTORS; s++) {
        if(!((o->dirty >> s) & 1u)) continue;
        const Gen3CartResult r =
            om_file_sector(o, GEN3_OMEGA_WORK, s) ? om_put_sector(o, s) : Gen3CartSdError;
        if(r != Gen3CartOk) return r;
        om_step(o, Gen3CartStepProgram, ++done, total);
    }
    return Gen3CartOk;
}

/* Each sector's OMEGA_CRC32 against want_crc: VerifyFailed, or WrittenShared if only sectors 30
 * and 31 differ. Every write is done by now, so the game loads the new save: a check the link cut
 * short is WrittenNoBackup. */
static Gen3CartResult om_write_verify(Gen3Omega* o) {
    bool bad = false, shared = false;
    for(uint32_t s = 0; s < GEN3_UB_SECTORS; s++) {
        uint32_t crc = 0;
        const Gen3CartResult r = om_crc(o, s, &crc);
        if(r != Gen3CartOk) {
            om_end_link(o, r);
            return Gen3CartWrittenNoBackup;
        }
        if(crc != o->want_crc[s]) {
            if(s >= OM_SHARED) {
                shared = true;
            } else {
                bad = true;
            }
        }
        om_step(o, Gen3CartStepVerify, s + 1u, GEN3_UB_SECTORS);
    }
    return bad ? Gen3CartVerifyFailed : shared ? Gen3CartWrittenShared : Gen3CartOk;
}

/* The new backup's steps as PostBackup 0 .. OM_POST_STEPS; ctx is the write's own progress */
static void om_post_step(void* ctx, Gen3CartStep step, uint32_t done, uint32_t total) {
    (void)total;
    const Gen3CartProgress* p = ctx;
    if(step == Gen3CartStepBackupVerify) done += GEN3_UB_SECTORS;
    if(step == Gen3CartStepAnalyze) done += 3u * GEN3_UB_SECTORS;
    if(p->cb) p->cb(p->ctx, Gen3CartStepPostBackup, done, OM_POST_STEPS);
}

/* The new backup, shown as PostBackup and not to be cancelled */
static Gen3CartResult om_write_backup(Gen3Omega* o, const char* stamp) {
    Gen3CartProgress outer = o->progress;
    om_step(o, Gen3CartStepPostBackup, 0, OM_POST_STEPS);
    o->progress = (Gen3CartProgress){.cb = om_post_step, .ctx = &outer, .cancel = NULL};
    const Gen3CartResult r = gen3_omega_backup(o, stamp);
    o->progress = outer;
    return r;
}

/* The write, from the checks on: the working copy's dirty sectors onto the card */
static Gen3CartResult om_write_job(Gen3Omega* o, const char* stamp) {
    if(om_other_slot_cut(o)) return Gen3CartOmegaSaveFirst;
    if(!stamp || strlen(stamp) > GEN3_CART_STAMP_MAX) return Gen3CartBadArg;
    if(!o->connected) return Gen3CartLinkLost;
    const Gen3Store* st = &o->store;
    if(st->free_bytes(st->ctx) < GEN3_UB_SAVE_SIZE + OM_SECTOR) return Gen3CartSdFull;
    memcpy(o->want_crc, o->crc, sizeof(o->want_crc));
    const uint32_t shared = ((o->dirty >> OM_SHARED) & 1u) + (o->dirty >> (OM_SHARED + 1u));
    const uint32_t total = 1u + GEN3_UB_SLOT_SECTORS + shared;
    Gen3CartResult r = om_write_check(o);
    if(r == Gen3CartOk) r = om_write_slot(o, total);
    if(r == Gen3CartOk) {
        /* The new slot is whole: the game loads it now */
        r = om_write_shared(o, total);
        if(r != Gen3CartOk) {
            om_end_link(o, r);
            r = Gen3CartWrittenShared;
        } else {
            r = om_write_verify(o);
        }
        if(r != Gen3CartOk) {
            /* The backup holds the save before this one, and the edits are on the card */
            o->backup_current = false;
            om_drop_edits(o);
        }
    }
    if(r != Gen3CartOk) {
        om_end_link(o, r);
        return r;
    }
    return om_write_backup(o, stamp) == Gen3CartOk ? Gen3CartOk : Gen3CartWrittenNoBackup;
}

Gen3CartResult gen3_omega_write(Gen3Omega* o, const char* stamp) {
    if(gen3_omega_changes(o) == 0) return Gen3CartNothing;
    if(!o->verified || !om_editable(o)) return Gen3CartRefused;
    return om_write_job(o, stamp);
}

/* ---- Restoring a backup ---- */

/* A store path backups/UNBOUND_*.sav, as gen3_omega_backup names its files */
static bool om_restore_name(const char* name) {
    static const char pre[] = GEN3_CART_BACKUP_DIR "/UNBOUND_";
    if(!name) return false;
    const size_t n = strlen(name), p = sizeof(pre) - 1u;
    return n < GEN3_CART_NAME_MAX && n > p + 4u && memcmp(name, pre, p) == 0 &&
           strcmp(name + n - 4u, ".sav") == 0;
}

/* The file's 28 footers into rinfos and rslots; its section 0 in buf if the slot loads */
static Gen3CartResult om_restore_read(Gen3Omega* o, const char* name) {
    const Gen3Store* st = &o->store;
    uint32_t size = 0;
    if(!st->size(st->ctx, name, &size)) return Gen3CartSdError;
    if(size != GEN3_UB_SAVE_SIZE) return Gen3CartBadBackup;
    void* f = st->open(st->ctx, name, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    Gen3CartResult r = Gen3CartOk;
    for(uint32_t s = 0; s < 2u * GEN3_UB_SLOT_SECTORS && r == Gen3CartOk; s++) {
        if(st->read(st->ctx, f, s * OM_SECTOR, o->buf, OM_SECTOR)) {
            gen3_ub_sector_info(o->buf, &o->rinfos[s]);
        } else {
            r = Gen3CartSdError;
        }
    }
    st->close(st->ctx, f);
    if(r != Gen3CartOk) return r;
    gen3_ub_slots(o->rinfos, &o->rslots);
    return o->rslots.loaded >= 0 ? Gen3CartOk : Gen3CartBadBackup;
}

Gen3CartResult gen3_omega_restore_check(Gen3Omega* o, const char* name, Gen3UbTrainer* t, bool* has) {
    *has = false;
    if(!om_restore_name(name)) return Gen3CartBadArg;
    Gen3CartResult r = om_restore_read(o, name);
    if(r != Gen3CartOk) return r;
    if(!om_file_sector(o, name, o->rslots.phys[0])) return Gen3CartSdError;
    gen3_ub_trainer(o->buf, t);
    *has = true;
    return Gen3CartOk;
}

/* The file's sector from into the working copy's sector to, through buf */
static bool om_restore_copy(Gen3Omega* o, void* src, void* dst, uint32_t from, uint32_t to) {
    const Gen3Store* st = &o->store;
    return st->read(st->ctx, src, from * OM_SECTOR, o->buf, OM_SECTOR) &&
           st->write(st->ctx, dst, to * OM_SECTOR, o->buf, OM_SECTOR);
}

/* The working copy made the file's save: each section of its loaded slot where the current
 * backup's section sits, and sectors 30 and 31; all of them dirty */
static Gen3CartResult om_restore_work(Gen3Omega* o, const char* name) {
    const Gen3Store* st = &o->store;
    void* src = st->open(st->ctx, name, Gen3StoreRead);
    if(!src) return Gen3CartSdError;
    void* dst = st->open(st->ctx, GEN3_OMEGA_WORK, Gen3StoreUpdate);
    if(!dst) {
        st->close(st->ctx, src);
        return Gen3CartSdError;
    }
    bool ok = true;
    uint32_t dirty = 0;
    for(uint8_t id = 0; id < GEN3_UB_SLOT_SECTORS && ok; id++) {
        ok = om_restore_copy(o, src, dst, o->rslots.phys[id], o->slots.phys[id]);
        dirty |= 1u << o->slots.phys[id];
    }
    for(uint32_t s = OM_SHARED; s < GEN3_UB_SECTORS && ok; s++) {
        ok = om_restore_copy(o, src, dst, s, s);
        dirty |= 1u << s;
    }
    st->close(st->ctx, src);
    if(!st->close(st->ctx, dst)) ok = false;
    if(!ok) return om_sd_fail(o);
    o->dirty = dirty;
    return Gen3CartOk;
}

Gen3CartResult gen3_omega_restore(Gen3Omega* o, const char* name, const char* stamp) {
    if(!o->verified || !om_editable(o)) return Gen3CartRefused;
    if(!om_restore_name(name)) return Gen3CartBadArg;
    if(om_other_slot_cut(o)) return Gen3CartOmegaSaveFirst;
    if(!stamp || strlen(stamp) > GEN3_CART_STAMP_MAX) return Gen3CartBadArg;
    Gen3CartResult r = om_restore_read(o, name);
    if(r != Gen3CartOk) return r;
    if(!o->connected) return Gen3CartLinkLost;
    const Gen3Store* st = &o->store;
    if(st->free_bytes(st->ctx) < GEN3_UB_SAVE_SIZE + OM_SECTOR) return Gen3CartSdFull;
    om_drop_edits(o);
    r = om_restore_work(o, name);
    if(r == Gen3CartOk) {
        o->work_ok = true;
        r = om_write_job(o, stamp);
    }
    if(r != Gen3CartOk && o->backup_current) {
        /* The card is as the backup says: the working copy is the backup again */
        o->work_ok = om_work_make(o) == Gen3CartOk;
        o->dirty = 0;
        memset(o->changed, 0, sizeof(o->changed));
    }
    return r;
}
