/* Pictures for Pokemon Unbound's newer species. See gen3_ub_pic.h.
 *
 * The LZ77 reader follows the GBA BIOS format (GBATEK, "BIOS Decompression Functions", type
 * 0x10). The picture conversion is a port of convert and fxbm_record in tools/gen3_sprites.py:
 * the same steps in the same order, in integers. */
#include "gen3_ub_pic.h"

#include <string.h>

/* --- LZ77 --- */

/* The compressed stream is read through a small buffer. A stream that makes want bytes is at
 * most 4 + want + ceil(want / 8) bytes long (a header, and a flag byte and a literal for every
 * output byte), so the source is never asked for more than that. */
#define LZ_BUF 64u

typedef struct {
    Gen3UbPicSource src;
    void* ctx;
    uint32_t pos;
    uint32_t len;
    uint32_t left; /* how many more bytes the source may be asked for */
    uint8_t buf[LZ_BUF];
} LzReader;

static bool lz_byte(LzReader* r, uint8_t* b) {
    if(r->pos == r->len) {
        uint32_t n = r->left < LZ_BUF ? r->left : LZ_BUF;
        if(n == 0) return false;
        uint32_t got = r->src(r->ctx, r->buf, n);
        if(got == 0 || got > n) return false;
        r->pos = 0;
        r->len = got;
        r->left -= got;
    }
    *b = r->buf[r->pos++];
    return true;
}

bool gen3_ub_lz77(Gen3UbPicSource src, void* ctx, uint8_t* out, uint32_t want) {
    if(want > 0xFFFFFFu) return false;
    LzReader r = {src, ctx, 0, 0, 4u + want + (want + 7u) / 8u, {0}};
    uint8_t head[4];
    for(uint32_t i = 0; i < 4; i++) {
        if(!lz_byte(&r, &head[i])) return false;
    }
    uint32_t size = (uint32_t)head[1] | (uint32_t)head[2] << 8 | (uint32_t)head[3] << 16;
    if(head[0] != 0x10 || size != want) return false;

    uint32_t made = 0;
    while(made < want) {
        uint8_t flags;
        if(!lz_byte(&r, &flags)) return false;
        for(uint32_t bit = 0; bit < 8 && made < want; bit++) {
            if(flags & (0x80u >> bit)) {
                uint8_t b0, b1;
                if(!lz_byte(&r, &b0) || !lz_byte(&r, &b1)) return false;
                uint32_t length = (uint32_t)(b0 >> 4) + 3u;
                uint32_t disp = ((uint32_t)(b0 & 0x0Fu) << 8 | b1) + 1u;
                if(disp > made || length > want - made) return false;
                /* Byte by byte: a copy may overlap what it writes */
                for(uint32_t i = 0; i < length; i++, made++) out[made] = out[made - disp];
            } else {
                uint8_t b;
                if(!lz_byte(&r, &b)) return false;
                out[made++] = b;
            }
        }
    }
    return true;
}

/* --- The picture --- */

#define SRC_SIZE 64u
#define FRAME    56u
#define ROW_B    7u /* bytes a row of the record */

#define LUM_BLACK_K   (60 * 1000)
#define LUM_WHITE_K   (165 * 1000)
#define LINE_MARGIN_K (35 * 1000)
#define TRANSP_SHADE  (255 * 1000)

enum { LV_TRANSPARENT = 0, LV_WHITE = 1, LV_GREY = 2, LV_BLACK = 3 };

/* work: 64 x 64 levels of 2 bits (4 to a byte), then the row map and the column map: for the
 * picture's row y and column x, the row and column of the grid they stand for */
#define MAP_ROWS 1024u
#define MAP_COLS (MAP_ROWS + SRC_SIZE)

static unsigned grid_get(const uint8_t* grid, unsigned x, unsigned y) {
    unsigned i = y * SRC_SIZE + x;
    return (grid[i >> 2] >> ((i & 3u) * 2u)) & 3u;
}

