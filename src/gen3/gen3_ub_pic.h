#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Pictures for Pokemon Unbound's newer species, read from the user's own ROM at runtime.
 *
 * gen3_ub_lz77 reads the GBA BIOS LZ77 format (type 0x10, as the BIOS's LZ77UnCompWram does).
 * gen3_ub_pic_convert turns a decoded 64x64 4bpp front picture and its palette into the record
 * of files/all_sprites.fxbm; it is a port of convert and fxbm_record in tools/gen3_sprites.py,
 * which stays the reference (tests/ub_pic_vectors.h holds records that tool made). */
#define GEN3_UB_PIC_RAW    0x800u /* 64x64, 4bpp, GBA 8x8 tiles */
#define GEN3_UB_PIC_RECORD 404u /* as all_sprites.fxbm */
/* A 2-bit level grid of 64x64 (1024 bytes), then the row and column maps (64 + 64 bytes) */
#define GEN3_UB_PIC_WORK (1024u + 128u)

/* Reads the next up to n bytes of a compressed stream into buf; returns the count, 0 at a failure */
typedef uint32_t (*Gen3UbPicSource)(void* ctx, uint8_t* buf, uint32_t n);

/* GBA LZ77 (type 0x10) into out, exactly want bytes; false on any malformed stream. It never
 * writes past out[want - 1], and never asks the source for more than a stream of want bytes can
 * hold. */
bool gen3_ub_lz77(Gen3UbPicSource src, void* ctx, uint8_t* out, uint32_t want);

/* raw: 2048 bytes of 4bpp tiles; pal: 16 BGR555 colours; work: GEN3_UB_PIC_WORK bytes;
 * out: a 404-byte fxbm record. false if the picture has no pixels. */
bool gen3_ub_pic_convert(
    const uint8_t* raw,
    const uint16_t pal[16],
    uint8_t* work,
    uint8_t out[GEN3_UB_PIC_RECORD]);
