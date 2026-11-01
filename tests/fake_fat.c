#include "fake_fat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 512u
#define RESERVED 32u
#define FATS 2u
#define EOC 0x0FFFFFFFu
#define ROOT 2u

static uint8_t* blk_w(FakeFat* f, uint32_t lba) {
    if(lba >= f->blocks) {
        fprintf(stderr, "fake_fat: block %u past the card\n", (unsigned)lba);
        abort();
    }
    if(!f->blk[lba]) f->blk[lba] = calloc(1, BLOCK);
    return f->blk[lba];
}

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void fat_set(FakeFat* f, uint32_t n, uint32_t v) {
    for(uint32_t k = 0; k < FATS; k++) {
        put32(blk_w(f, f->fat + k * f->fatsz + n / 128u) + (n % 128u) * 4u, v);
    }
}

static uint32_t fat_get(FakeFat* f, uint32_t n) {
    return get32(blk_w(f, f->fat + n / 128u) + (n % 128u) * 4u) & 0x0FFFFFFFu;
}

static uint32_t cluster_lba(const FakeFat* f, uint32_t c) {
    return f->data + (c - 2u) * f->spc;
}

static uint32_t alloc_cluster(FakeFat* f) {
    if(f->next_free >= f->clusters + 2u) {
        fprintf(stderr, "fake_fat: card full\n");
        abort();
    }
    const uint32_t c = f->next_free++;
    fat_set(f, c, EOC);
    for(uint32_t s = 0; s < f->spc; s++) memset(blk_w(f, cluster_lba(f, c) + s), 0, BLOCK);
    return c;
}

static void blank(FakeFat* f, uint32_t blocks) {
    memset(f, 0, sizeof(*f));
    f->blocks = blocks;
    f->blk = calloc(blocks ? blocks : 1u, sizeof(uint8_t*));
}

void fake_fat_init_blank(FakeFat* f, uint32_t blocks) {
    blank(f, blocks);
}

void fake_fat_init(FakeFat* f, uint32_t part_lba, uint8_t spc, uint32_t blocks) {
    blank(f, blocks);
    f->part = part_lba;
    f->spc = spc;
    const uint32_t avail = blocks - part_lba - RESERVED;
    /* clusters * spc + FATS * fatsz <= avail, fatsz = ceil((clusters + 2) * 4 / 512) */
    uint32_t clusters = avail / spc;
    for(;;) {
        const uint32_t fatsz = ((clusters + 2u) * 4u + BLOCK - 1u) / BLOCK;
        if(clusters * spc + FATS * fatsz <= avail) {
            f->fatsz = fatsz;
            break;
        }
        clusters--;
    }
    f->clusters = clusters;
    f->fat = part_lba + RESERVED;
    f->data = f->fat + FATS * f->fatsz;
    f->stamp = 0x6000u;

    if(part_lba) {
        uint8_t* mbr = blk_w(f, 0);
        uint8_t* pe = mbr + 0x1BEu;
        pe[4] = 0x0C;
        put32(pe + 8, part_lba);
        put32(pe + 12, blocks - part_lba);
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
    }
    uint8_t* v = blk_w(f, part_lba);
    v[0] = 0xEB;
    v[1] = 0x58;
    v[2] = 0x90;
    memcpy(v + 3, "MSWIN4.1", 8);
    put16(v + 11, BLOCK);
    v[13] = spc;
    put16(v + 14, RESERVED);
    v[16] = FATS;
    v[21] = 0xF8;
    put32(v + 32, blocks - part_lba);
    put32(v + 36, f->fatsz);
    put32(v + 44, ROOT);
    memcpy(v + 82, "FAT32   ", 8);
    v[510] = 0x55;
    v[511] = 0xAA;

    fat_set(f, 0, 0x0FFFFFF8u);
    fat_set(f, 1, EOC);
    f->next_free = ROOT;
    alloc_cluster(f); /* the root */
}

uint32_t fake_fat_root(const FakeFat* f) {
    (void)f;
    return ROOT;
}

/* Entry k of the directory at dir: its block and offset, the chain grown as needed */
static uint8_t* dir_entry(FakeFat* f, uint32_t dir, uint32_t k) {
    const uint32_t per_cluster = f->spc * (BLOCK / 32u);
    uint32_t c = dir;
    for(uint32_t i = 0; i < k / per_cluster; i++) {
        uint32_t next = fat_get(f, c);
        if(next >= 0x0FFFFFF8u) {
            next = alloc_cluster(f);
            fat_set(f, c, next);
        }
        c = next;
    }
    const uint32_t in = k % per_cluster;
    return blk_w(f, cluster_lba(f, c) + in / (BLOCK / 32u)) + (in % (BLOCK / 32u)) * 32u;
}

static uint32_t dir_end(FakeFat* f, uint32_t dir) {
    uint32_t k = 0;
    while(dir_entry(f, dir, k)[0] != 0) k++;
    return k;
}

static uint8_t lfn_sum(const uint8_t short_name[11]) {
    uint8_t s = 0;
    for(uint32_t i = 0; i < 11u; i++) s = (uint8_t)(((s & 1u) << 7) + (s >> 1) + short_name[i]);
    return s;
}

