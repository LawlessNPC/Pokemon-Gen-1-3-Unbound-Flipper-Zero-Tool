#include "hp_omega.h"

#include "hp_hal.h"

/* FAT32 as Microsoft's "FAT: General Overview of On-Disk Format" lays it out: the boot sector's
 * BPB, 32-byte directory entries, LFN runs before their 8.3 entry. */

#define HP_OMEGA_BLOCK 512u
#define HP_OMEGA_RUN 4u /* blocks one hp_hal_omega_sd_read takes */
#define HP_OMEGA_EOC 0x0FFFFFF8u
#define HP_OMEGA_DIR_SECTORS_MAX 4096u /* FAT's 2 MiB directory limit; a loop in a chain ends */
#define HP_OMEGA_BAD 0x0FFFFFF7u /* a FAT entry marking a bad cluster */
#define HP_OMEGA_LFN_SEQ_MAX 20u /* 260 characters */
#define HP_OMEGA_NO_MATCH 0x7Fu /* a long-name character no path character equals */

static uint8_t b8(const uint16_t* h, uint32_t i) {
    return (uint8_t)(h[i >> 1] >> ((i & 1u) * 8u));
}

static uint16_t le16(const uint16_t* h, uint32_t i) {
    return (uint16_t)(b8(h, i) | b8(h, i + 1u) << 8);
}

static uint32_t le32(const uint16_t* h, uint32_t i) {
    return (uint32_t)le16(h, i) | (uint32_t)le16(h, i + 2u) << 16;
}

static bool hp_omega_block(uint32_t lba, uint16_t* buf) {
    return hp_hal_omega_sd_read(lba, 1, buf);
}

static char hp_omega_upper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

static bool hp_omega_same(const char* a, uint32_t alen, const char* b, uint32_t blen) {
    if(alen != blen) return false;
    for(uint32_t i = 0; i < alen; i++) {
        if(hp_omega_upper(a[i]) != hp_omega_upper(b[i])) return false;
    }
    return true;
}

void hp_omega_init(HpOmega* o) {
    o->mounted = false;
    o->fat_cached = UINT32_MAX;
    for(uint32_t k = 0; k < HP_OMEGA_SLOTS; k++) o->files[k].used = false;
}

/* Whether c names a cluster of the volume */
static bool hp_omega_cluster_ok(const HpOmega* o, uint32_t c) {
    return c >= 2u && c - 2u < o->clusters;
}

/* The cluster after c into *next (0 at the chain's end); false if the FAT can't be read, or the
 * entry is free, reserved, bad, or names a cluster outside the volume */
static bool hp_omega_next(HpOmega* o, uint32_t c, uint32_t* next) {
    const uint32_t lba = o->fat + c / 128u;
    if(lba != o->fat_cached) {
        if(!hp_omega_block(lba, o->fat_buf)) {
            o->fat_cached = UINT32_MAX;
            return false;
        }
        o->fat_cached = lba;
    }
    const uint32_t v = le32(o->fat_buf, (c % 128u) * 4u) & 0x0FFFFFFFu;
    if(v >= HP_OMEGA_EOC) {
        *next = 0;
        return true;
    }
    if(v == HP_OMEGA_BAD || !hp_omega_cluster_ok(o, v)) return false;
    *next = v;
    return true;
}

static uint32_t hp_omega_lba(const HpOmega* o, uint32_t cluster) {
    return o->data + (cluster - 2u) * o->spc;
}

static bool hp_omega_is_vbr(const uint16_t* b) {
    static const char fat32[5] = {'F', 'A', 'T', '3', '2'};
    static const char exfat[5] = {'E', 'X', 'F', 'A', 'T'};
    bool is32 = true, isex = true;
    for(uint32_t i = 0; i < 5u; i++) {
        if(b8(b, 82u + i) != (uint8_t)fat32[i]) is32 = false;
        if(b8(b, 3u + i) != (uint8_t)exfat[i]) isex = false;
    }
    return is32 || isex;
}

