#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Test-only microSD card with a FAT32 file system, as an EZ-Flash Omega's card is laid out: an MBR
 * with one partition (type 0x0C), or a boot sector at block 0 with no MBR ("superfloppy"); 512-byte
 * sectors, 32 reserved sectors, 2 FATs, the root directory at cluster 2. Directory entries get a
 * long-name (LFN) run, with its checksum, and an 8.3 name the test never relies on. It shares no
 * code with gba/helper/src/hp_omega.c.
 *
 * Sparse: a block is allocated on its first write, and unwritten ones read as zero. Never a stack
 * local in a test: static. Each fake_fat_init* allocates; fake_fat_free releases. */

typedef struct FakeFat FakeFat;

struct FakeFat {
    uint32_t blocks; /* the card's size */
    uint8_t** blk; /* per block: NULL (zero) or 512 bytes */
    uint32_t part, fat, data, fatsz;
    uint8_t spc;
    uint32_t next_free; /* the next cluster to allocate */
    uint32_t clusters; /* clusters 2 .. clusters + 1 exist */
    uint16_t stamp; /* the next file's write time */
};

/* A formatted card of `blocks` blocks: an MBR and a partition at part_lba, or a boot sector at
 * block 0 if part_lba is 0; spc sectors per cluster; an empty root directory. */
void fake_fat_init(FakeFat* f, uint32_t part_lba, uint8_t spc, uint32_t blocks);
/* A card of `blocks` zero blocks: no MBR, no file system */
void fake_fat_init_blank(FakeFat* f, uint32_t blocks);
/* A directory long_name in the directory at parent_cluster; returns its cluster */
uint32_t fake_fat_mkdir(FakeFat* f, uint32_t parent_cluster, const char* long_name);
/* A file long_name in the directory at dir_cluster holding size bytes of data, in clusters;
 * gap_every > 0 leaves one free cluster after every gap_every of them (fragmented). Returns its
 * first cluster (0 for an empty file). */
uint32_t fake_fat_add_file(
    FakeFat* f,
    uint32_t dir_cluster,
    const char* long_name,
    const uint8_t* data,
    uint32_t size,
    uint32_t gap_every);
/* A deleted entry for long_name: its LFN run and 8.3 entry, each first byte 0xE5 */
void fake_fat_add_deleted(FakeFat* f, uint32_t dir_cluster, const char* long_name);
/* Writes cluster's FAT entry (both FATs) */
void fake_fat_set_next(FakeFat* f, uint32_t cluster, uint32_t next);
/* An empty file "LFNTEST.DAT" in the directory at dir, with an LFN run for long_name (its checksum
 * right) from which the entry with sequence drop_seq is left out; 0 leaves none out. */
void fake_fat_lfn_entry(FakeFat* f, uint32_t dir, const char* long_name, uint8_t drop_seq);
uint32_t fake_fat_root(const FakeFat* f);
/* Overwrites n bytes of block lba at off */
void fake_fat_poke(FakeFat* f, uint32_t lba, uint32_t off, const void* p, uint32_t n);
/* Block lba into out; false past the card's end */
bool fake_fat_block(const FakeFat* f, uint32_t lba, uint8_t out[512]);
/* The LBA of byte off of the file starting at first_cluster */
uint32_t fake_fat_file_lba(FakeFat* f, uint32_t first_cluster, uint32_t off);
void fake_fat_free(FakeFat* f);