static void grid_set(uint8_t* grid, unsigned x, unsigned y, unsigned v) {
    unsigned i = y * SRC_SIZE + x;
    unsigned shift = (i & 3u) * 2u;
    grid[i >> 2] = (uint8_t)((grid[i >> 2] & ~(3u << shift)) | v << shift);
}

/* The palette index of the pixel at (x, y) of 8x8 tiles in 4bpp, the low nibble on the left */
static unsigned raw_pixel(const uint8_t* raw, unsigned x, unsigned y) {
    unsigned tile = (y >> 3) * 8u + (x >> 3);
    uint8_t b = raw[tile * 32u + (y & 7u) * 4u + ((x & 7u) >> 1)];
    return (x & 1u) ? (unsigned)(b >> 4) : (unsigned)(b & 0x0Fu);
}

/* Every pixel next to a transparent one, or to the edge, becomes black. Rewriting in place is
 * the same as Python's copy: only a transparent pixel stops a pixel being black, and a pixel
 * that turns black was not transparent. */
static void outline(uint8_t* work, unsigned w, unsigned h) {
    const uint8_t* rows = work + MAP_ROWS;
    const uint8_t* cols = work + MAP_COLS;
    for(unsigned y = 0; y < h; y++) {
        for(unsigned x = 0; x < w; x++) {
            if(grid_get(work, cols[x], rows[y]) == LV_TRANSPARENT) continue;
            bool edge = x == 0 || y == 0 || x + 1 == w || y + 1 == h ||
                        grid_get(work, cols[x - 1], rows[y]) == LV_TRANSPARENT ||
                        grid_get(work, cols[x + 1], rows[y]) == LV_TRANSPARENT ||
                        grid_get(work, cols[x], rows[y - 1]) == LV_TRANSPARENT ||
                        grid_get(work, cols[x], rows[y + 1]) == LV_TRANSPARENT;
            if(edge) grid_set(work, cols[x], rows[y], LV_BLACK);
        }
    }
}

/* How many of the first n entries along the other axis differ between the lines a and b, where
 * the map is the one being dropped from. */
static unsigned line_diff(
    const uint8_t* work,
    bool by_column,
    unsigned a,
    unsigned b,
    unsigned n) {
    const uint8_t* rows = work + MAP_ROWS;
    const uint8_t* cols = work + MAP_COLS;
    unsigned diff = 0;
    for(unsigned k = 0; k < n; k++) {
        if(by_column) {
            diff += grid_get(work, cols[a], rows[k]) != grid_get(work, cols[b], rows[k]);
        } else {
            diff += grid_get(work, cols[k], rows[a]) != grid_get(work, cols[k], rows[b]);
        }
    }
    return diff;
}

/* Removes columns (or rows), one at a time, the one that differs least from the one before it;
 * the first of equals. count is the size along the dropped axis, other the size along the
 * other. */
static unsigned drop_lines(uint8_t* work, bool by_column, unsigned count, unsigned other) {
    uint8_t* map = work + (by_column ? MAP_COLS : MAP_ROWS);
    while(count > FRAME) {
        unsigned best = 1;
        unsigned best_diff = line_diff(work, by_column, 1, 0, other);
        for(unsigned i = 2; i < count; i++) {
            unsigned d = line_diff(work, by_column, i, i - 1, other);
            if(d < best_diff) {
                best_diff = d;
                best = i;
            }
        }
        memmove(map + best, map + best + 1, count - best - 1);
        count--;
    }
    return count;
}