HpOmegaResult hp_omega_mount(HpOmega* o) {
    hp_omega_init(o);
    uint16_t* b = o->dir_buf;
    if(!hp_omega_block(0, b)) return HpOmegaSdError;
    if(b8(b, 510) != 0x55u || b8(b, 511) != 0xAAu) return HpOmegaNoFs;
    uint32_t part = 0;
    if(!hp_omega_is_vbr(b)) {
        part = le32(b, 0x1C6u);
        if(part == 0 || b8(b, 0x1C2u) == 0) return HpOmegaNoFs;
        if(!hp_omega_block(part, b)) return HpOmegaSdError;
        if(b8(b, 510) != 0x55u || b8(b, 511) != 0xAAu) return HpOmegaNoFs;
    }
    if(b8(b, 3) == 'E' && b8(b, 4) == 'X' && b8(b, 5) == 'F') return HpOmegaNotFat32;
    const uint8_t spc = b8(b, 13);
    const uint16_t reserved = le16(b, 14);
    const uint8_t fats = b8(b, 16);
    const uint32_t fatsz = le32(b, 36);
    const uint32_t root = le32(b, 44);
    const uint32_t total = le32(b, 32) ? le32(b, 32) : le16(b, 19);
    if(le16(b, 11) != HP_OMEGA_BLOCK || spc == 0 || (spc & (spc - 1u)) || reserved == 0 ||
       fats < 1u || fats > 2u || le16(b, 22) != 0 || fatsz == 0 || root < 2u) {
        return HpOmegaNotFat32;
    }
    o->part = part;
    o->spc = spc;
    o->reserved = reserved;
    o->fats = fats;
    o->fatsz = fatsz;
    o->root = root;
    o->fat = part + reserved;
    o->data = o->fat + fats * fatsz;
    if(total <= o->data - part) return HpOmegaNotFat32;
    o->clusters = (total - (o->data - part)) / spc;
    o->mounted = true;
    return HpOmegaOk;
}

typedef struct {
    uint32_t cluster, size;
    uint8_t attr;
    uint16_t date, time;
} HpOmegaEntry;

/* The long name being gathered from an LFN run */
typedef struct {
    bool ok; /* a run started with its last entry, every entry since has its checksum and the
              * sequence number one less than the one before */
    bool too_long; /* a character at or past HP_OMEGA_PATH_MAX - 1 */
    uint8_t sum;
    uint32_t seq; /* the last entry's sequence number: the next is one less, and the run ends at 1 */
    uint32_t len;
    char name[HP_OMEGA_PATH_MAX];
} HpOmegaLfn;

static void hp_omega_lfn_entry(HpOmegaLfn* l, const uint16_t* b, uint32_t e) {
    static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    const uint8_t first = b8(b, e);
    const uint32_t seq = first & 0x1Fu;
    if(first & 0x40u) {
        l->ok = seq >= 1u && seq <= HP_OMEGA_LFN_SEQ_MAX;
        l->too_long = false;
        l->sum = b8(b, e + 13u);
        l->len = seq * 13u;
        l->seq = seq;
    } else if(!l->ok || b8(b, e + 13u) != l->sum || seq < 1u || seq != l->seq - 1u) {
        l->ok = false;
    } else {
        l->seq = seq;
    }
    if(!l->ok) return;
    for(uint32_t j = 0; j < 13u; j++) {
        const uint32_t pos = (seq - 1u) * 13u + j;
        const uint16_t ch = le16(b, e + at[j]);
        if(ch == 0xFFFFu) continue;
        if(ch == 0) {
            if(pos < l->len) l->len = pos;
            continue;
        }
        if(pos >= HP_OMEGA_PATH_MAX - 1u) {
            l->too_long = true;
            continue;
        }
        l->name[pos] = ch < 0x20u || ch > 0x7Eu ? (char)HP_OMEGA_NO_MATCH : (char)ch;
    }
}

static uint8_t hp_omega_sum(const uint16_t* b, uint32_t e) {
    uint8_t s = 0;
    for(uint32_t i = 0; i < 11u; i++) s = (uint8_t)(((s & 1u) << 7) + (s >> 1) + b8(b, e + i));
    return s;
}

/* The 8.3 entry's name, "BASE.EXT" or "BASE", against name */
static bool hp_omega_short_is(const uint16_t* b, uint32_t e, const char* name, uint32_t len) {
    char s[13];
    uint32_t n = 0;
    for(uint32_t i = 0; i < 8u && b8(b, e + i) != ' '; i++) s[n++] = (char)b8(b, e + i);
    if(b8(b, e + 8u) != ' ') {
        s[n++] = '.';
        for(uint32_t i = 8; i < 11u && b8(b, e + i) != ' '; i++) s[n++] = (char)b8(b, e + i);
    }
    return hp_omega_same(s, n, name, len);
}

/* name (len characters) in the directory starting at cluster dir */
static HpOmegaResult hp_omega_find(
    HpOmega* o,
    uint32_t dir,
    const char* name,
    uint32_t len,
    HpOmegaEntry* out) {
    uint16_t* b = o->dir_buf;
    HpOmegaLfn lfn;
    lfn.ok = false;
    uint32_t sectors = 0;
    for(uint32_t c = dir; c;) {
        if(!hp_omega_cluster_ok(o, c)) return HpOmegaSdError;
        for(uint32_t s = 0; s < o->spc; s++) {
            if(++sectors > HP_OMEGA_DIR_SECTORS_MAX) return HpOmegaNotFound;
            if(!hp_omega_block(hp_omega_lba(o, c) + s, b)) return HpOmegaSdError;
            for(uint32_t e = 0; e < HP_OMEGA_BLOCK; e += 32u) {
                const uint8_t first = b8(b, e);
                if(first == 0) return HpOmegaNotFound;
                const uint8_t attr = b8(b, e + 11u);
                if(first == 0xE5u) {
                    lfn.ok = false;
                    continue;
                }
                if(attr == 0x0Fu) {
                    hp_omega_lfn_entry(&lfn, b, e);
                    continue;
                }
                const bool long_ok = lfn.ok && !lfn.too_long && lfn.seq == 1u && hp_omega_sum(b, e) == lfn.sum;
                lfn.ok = false;
                if((attr & 0x08u) || first == '.') continue;
                const bool match = long_ok ? hp_omega_same(lfn.name, lfn.len, name, len) :
                                             hp_omega_short_is(b, e, name, len);
                if(!match) continue;
                out->cluster = (uint32_t)le16(b, e + 20u) << 16 | le16(b, e + 26u);
                out->size = le32(b, e + 28u);
                out->attr = attr;
                out->time = le16(b, e + 22u);
                out->date = le16(b, e + 24u);
                return HpOmegaOk;
            }
        }
        uint32_t next = 0;
        if(!hp_omega_next(o, c, &next)) return HpOmegaSdError;
        c = next;
    }
    return HpOmegaNotFound;
}

