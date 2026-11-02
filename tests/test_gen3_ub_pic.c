#include "test.h"
#include <src/gen3/gen3_ub_pic.h>
#include "ub_pic_vectors.h"

/* A source that hands out at most chunk bytes a call, fails (returns 0) once fail_at bytes have
 * been given, and counts what it was asked for. */
typedef struct {
    const uint8_t* data;
    uint32_t size;
    uint32_t pos;
    uint32_t chunk;
    uint32_t fail_at;
} Feed;

static uint32_t feed_read(void* ctx, uint8_t* buf, uint32_t n) {
    Feed* f = ctx;
    uint32_t end = f->size < f->fail_at ? f->size : f->fail_at;
    if(f->pos >= end) return 0;
    if(n > f->chunk) n = f->chunk;
    if(n > end - f->pos) n = end - f->pos;
    memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    return n;
}

#define GUARD 16

TEST(test_ub_pic_convert_vectors) {
    for(size_t i = 0; i < UB_PIC_VECTOR_COUNT; i++) {
        const UbPicVector* v = &ub_pic_vectors[i];
        uint8_t work[GEN3_UB_PIC_WORK];
        uint8_t out[GEN3_UB_PIC_RECORD];
        memset(out, 0xA5, sizeof(out));
        memset(work, 0x5A, sizeof(work));
        ASSERT_TRUE(gen3_ub_pic_convert(v->tiles, v->palette, work, out));
        if(memcmp(out, v->record, sizeof(out)) != 0) printf("  vector %zu differs\n", i);
        ASSERT_MEM_EQ(out, v->record, GEN3_UB_PIC_RECORD);
    }
}

TEST(test_ub_lz77_vectors) {
    static const uint32_t chunks[] = {1, 7, 512};
    for(size_t i = 0; i < UB_PIC_VECTOR_COUNT; i++) {
        const UbPicVector* v = &ub_pic_vectors[i];
        for(size_t c = 0; c < 3; c++) {
            Feed f = {v->tiles_lz, v->tiles_lz_size, 0, chunks[c], 0xFFFFFFFFu};
            uint8_t out[GEN3_UB_PIC_RAW + GUARD];
            memset(out, 0xCC, sizeof(out));
            ASSERT_TRUE(gen3_ub_lz77(feed_read, &f, out, GEN3_UB_PIC_RAW));
            ASSERT_MEM_EQ(out, v->tiles, GEN3_UB_PIC_RAW);
            for(size_t g = 0; g < GUARD; g++) ASSERT_EQ(out[GEN3_UB_PIC_RAW + g], 0xCC);
        }
    }
    /* The palette is a stream of 32 bytes */
    for(size_t c = 0; c < 3; c++) {
        Feed f = {ub_pic_palette_lz, sizeof(ub_pic_palette_lz), 0, chunks[c], 0xFFFFFFFFu};
        uint8_t out[32];
        ASSERT_TRUE(gen3_ub_lz77(feed_read, &f, out, 32));
        ASSERT_MEM_EQ(out, ub_pic_palette_raw, 32);
    }
}

/* The vectors must exercise literals, back-references and overlapping copies */
TEST(test_ub_lz77_vectors_are_compressed) {
    for(size_t i = 0; i < UB_PIC_VECTOR_COUNT; i++) {
        ASSERT_TRUE(ub_pic_vectors[i].tiles_lz_size < GEN3_UB_PIC_RAW / 2);
    }
}

static bool run_bad(const uint8_t* stream, uint32_t size, uint32_t want, uint32_t fail_at) {
    Feed f = {stream, size, 0, 5, fail_at};
    uint8_t out[GEN3_UB_PIC_RAW + GUARD];
    memset(out, 0xCC, sizeof(out));
    bool ok = gen3_ub_lz77(feed_read, &f, out, want);
    for(size_t g = want; g < sizeof(out); g++) {
        if(out[g] != 0xCC) {
            printf("  guard byte %zu written\n", g);
            test_failures++;
            return false;
        }
    }
    return ok;
}

