#include "test.h"
#include "fake_ub_save.h"
#include <stdbool.h>
#include <src/gen3/gen3_rules.h>
#include <src/gen3/gen3_text.h>
#include <src/gen3/gen3_ub_save.h>

/* The Pokemon Unbound save model (gen3_ub_save.h) on saves fake_ub_save.h builds */

static uint8_t img[FAKE_UB_LEN];
static Gen3UbSector infos[28];
static Gen3UbSlots slots;

static void analyze(void) {
    for(uint32_t k = 0; k < 28u; k++) gen3_ub_sector_info(img + k * 4096u, &infos[k]);
    gen3_ub_slots(infos, &slots);
}

static int growth3(uint16_t species, void* ctx) {
    (void)species;
    (void)ctx;
    return 3;
}

static int growth_unknown(uint16_t species, void* ctx) {
    (void)species;
    (void)ctx;
    return -1;
}

TEST(test_ub_slots_single) {
    fake_ub_erase(img);
    fake_ub_build(img, 0, 20, 3);
    analyze();
    ASSERT_EQ(slots.loaded, 0);
    ASSERT_TRUE(slots.valid[0]);
    ASSERT_TRUE(!slots.valid[1]);
    ASSERT_EQ(slots.counter[0], 20);
    for(uint8_t id = 0; id < 14u; id++) ASSERT_EQ(slots.phys[id], fake_ub_phys(img, 0, id));
    ASSERT_EQ(slots.phys[3], 0); /* rotate 3: id 3 at the slot's first sector */
    ASSERT_TRUE(infos[0].sig_ok);
    ASSERT_TRUE(infos[0].sum_ok);
    ASSERT_EQ(infos[0].counter, 20);
}

TEST(test_ub_slots_newer) {
    fake_ub_erase(img);
    fake_ub_build(img, 0, 20, 0);
    fake_ub_build(img, 1, 21, 5);
    analyze();
    ASSERT_EQ(slots.loaded, 1);
    ASSERT_EQ(slots.phys[0], fake_ub_phys(img, 1, 0));
    /* the counter wraps: 0 is newer than 0xFFFFFFFF */
    fake_ub_build(img, 0, 0xFFFFFFFFu, 0);
    fake_ub_build(img, 1, 0, 0);
    analyze();
    ASSERT_EQ(slots.loaded, 1);
    /* a tie: slot A */
    fake_ub_build(img, 0, 7, 0);
    fake_ub_build(img, 1, 7, 0);
    analyze();
    ASSERT_EQ(slots.loaded, 0);
}

TEST(test_ub_slots_bad_checksum) {
    fake_ub_erase(img);
    fake_ub_build(img, 0, 20, 0);
    fake_ub_build(img, 1, 21, 0);
    img[fake_ub_phys(img, 1, 7) * 4096u + 0x10] ^= 0x01;
    analyze();
    ASSERT_TRUE(!slots.valid[1]);
    ASSERT_EQ(slots.loaded, 0);
    /* section 4's checksum isn't checked (PUSE leaves it opaque) */
    fake_ub_build(img, 1, 21, 0);
    img[fake_ub_phys(img, 1, 4) * 4096u + 0x10] ^= 0x01;
    analyze();
    ASSERT_TRUE(slots.valid[1]);
    ASSERT_EQ(slots.loaded, 1);
    /* a byte past the checksum length doesn't count either: section 0's ends at 0xF24 */
    fake_ub_build(img, 1, 21, 0);
    img[fake_ub_phys(img, 1, 0) * 4096u + 0xF30] ^= 0x01;
    analyze();
    ASSERT_EQ(slots.loaded, 1);
    /* a counter that differs in one sector */
    fake_ub_build(img, 1, 21, 0);
    img[fake_ub_phys(img, 1, 9) * 4096u + 0xFFC] ^= 0x01;
    analyze();
    ASSERT_EQ(slots.loaded, 0);
    /* an id twice */
    fake_ub_build(img, 1, 21, 0);
    img[fake_ub_phys(img, 1, 9) * 4096u + 0xFF4] = 8;
    fake_ub_fix_checksums(img);
    analyze();
    ASSERT_EQ(slots.loaded, 0);
}

TEST(test_ub_slots_fr_signature) {
    fake_ub_erase(img);
    fake_ub_build(img, 0, 20, 0);
    for(uint32_t k = 0; k < 14u; k++) {
        uint8_t* s = img + k * 4096u;
        s[0xFF8] = 0x25;
        s[0xFF9] = 0x20;
        s[0xFFA] = 0x01;
        s[0xFFB] = 0x08;
    }
    analyze();
    ASSERT_EQ(slots.loaded, -1);
    ASSERT_TRUE(!infos[0].sig_ok);
    /* the other Unbound signature is one too */
    fake_ub_build(img, 0, 20, 0);
    img[0xFF8] = 0x98;
    fake_ub_fix_checksums(img);
    analyze();
    ASSERT_EQ(slots.loaded, 0);
    /* an empty (erased) save */
    fake_ub_erase(img);
    analyze();
    ASSERT_EQ(slots.loaded, -1);
}