HpOmegaResult hp_omega_open(HpOmega* o, uint8_t slot, const char* path) {
    if(!o->mounted || slot >= HP_OMEGA_SLOTS || path[0] != '/') return HpOmegaBadArg;
    uint32_t n = 0;
    while(path[n]) {
        if((uint8_t)path[n] < 0x20u || (uint8_t)path[n] > 0x7Eu) return HpOmegaBadArg;
        if(++n >= HP_OMEGA_PATH_MAX) return HpOmegaBadArg;
    }
    o->files[slot].used = false;
    uint32_t dir = o->root;
    uint32_t at = 1;
    for(;;) {
        uint32_t len = 0;
        while(path[at + len] && path[at + len] != '/') len++;
        if(len == 0) return HpOmegaBadArg; /* "//", or a trailing '/' */
        HpOmegaEntry ent;
        const HpOmegaResult r = hp_omega_find(o, dir, path + at, len, &ent);
        if(r != HpOmegaOk) return r;
        at += len;
        if(path[at] == '/') {
            if(!(ent.attr & 0x10u)) return HpOmegaNotFound;
            dir = ent.cluster ? ent.cluster : o->root;
            at++;
            continue;
        }
        if(ent.attr & 0x10u) return HpOmegaIsDir;
        HpOmegaFile* f = &o->files[slot];
        f->first = ent.cluster;
        f->size = ent.size;
        f->date = ent.date;
        f->time = ent.time;
        f->at_index = 0;
        f->at_cluster = ent.cluster;
        f->used = true;
        return HpOmegaOk;
    }
}

/* f's cluster number index into *cluster, from where the last seek left it when going forward */
static HpOmegaResult hp_omega_seek(HpOmega* o, HpOmegaFile* f, uint32_t index, uint32_t* cluster) {
    if(f->at_cluster < 2u || index < f->at_index) {
        f->at_index = 0;
        f->at_cluster = f->first;
    }
    while(f->at_index < index) {
        uint32_t next = 0;
        if(!hp_omega_next(o, f->at_cluster, &next) || next == 0) return HpOmegaSdError;
        f->at_cluster = next;
        f->at_index++;
    }
    *cluster = f->at_cluster;
    return HpOmegaOk;
}

HpOmegaResult
    hp_omega_read(HpOmega* o, uint8_t slot, uint32_t off, uint8_t blocks, uint16_t* out) {
    if(!o->mounted || slot >= HP_OMEGA_SLOTS || !o->files[slot].used) return HpOmegaBadArg;
    HpOmegaFile* f = &o->files[slot];
    const uint64_t rounded = ((uint64_t)f->size + HP_OMEGA_BLOCK - 1u) & ~(uint64_t)(HP_OMEGA_BLOCK - 1u);
    if(blocks < 1u || blocks > HP_OMEGA_READ_MAX || (off % HP_OMEGA_BLOCK) || f->first < 2u ||
       (uint64_t)off + (uint64_t)blocks * HP_OMEGA_BLOCK > rounded) {
        return HpOmegaBadArg;
    }
    if(!hp_omega_cluster_ok(o, f->first)) return HpOmegaSdError;
    for(uint32_t b = 0; b < blocks;) {
        const uint32_t idx = off / HP_OMEGA_BLOCK + b;
        const uint32_t within = idx % o->spc;
        uint32_t cluster = 0;
        const HpOmegaResult r = hp_omega_seek(o, f, idx / o->spc, &cluster);
        if(r != HpOmegaOk) return r;
        uint32_t run = o->spc - within;
        if(run > blocks - b) run = blocks - b;
        if(run > HP_OMEGA_RUN) run = HP_OMEGA_RUN;
        if(!hp_hal_omega_sd_read(hp_omega_lba(o, cluster) + within, (uint8_t)run, out + b * 256u)) {
            return HpOmegaSdError;
        }
        b += run;
    }
    return HpOmegaOk;
}