TEST(test_ub_lz77_malformed) {
    /* Type 0x11 */
    static const uint8_t type11[] = {0x11, 0x00, 0x08, 0x00, 0x00, 1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(!run_bad(type11, sizeof(type11), 0x800, 0xFFFFFFFFu));
    /* Size 0x801 asked as 0x800, and 0x7FF asked as 0x800 */
    static const uint8_t big[] = {0x10, 0x01, 0x08, 0x00, 0x00, 1};
    ASSERT_TRUE(!run_bad(big, sizeof(big), 0x800, 0xFFFFFFFFu));
    static const uint8_t small[] = {0x10, 0xFF, 0x07, 0x00, 0x00, 1};
    ASSERT_TRUE(!run_bad(small, sizeof(small), 0x800, 0xFFFFFFFFu));
    /* A back-reference before the start: one literal, then displacement 3 (b0 0x00, b1 0x02) */
    static const uint8_t before[] = {0x10, 0x08, 0x00, 0x00, 0x40, 'a', 0x00, 0x02};
    ASSERT_TRUE(!run_bad(before, sizeof(before), 8, 0xFFFFFFFFu));
    /* A back-reference with nothing written yet */
    static const uint8_t first[] = {0x10, 0x08, 0x00, 0x00, 0x80, 0x00, 0x00};
    ASSERT_TRUE(!run_bad(first, sizeof(first), 8, 0xFFFFFFFFu));
    /* A copy that would run past want: 1 literal, then length 18 (0xF) from distance 1, but only
     * 8 bytes are wanted */
    static const uint8_t pass[] = {0x10, 0x08, 0x00, 0x00, 0x40, 'a', 0xF0, 0x00};
    ASSERT_TRUE(!run_bad(pass, sizeof(pass), 8, 0xFFFFFFFFu));
    /* The same stream at the exact length is good: 1 + 7 */
    static const uint8_t exact[] = {0x10, 0x08, 0x00, 0x00, 0x40, 'a', 0x40, 0x00};
    ASSERT_TRUE(run_bad(exact, sizeof(exact), 8, 0xFFFFFFFFu));
    /* The source fails mid-stream, in the header, in the flags and in the data */
    for(uint32_t at = 0; at < sizeof(exact); at++) {
        ASSERT_TRUE(!run_bad(exact, sizeof(exact), 8, at));
    }
    /* The source runs dry before want bytes were made */
    static const uint8_t dry[] = {0x10, 0x08, 0x00, 0x00, 0x00, 'a', 'b'};
    ASSERT_TRUE(!run_bad(dry, sizeof(dry), 8, 0xFFFFFFFFu));
    /* A stream for 0x800 bytes that is cut short at 0x400 */
    static uint8_t lits[4 + 0x800 + 0x100];
    lits[0] = 0x10;
    lits[2] = 0x08;
    /* flag bytes 0, then 8 literals each: 9 bytes per 8 */
    uint32_t n = 4;
    for(uint32_t k = 0; k < 0x800 / 8; k++) {
        lits[n++] = 0;
        for(int j = 0; j < 8; j++) lits[n++] = (uint8_t)(k + j);
    }
    ASSERT_TRUE(run_bad(lits, n, 0x800, 0xFFFFFFFFu));
    ASSERT_TRUE(!run_bad(lits, n, 0x800, n / 2));
}

TEST(test_ub_pic_empty) {
    uint8_t raw[GEN3_UB_PIC_RAW] = {0};
    uint16_t pal[16] = {0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF,
                        0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF};
    uint8_t work[GEN3_UB_PIC_WORK];
    uint8_t out[GEN3_UB_PIC_RECORD];
    ASSERT_TRUE(!gen3_ub_pic_convert(raw, pal, work, out));
}

int main(void) {
    RUN(test_ub_pic_convert_vectors);
    RUN(test_ub_lz77_vectors);
    RUN(test_ub_lz77_vectors_are_compressed);
    RUN(test_ub_lz77_malformed);
    RUN(test_ub_pic_empty);
    TEST_EXIT();
}