bool gen3_ub_pic_convert(
    const uint8_t* raw,
    const uint16_t pal[16],
    uint8_t* work,
    uint8_t out[GEN3_UB_PIC_RECORD]) {
    /* Each colour's luminance times 1000, and its level; colour 0 is transparent */
    int32_t lum[16];
    uint8_t level[16];
    lum[0] = TRANSP_SHADE;
    level[0] = LV_TRANSPARENT;
    for(unsigned i = 1; i < 16; i++) {
        int32_t r = (pal[i] & 31) * 8;
        int32_t g = ((pal[i] >> 5) & 31) * 8;
        int32_t b = ((pal[i] >> 10) & 31) * 8;
        lum[i] = 299 * r + 587 * g + 114 * b;
        level[i] = lum[i] < LUM_BLACK_K ? LV_BLACK : lum[i] < LUM_WHITE_K ? LV_GREY : LV_WHITE;
    }

    memset(work, 0, MAP_ROWS + 2 * SRC_SIZE);
    for(unsigned y = 0; y < SRC_SIZE; y++) {
        for(unsigned x = 0; x < SRC_SIZE; x++) {
            grid_set(work, x, y, level[raw_pixel(raw, x, y)]);
        }
    }

    /* Lines: a pixel darker by more than the margin than both neighbours along a row, a column
     * or a diagonal. The shades come from the picture, so the pixels turned black here don't
     * change what the others see. */
    static const int8_t pairs[4][4] = {
        {-1, 0, 1, 0},
        {0, -1, 0, 1},
        {-1, -1, 1, 1},
        {-1, 1, 1, -1},
    };
    for(unsigned y = 1; y < SRC_SIZE - 1; y++) {
        for(unsigned x = 1; x < SRC_SIZE - 1; x++) {
            unsigned lv = grid_get(work, x, y);
            if(lv == LV_TRANSPARENT || lv == LV_BLACK) continue;
            int32_t v = lum[raw_pixel(raw, x, y)] + LINE_MARGIN_K;
            for(unsigned p = 0; p < 4; p++) {
                int32_t a = lum[raw_pixel(raw, x + pairs[p][0], y + pairs[p][1])];
                int32_t b = lum[raw_pixel(raw, x + pairs[p][2], y + pairs[p][3])];
                if(a > v && b > v) {
                    grid_set(work, x, y, LV_BLACK);
                    break;
                }
            }
        }
    }

    uint8_t* rows = work + MAP_ROWS;
    uint8_t* cols = work + MAP_COLS;
    for(unsigned i = 0; i < SRC_SIZE; i++) {
        rows[i] = (uint8_t)i;
        cols[i] = (uint8_t)i;
    }
    outline(work, SRC_SIZE, SRC_SIZE);

    /* Crop to the pixels */
    unsigned x0 = SRC_SIZE, x1 = 0, y0 = SRC_SIZE, y1 = 0;
    for(unsigned y = 0; y < SRC_SIZE; y++) {
        for(unsigned x = 0; x < SRC_SIZE; x++) {
            if(grid_get(work, x, y) == LV_TRANSPARENT) continue;
            if(x < x0) x0 = x;
            if(x > x1) x1 = x;
            if(y < y0) y0 = y;
            if(y > y1) y1 = y;
        }
    }
    if(x0 > x1) return false;
    unsigned w = x1 - x0 + 1;
    unsigned h = y1 - y0 + 1;
    for(unsigned i = 0; i < w; i++) cols[i] = (uint8_t)(x0 + i);
    for(unsigned i = 0; i < h; i++) rows[i] = (uint8_t)(y0 + i);

    /* Too wide: drop columns, then too tall: drop rows */
    if(w > FRAME) w = drop_lines(work, true, w, h);
    if(h > FRAME) h = drop_lines(work, false, h, w);
    outline(work, w, h);

    /* The record: bottom centre of the frame; grey is a checkerboard in frame coordinates */
    memset(out, 0, GEN3_UB_PIC_RECORD);
    out[0] = 400u & 0xFFu;
    out[1] = 400u >> 8;
    out[4] = FRAME;
    out[8] = FRAME;
    unsigned left = (FRAME - w) / 2u;
    unsigned top = FRAME - h;
    for(unsigned y = 0; y < h; y++) {
        for(unsigned x = 0; x < w; x++) {
            unsigned v = grid_get(work, cols[x], rows[y]);
            unsigned fx = left + x;
            unsigned fy = top + y;
            if(v == LV_BLACK || (v == LV_GREY && (fx + fy) % 2u == 0)) {
                out[12u + fy * ROW_B + fx / 8u] |= (uint8_t)(1u << (fx % 8u));
            }
        }
    }
    return true;
}