TEST(test_ub_box_pieces_straddle) {
    fake_ub_erase(img);
    fake_ub_build(img, 0, 20, 2);
    analyze();
    Gen3UbPieces pc;
    /* record 70 (box 2 slot 10): stream bytes 4060..4118, across the end of section 5's 4076 */
    ASSERT_TRUE(gen3_ub_box_pieces(&slots, 2, 10, &pc));
    ASSERT_EQ(pc.n, 2);
    ASSERT_EQ(pc.sector[0], slots.phys[5]);
    ASSERT_EQ(pc.off[0], 4064);
    ASSERT_EQ(pc.len[0], 16);
    ASSERT_EQ(pc.sector[1], slots.phys[6]);
    ASSERT_EQ(pc.off[1], 0);
    ASSERT_EQ(pc.len[1], 42);
    /* the last record ends section 3's region at 3264 */
    ASSERT_TRUE(gen3_ub_box_pieces(&slots, 23, 29, &pc));
    ASSERT_EQ(pc.n, 1);
    ASSERT_EQ(pc.sector[0], slots.phys[3]);
    ASSERT_EQ(pc.off[0], 3264 - 58);
    ASSERT_EQ(pc.len[0], 58);
    /* the first */
    ASSERT_TRUE(gen3_ub_box_pieces(&slots, 0, 0, &pc));
    ASSERT_EQ(pc.n, 1);
    ASSERT_EQ(pc.sector[0], slots.phys[5]);
    ASSERT_EQ(pc.off[0], 4);
    /* out of range, and no slot loaded */
    ASSERT_TRUE(!gen3_ub_box_pieces(&slots, 24, 0, &pc));
    ASSERT_TRUE(!gen3_ub_box_pieces(&slots, 0, 30, &pc));
    fake_ub_erase(img);
    analyze();
    ASSERT_TRUE(!gen3_ub_box_pieces(&slots, 0, 0, &pc));
}

TEST(test_ub_box_pieces_shared) {
    fake_ub_erase(img);
    fake_ub_build(img, 1, 20, 0);
    analyze();
    Gen3UbPieces pc;
    /* record 570 (box 19 slot 0) starts the shared sector 30's region exactly */
    ASSERT_TRUE(gen3_ub_box_pieces(&slots, 19, 0, &pc));
    ASSERT_EQ(pc.n, 1);
    ASSERT_EQ(pc.sector[0], 30);
    ASSERT_EQ(pc.off[0], 2828);
    ASSERT_EQ(pc.len[0], 58);
}

TEST(test_ub_pieces_match_fake) {
    /* Every record, through the pieces, reads what fake_ub_put_box put */
    fake_ub_erase(img);
    fake_ub_build(img, 0, 20, 6);
    uint8_t rec[58];
    for(uint8_t box = 0; box < GEN3_UB_BOXES; box++) {
        for(uint8_t k = 0; k < GEN3_UB_BOX_SLOTS; k++) {
            for(uint32_t i = 0; i < 58u; i++) rec[i] = (uint8_t)(box * 31u + k * 7u + i);
            fake_ub_put_box(img, 0, box, k, rec);
        }
    }
    analyze();
    ASSERT_EQ(slots.loaded, 0);
    for(uint8_t box = 0; box < GEN3_UB_BOXES; box++) {
        for(uint8_t k = 0; k < GEN3_UB_BOX_SLOTS; k++) {
            Gen3UbPieces pc;
            ASSERT_TRUE(gen3_ub_box_pieces(&slots, box, k, &pc));
            uint8_t got[58];
            uint32_t at = 0;
            for(uint8_t j = 0; j < pc.n; j++) {
                memcpy(got + at, img + pc.sector[j] * 4096u + pc.off[j], pc.len[j]);
                at += pc.len[j];
            }
            ASSERT_EQ(at, 58);
            for(uint32_t i = 0; i < 58u; i++) ASSERT_EQ(got[i], (uint8_t)(box * 31u + k * 7u + i));
        }
    }
}

