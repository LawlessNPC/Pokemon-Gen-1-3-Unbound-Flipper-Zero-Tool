#pragma once

#include <stdbool.h>
#include <stdint.h>

/* FAT32 over an EZ-Flash Omega's microSD, through hp_hal_omega_sd_read: mount the card, open files
 * by path, and read them by offset. The one write is hp_omega_write_sector, which puts 4 KB into the
 * open save file's own blocks and nothing else. Portable C, no heap.
 *
 * The card: an MBR whose first partition holds the file system, or a boot sector at block 0; 512
 * bytes a sector; FAT32 only. Paths are absolute ("/SAVER/Pokemon - Unbound.sav"), ASCII, matched
 * against each entry's long name if it has a sound one (its LFN checksum matches its 8.3 entry),
 * else its 8.3 name, case-insensitively. A long name with a character past 0x7E never matches. */

#define HP_OMEGA_SLOTS 3u /* 0 the save, 1 Recently play.txt, 2 the ROM */
#define HP_OMEGA_PATH_MAX 96u /* bytes, with the terminator */
#define HP_OMEGA_READ_MAX 8u /* blocks a hp_omega_read takes */

/* The values are the fail codes the Omega commands send (hp_core.h) */
typedef enum {
    HpOmegaOk,
    HpOmegaSdError, /* the card didn't answer, or a cluster chain broke */
    HpOmegaNoFs, /* no MBR or boot sector signature, or no partition */
    HpOmegaNotFat32, /* exFAT, FAT12/16, or sectors other than 512 bytes */
    HpOmegaNotFound,
    HpOmegaIsDir, /* the path names a directory */
    HpOmegaBadArg, /* a slot, path, offset or count outside the rules, or not mounted or open */
} HpOmegaResult;

typedef struct {
    bool used;
    uint32_t first; /* first cluster, 0 for an empty file */
    uint32_t size;
    uint16_t date, time; /* the directory entry's write date and time */
    uint32_t at_index, at_cluster; /* the last cluster reached: its index in the chain, and it */
} HpOmegaFile;

typedef struct {
    bool mounted;
    uint32_t part, fat, data, root, fatsz; /* LBAs, the root's cluster, FAT size in sectors */
    uint16_t reserved;
    uint8_t spc, fats;
    uint32_t clusters; /* the volume's cluster count: clusters 2 .. clusters + 1 exist */
    uint32_t fat_cached; /* the LBA in fat_buf, UINT32_MAX none */
    uint16_t fat_buf[256];
    uint16_t dir_buf[256];
    HpOmegaFile files[HP_OMEGA_SLOTS];
} HpOmega;

void hp_omega_init(HpOmega* o); /* not mounted, no file open */
/* Reads the MBR and the boot sector. Ok: mounted, the files closed. */
HpOmegaResult hp_omega_mount(HpOmega* o);
/* Opens path into slot, closing what was there. BadArg if not mounted, the slot is out of range, or
 * path isn't "/..." of 1..95 printable ASCII characters. */
HpOmegaResult hp_omega_open(HpOmega* o, uint8_t slot, const char* path);
/* blocks (1..HP_OMEGA_READ_MAX) blocks of the file in slot from off into out (blocks * 256
 * halfwords, little-endian): off % 512 == 0 and off + 512 * blocks <= its size rounded up to 512.
 * The part of the last block past the size is whatever the card holds. */
HpOmegaResult
    hp_omega_read(HpOmega* o, uint8_t slot, uint32_t off, uint8_t blocks, uint16_t* out);
/* Writes the 4 KB image data (2048 halfwords, little-endian) over sector (0 .. size / 4096 - 1) of
 * the file in slot 0, in place, and reads it back. First every block address of the sector is
 * resolved through the file's chain, and a cluster outside the volume or a chain that ends early is
 * SdError with nothing written. Then the 8 blocks go out in runs of up to 4 (never across a
 * cluster), and *crc is the CRC-32 (as OMEGA_CRC32's) of the 4096 bytes read back, which the caller
 * compares with the image's. BadArg: not mounted, slot 0 not open, or a sector out of range. Only
 * slot 0 is ever written; the file's chain is never changed. */
HpOmegaResult hp_omega_write_sector(HpOmega* o, uint32_t sector, const uint16_t* data, uint32_t* crc);