static void short_name(FakeFat* f, uint8_t out[11]) {
    char tmp[12];
    snprintf(tmp, sizeof(tmp), "F%07uDAT", (unsigned)(f->next_free * 131u + f->stamp));
    memcpy(out, tmp, 11);
}

/* The LFN run then the 8.3 entry for name at the directory's end; first byte 0xE5 if deleted */
static uint8_t* add_entry(FakeFat* f, uint32_t dir, const char* name, bool deleted) {
    static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    uint8_t sn[11];
    short_name(f, sn);
    const uint8_t sum = lfn_sum(sn);
    const uint32_t len = (uint32_t)strlen(name);
    const uint32_t n = (len + 12u) / 13u;
    uint32_t k = dir_end(f, dir);
    for(uint32_t i = n; i >= 1u; i--) {
        uint8_t* e = dir_entry(f, dir, k++);
        memset(e, 0, 32);
        e[0] = (uint8_t)(i | (i == n ? 0x40u : 0u));
        e[11] = 0x0F;
        e[13] = sum;
        for(uint32_t j = 0; j < 13u; j++) {
            const uint32_t pos = (i - 1u) * 13u + j;
            const uint16_t ch = pos < len ? (uint16_t)(uint8_t)name[pos] : pos == len ? 0u : 0xFFFFu;
            put16(e + at[j], ch);
        }
        if(deleted) e[0] = 0xE5;
    }
    uint8_t* e = dir_entry(f, dir, k);
    memset(e, 0, 32);
    memcpy(e, sn, 11);
    put16(e + 22, f->stamp++);
    put16(e + 24, 0x5B21u);
    if(deleted) e[0] = 0xE5;
    return e;
}

uint32_t fake_fat_mkdir(FakeFat* f, uint32_t parent_cluster, const char* long_name) {
    const uint32_t c = alloc_cluster(f);
    uint8_t* e = add_entry(f, parent_cluster, long_name, false);
    e[11] = 0x10;
    put16(e + 20, (uint16_t)(c >> 16));
    put16(e + 26, (uint16_t)c);
    uint8_t* dot = dir_entry(f, c, 0);
    memcpy(dot, ".          ", 11);
    dot[11] = 0x10;
    put16(dot + 20, (uint16_t)(c >> 16));
    put16(dot + 26, (uint16_t)c);
    uint8_t* dotdot = dir_entry(f, c, 1);
    memcpy(dotdot, "..         ", 11);
    dotdot[11] = 0x10;
    const uint32_t up = parent_cluster == ROOT ? 0u : parent_cluster;
    put16(dotdot + 20, (uint16_t)(up >> 16));
    put16(dotdot + 26, (uint16_t)up);
    return c;
}

uint32_t fake_fat_add_file(
    FakeFat* f,
    uint32_t dir_cluster,
    const char* long_name,
    const uint8_t* data,
    uint32_t size,
    uint32_t gap_every) {
    const uint32_t csize = f->spc * BLOCK;
    const uint32_t n = (size + csize - 1u) / csize;
    uint32_t first = 0, prev = 0;
    for(uint32_t i = 0; i < n; i++) {
        if(gap_every && i && i % gap_every == 0) alloc_cluster(f); /* left free below */
        const uint32_t c = alloc_cluster(f);
        if(gap_every && i && i % gap_every == 0) fat_set(f, c - 1u, 0);
        if(prev) {
            fat_set(f, prev, c);
        } else {
            first = c;
        }
        prev = c;
        for(uint32_t s = 0; s < f->spc; s++) {
            const uint32_t off = i * csize + s * BLOCK;
            if(off >= size) break;
            const uint32_t len = size - off < BLOCK ? size - off : BLOCK;
            memcpy(blk_w(f, cluster_lba(f, c) + s), data + off, len);
        }
    }
    uint8_t* e = add_entry(f, dir_cluster, long_name, false);
    e[11] = 0x20;
    put16(e + 20, (uint16_t)(first >> 16));
    put16(e + 26, (uint16_t)first);
    put32(e + 28, size);
    return first;
}

void fake_fat_add_deleted(FakeFat* f, uint32_t dir_cluster, const char* long_name) {
    uint8_t* e = add_entry(f, dir_cluster, long_name, true);
    e[11] = 0x20;
    put32(e + 28, 1234u);
}

void fake_fat_poke(FakeFat* f, uint32_t lba, uint32_t off, const void* p, uint32_t n) {
    memcpy(blk_w(f, lba) + off, p, n);
}

bool fake_fat_block(const FakeFat* f, uint32_t lba, uint8_t out[512]) {
    if(lba >= f->blocks) return false;
    if(f->blk[lba]) {
        memcpy(out, f->blk[lba], BLOCK);
    } else {
        memset(out, 0, BLOCK);
    }
    return true;
}

uint32_t fake_fat_file_lba(FakeFat* f, uint32_t first_cluster, uint32_t off) {
    const uint32_t csize = f->spc * BLOCK;
    uint32_t c = first_cluster;
    for(uint32_t i = 0; i < off / csize; i++) c = fat_get(f, c);
    return cluster_lba(f, c) + (off % csize) / BLOCK;
}

void fake_fat_free(FakeFat* f) {
    if(f->blk) {
        for(uint32_t i = 0; i < f->blocks; i++) free(f->blk[i]);
        free(f->blk);
    }
    memset(f, 0, sizeof(*f));
}