TEST(test_ub_summary) {
    uint8_t nick[10];
    gen3_text_encode("TEST", nick, 10, 10);
    uint8_t rec[58];
    fake_ub_box_rec(rec, 1000, gen3_exp_for_level(3, 50), nick, 0);
    Gen3UbSummary m;
    gen3_ub_box_summary(rec, growth3, NULL, &m);
    ASSERT_EQ(m.species, 1000);
    ASSERT_EQ(m.level, 50);
    ASSERT_TRUE(!m.egg);
    ASSERT_STR_EQ(m.nickname, "TEST");
    gen3_ub_box_summary(rec, growth_unknown, NULL, &m);
    ASSERT_EQ(m.level, 0);
    fake_ub_box_rec(rec, 1000, gen3_exp_for_level(3, 5), nick, 1u << 30);
    gen3_ub_box_summary(rec, growth3, NULL, &m);
    ASSERT_TRUE(m.egg);
    ASSERT_EQ(m.level, 5);
    /* the hidden-ability flag is bit 31, not the egg */
    fake_ub_box_rec(rec, 1000, 100, nick, 1u << 31);
    gen3_ub_box_summary(rec, growth3, NULL, &m);
    ASSERT_TRUE(!m.egg);

    uint8_t party[100];
    fake_ub_party_rec(party, 1265, 37, nick, 0);
    gen3_ub_party_summary(party, &m);
    ASSERT_EQ(m.species, 1265);
    ASSERT_EQ(m.level, 37);
    ASSERT_TRUE(!m.egg);
    ASSERT_STR_EQ(m.nickname, "TEST");
    fake_ub_party_rec(party, 1265, 1, nick, 1u << 30);
    gen3_ub_party_summary(party, &m);
    ASSERT_TRUE(m.egg);
}

TEST(test_ub_trainer) {
    fake_ub_erase(img);
    fake_ub_build(img, 0, 20, 4);
    uint8_t name[8];
    gen3_text_encode("RED", name, 8, 7);
    fake_ub_put_trainer(img, 0, name, 1, 12345, 54321);
    fake_ub_set_party_count(img, 0, 4);
    analyze();
    Gen3UbTrainer t;
    gen3_ub_trainer(img + slots.phys[0] * 4096u, &t);
    ASSERT_STR_EQ(t.name, "RED");
    ASSERT_EQ(t.gender, 1);
    ASSERT_EQ(t.tid, 12345);
    ASSERT_EQ(t.sid, 54321);
    ASSERT_EQ(gen3_ub_party_count(img + slots.phys[1] * 4096u), 4);
    fake_ub_set_party_count(img, 0, 9);
    ASSERT_EQ(gen3_ub_party_count(img + slots.phys[1] * 4096u), 6);
}

TEST(test_ub_footer_write) {
    /* the section checksum over each id's length: the one the fake save has */
    fake_ub_erase(img);
    fake_ub_build(img, 0, 20, 3);
    analyze();
    for(uint8_t id = 0; id < 14u; id++) {
        const uint8_t* s = img + slots.phys[id] * 4096u;
        ASSERT_EQ(gen3_ub_section_sum(s, id), infos[slots.phys[id]].checksum);
    }
    ASSERT_EQ(gen3_ub_section_sum(img, 14), 0);
    /* a byte past section 13's length (0x450) doesn't count */
    uint8_t* s13 = img + slots.phys[13] * 4096u;
    const uint16_t sum13 = gen3_ub_section_sum(s13, 13);
    s13[0x460] ^= 0x01;
    ASSERT_EQ(gen3_ub_section_sum(s13, 13), sum13);
    s13[0x440] ^= 0x01;
    ASSERT_TRUE(gen3_ub_section_sum(s13, 13) != sum13);
    /* a footer written, and read back; nothing else changes */
    static uint8_t sec[4096], was[4096];
    for(uint32_t i = 0; i < 4096u; i++) sec[i] = (uint8_t)(i * 7u);
    memcpy(was, sec, sizeof(sec));
    const Gen3UbSector f = {
        .id = 9, .checksum = 0xBEEF, .signature = GEN3_UB_SIG_B, .counter = 0x12345678u};
    gen3_ub_set_footer(sec, &f);
    Gen3UbSector g;
    gen3_ub_sector_info(sec, &g);
    ASSERT_EQ(g.id, 9);
    ASSERT_EQ(g.checksum, 0xBEEF);
    ASSERT_EQ(g.signature, GEN3_UB_SIG_B);
    ASSERT_EQ(g.counter, 0x12345678u);
    ASSERT_TRUE(g.sig_ok);
    ASSERT_MEM_EQ(sec, was, 0xFF4);
    /* with its checksum put right, the sector is sound */
    const Gen3UbSector h = {
        .id = 9,
        .checksum = gen3_ub_section_sum(sec, 9),
        .signature = GEN3_UB_SIG_A,
        .counter = 1,
    };
    gen3_ub_set_footer(sec, &h);
    gen3_ub_sector_info(sec, &g);
    ASSERT_TRUE(g.sum_ok);
}

int main(void) {
    RUN(test_ub_slots_single);
    RUN(test_ub_slots_newer);
    RUN(test_ub_slots_bad_checksum);
    RUN(test_ub_slots_fr_signature);
    RUN(test_ub_box_pieces_straddle);
    RUN(test_ub_box_pieces_shared);
    RUN(test_ub_pieces_match_fake);
    RUN(test_ub_summary);
    RUN(test_ub_trainer);
    RUN(test_ub_footer_write);
    TEST_EXIT();
}
