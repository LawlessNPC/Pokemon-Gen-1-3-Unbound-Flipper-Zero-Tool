#include "fake_ub_save.h"

#include <string.h>

#define SEC 4096u

static const uint16_t sum_len[14] = {
    0xF24, 0xFF0, 0xFF0, 0xFF0, 0xD98, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0xFF0, 0x450};

/* The PC stream: (id, start, end); id 30 and 31 are physical sectors */
static const uint16_t region[13][3] = {
    {5, 4, 4080},
    {6, 0, 4080},
    {7, 0, 4080},
    {8, 0, 4080},
    {9, 0, 4080},
    {10, 0, 4080},
    {11, 0, 4080},
    {12, 0, 4080},
    {13, 0, 424},
    {30, 2828, 4080},
    {31, 0, 3968},
    {2, 3864, 4080},
    {3, 0, 3264},
};

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t* p, uint32_t v) {
    wr16(p, (uint16_t)v);
    wr16(p + 2, (uint16_t)(v >> 16));
}

static void fix(uint8_t* s) {
    const uint16_t id = (uint16_t)(s[0xFF4] | s[0xFF5] << 8);
    if(id >= 14u) return;
    uint32_t sum = 0;
    for(uint32_t i = 0; i < sum_len[id]; i += 4) sum += rd32(s + i);
    wr16(s + 0xFF6, (uint16_t)((sum & 0xFFFFu) + (sum >> 16)));
}

void fake_ub_erase(uint8_t img[FAKE_UB_LEN]) {
    memset(img, 0xFF, FAKE_UB_LEN);
}

void fake_ub_build(uint8_t img[FAKE_UB_LEN], uint8_t slot, uint32_t counter, uint8_t rotate) {
    for(uint8_t k = 0; k < 14u; k++) {
        const uint8_t id = (uint8_t)((k + rotate) % 14u);
        uint8_t* s = img + (slot * 14u + k) * SEC;
        memset(s, 0, SEC);
        s[0x100] = (uint8_t)(id + 1u);
        s[0x101] = (uint8_t)counter;
        wr16(s + 0xFF4, id);
        wr32(s + 0xFF8, FAKE_UB_SIG);
        wr32(s + 0xFFC, counter);
        fix(s);
    }
    memset(img + 30u * SEC, 0, 2u * SEC);
}

uint8_t fake_ub_phys(const uint8_t img[FAKE_UB_LEN], uint8_t slot, uint8_t id) {
    for(uint8_t k = 0; k < 14u; k++) {
        const uint8_t* s = img + (slot * 14u + k) * SEC;
        if((s[0xFF4] | s[0xFF5] << 8) == id) return (uint8_t)(slot * 14u + k);
    }
    return 0xFF;
}

/* Writes n bytes at stream offset off */
static void stream_put(uint8_t* img, uint8_t slot, uint32_t off, const uint8_t* p, uint32_t n) {
    uint32_t base = 0;
    for(uint32_t r = 0; r < 13u && n; r++) {
        const uint32_t len = region[r][2] - region[r][1];
        if(off < base + len) {
            const uint32_t at = off - base;
            const uint32_t take = len - at < n ? len - at : n;
            const uint8_t sec = region[r][0] >= 30u ? (uint8_t)region[r][0] : fake_ub_phys(img, slot, (uint8_t)region[r][0]);
            memcpy(img + sec * SEC + region[r][1] + at, p, take);
            if(sec < 28u) fix(img + sec * SEC);
            off += take;
            p += take;
            n -= take;
        }
        base += len;
    }
}

void fake_ub_put_box(uint8_t img[FAKE_UB_LEN], uint8_t slot, uint8_t box, uint8_t k, const uint8_t rec[58]) {
    stream_put(img, slot, (box * 30u + k) * 58u, rec, 58);
}

void fake_ub_put_party(uint8_t img[FAKE_UB_LEN], uint8_t slot, uint8_t i, const uint8_t rec[100]) {
    uint8_t* s = img + fake_ub_phys(img, slot, 1) * SEC;
    memcpy(s + 0x38u + i * 100u, rec, 100);
    fix(s);
}

void fake_ub_set_party_count(uint8_t img[FAKE_UB_LEN], uint8_t slot, uint8_t count) {
    uint8_t* s = img + fake_ub_phys(img, slot, 1) * SEC;
    s[0x34] = count;
    fix(s);
}

void fake_ub_put_trainer(
    uint8_t img[FAKE_UB_LEN],
    uint8_t slot,
    const uint8_t name[8],
    uint8_t gender,
    uint16_t tid,
    uint16_t sid) {
    uint8_t* s = img + fake_ub_phys(img, slot, 0) * SEC;
    memcpy(s, name, 8);
    s[8] = gender;
    wr16(s + 0x0A, tid);
    wr16(s + 0x0C, sid);
    fix(s);
}

void fake_ub_fix_checksums(uint8_t img[FAKE_UB_LEN]) {
    for(uint32_t k = 0; k < 28u; k++) {
        uint8_t* s = img + k * SEC;
        const uint32_t sig = rd32(s + 0xFF8);
        if(sig == 0x01121999u || sig == 0x01121998u) fix(s);
    }
}

void fake_ub_box_rec(uint8_t rec[58], uint16_t species, uint32_t exp, const uint8_t nick[10], uint32_t ivs) {
    memset(rec, 0, 58);
    wr32(rec, 0x12345678u);
    memcpy(rec + 8, nick, 10);
    wr16(rec + 28, species);
    wr32(rec + 32, exp);
    wr32(rec + 54, ivs);
}

void fake_ub_party_rec(uint8_t rec[100], uint16_t species, uint8_t level, const uint8_t nick[10], uint32_t ivs) {
    memset(rec, 0, 100);
    wr32(rec, 0x12345678u);
    memcpy(rec + 8, nick, 10);
    wr16(rec + 32, species);
    wr32(rec + 72, ivs);
    rec[84] = level;
}
