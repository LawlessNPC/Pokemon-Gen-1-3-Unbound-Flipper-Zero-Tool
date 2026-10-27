#include "fake_save.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_SAVE_DATA_LEN 0xF80u /* a sector's data area (multiboot-and-save.md §5.2) */
#define FAKE_SAVE_OFF_ID 0xFF4u
#define FAKE_SAVE_OFF_CHECKSUM 0xFF6u
#define FAKE_SAVE_OFF_SIGNATURE 0xFF8u
#define FAKE_SAVE_OFF_COUNTER 0xFFCu
#define FAKE_SAVE_SECTORS 32u

static void fake_save_misuse(const char* what, uint32_t v) {
    fprintf(stderr, "fake_save: %s (%u)\n", what, (unsigned)v);
    abort();
}

static uint8_t* fake_save_sector(uint8_t* img, uint8_t sector) {
    if(sector >= FAKE_SAVE_SECTORS) fake_save_misuse("sector", sector);
    return img + (uint32_t)sector * FAKE_SAVE_SECTOR_LEN;
}

static void fake_save_put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void fake_save_put32(uint8_t* p, uint32_t v) {
    for(uint32_t i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* multiboot-and-save.md §5.4: ids 0, 4 and 13 are short; the rest fill all 0xF80 bytes. */
uint16_t fake_save_size(FakeGame game, uint8_t id) {
    if(id >= FAKE_SAVE_SLOT_SECTORS) fake_save_misuse("id", id);
    if(id == 13) return 0x7D0;
    if(id != 0 && id != 4) return FAKE_SAVE_DATA_LEN;
    switch(game) {
    case FakeGameRS:
        return id == 0 ? 0x890 : 0xC40;
    case FakeGameFRLG:
        return id == 0 ? 0xF24 : 0xEE8;
    case FakeGameE:
        return id == 0 ? 0xF2C : 0xF08;
    }
    fake_save_misuse("game", (uint32_t)game);
    return 0;
}

/* multiboot-and-save.md §5.3: the 32-bit sum of the first len / 4 little-endian words, folded
 * to 16 bits as hi + lo. */
static uint16_t fake_save_checksum(const uint8_t* data, uint32_t len) {
    uint32_t sum = 0;
    for(uint32_t i = 0; i + 4 <= len; i += 4) {
        sum += (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 | (uint32_t)data[i + 2] << 16 |
               (uint32_t)data[i + 3] << 24;
    }
    return (uint16_t)((sum >> 16) + sum);
}

uint32_t fake_save_phys(const FakeSaveSpec* spec, uint8_t slot, uint8_t id) {
    if(slot > 1 || id >= FAKE_SAVE_SLOT_SECTORS || spec->first[slot] >= FAKE_SAVE_SLOT_SECTORS) {
        fake_save_misuse("slot, id or first", (uint32_t)slot << 8 | id);
    }
    return FAKE_SAVE_SLOT_SECTORS * slot + (id + spec->first[slot]) % FAKE_SAVE_SLOT_SECTORS;
}

void fake_save_build(uint8_t img[131072], const FakeSaveSpec* spec) {
    memset(img, 0xFF, FAKE_SAVE_LEN);
    for(uint8_t slot = 0; slot < 2; slot++) {
        if(!spec->slot_present[slot]) continue;
        for(uint8_t id = 0; id < FAKE_SAVE_SLOT_SECTORS; id++) {
            uint8_t* s = fake_save_sector(img, (uint8_t)fake_save_phys(spec, slot, id));
            const uint16_t size = fake_save_size(spec->game, id);
            memset(s, 0, FAKE_SAVE_SECTOR_LEN);
            uint32_t x = spec->seed ^
                         ((uint32_t)(slot * FAKE_SAVE_SLOT_SECTORS + id + 1u) * 0x9E3779B9u);
            for(uint32_t i = 0; i < size; i++) {
                x = x * 1664525u + 1013904223u;
                s[i] = (uint8_t)(x >> 24);
            }
            fake_save_put16(s + FAKE_SAVE_OFF_ID, id);
            fake_save_put16(s + FAKE_SAVE_OFF_CHECKSUM, fake_save_checksum(s, size));
            fake_save_put32(s + FAKE_SAVE_OFF_SIGNATURE, 0x08012025u);
            fake_save_put32(s + FAKE_SAVE_OFF_COUNTER, spec->counter[slot]);
        }
    }
}

void fake_save_fix(uint8_t img[131072], uint8_t sector, FakeGame game) {
    uint8_t* s = fake_save_sector(img, sector);
    const uint16_t id = (uint16_t)(s[FAKE_SAVE_OFF_ID] | s[FAKE_SAVE_OFF_ID + 1] << 8);
    const uint32_t size = id < FAKE_SAVE_SLOT_SECTORS ? fake_save_size(game, (uint8_t)id) :
                                                        FAKE_SAVE_DATA_LEN;
    fake_save_put16(s + FAKE_SAVE_OFF_CHECKSUM, fake_save_checksum(s, size));
}

void fake_save_set_counter(uint8_t img[131072], uint8_t sector, uint32_t counter) {
    fake_save_put32(fake_save_sector(img, sector) + FAKE_SAVE_OFF_COUNTER, counter);
}

void fake_save_set_id(uint8_t img[131072], uint8_t sector, uint16_t id) {
    fake_save_put16(fake_save_sector(img, sector) + FAKE_SAVE_OFF_ID, id);
}

void fake_save_break_checksum(uint8_t img[131072], uint8_t sector) {
    uint8_t* s = fake_save_sector(img, sector) + FAKE_SAVE_OFF_CHECKSUM;
    fake_save_put16(s, (uint16_t)((s[0] | s[1] << 8) + 1u));
}
