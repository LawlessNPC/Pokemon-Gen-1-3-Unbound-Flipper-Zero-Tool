#include "test.h"
#include "bulk_file.h"
#include "club_bench.h"
#include "trade_mons.h"
#include <stdbool.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_partner.h>
#include <src/gen3/gen3_text.h>

/* The Flipper (club) and the fake in FRLG mode (docs/gen3/link-protocol-trade.md Part C). Unless a
 * test says otherwise it starts with bench_init_frlg(&b, GEN3_VERSION_FIRERED, false): the fake is
 * FireRed, RED 13579/24680 without the National Dex, holding bench_init's three Pokemon; the
 * Flipper is FLIPPER 22222 (club_cfg_trade) offering CHARMANDER. */

/* FLIPPER, EOS-filled to 8 bytes (the name in a card, and name[0..7] of a LinkPlayer) */
static const uint8_t flipper_name[8] = {0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC, 0xFF};
static const uint16_t link_types[] = {
    GEN3_LINKTYPE_TRADE_SETUP,
    GEN3_LINKTYPE_TRADE_CONNECTING,
    GEN3_LINKTYPE_TRADE_DISCONNECTED,
    GEN3_LINKTYPE_TRADE,
};

/* name (11 bytes) decodes to want, up to its first EOS */
static bool name_is(const uint8_t name[11], const char* want) {
    char s[12];
    gen3_text_decode(name, 11, s, sizeof(s));
    return strcmp(s, want) == 0;
}

/* gen3_club_claim_game for a partner of `version` (link type partner_type), a session of
 * link_type and the National Dex opt-in natdex gives the FRLG claim (§C1, §C2): that version,
 * lp_field_2 0x8000, progress 01 00 01 and the FRLG card with the version byte. */
static bool
    frlg_claim_ok(uint16_t version, uint32_t partner_type, uint32_t link_type, bool natdex) {
    static const uint8_t name[11] = {
        0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC, 0xFF, 0x01, 0x00, 0x01};
    Gen3LinkPlayer partner = bench_frlg_player(version);
    Gen3LinkPlayer lp;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    uint8_t want[GEN3_TRAINER_CARD_BLOCK_SIZE];
    partner.link_type = partner_type;
    memset(&lp, 0xAA, sizeof(lp));
    memset(card, 0xAA, sizeof(card));
    gen3_club_claim_game(&club_cfg_trade, &partner, link_type, natdex, &lp, card);
    gen3_trainer_card_build_frlg(&lp, want);
    const bool ok = lp.version == version && lp.lp_field_2 == 0x8000 && lp.trainer_id == 22222 &&
                    lp.gender == 0 && lp.id == 0 && lp.language == 2 &&
                    lp.link_type == link_type && memcmp(lp.name, name, sizeof(name)) == 0 &&
                    memcmp(card, want, sizeof(card)) == 0 && card[0x38] == (version & 0xFF);
    if(!ok) {
        printf(
            "  claim for 0x%04X (type 0x%04X), link type 0x%04X, natdex %d: version 0x%04X\n",
            version,
            (unsigned)partner_type,
            (unsigned)link_type,
            natdex,
            lp.version);
    }
    return ok;
}

/* The same for partner (NULL allowed) gives the Ruby claim, byte for byte as before FRLG
 * support (test_club_claim_game). */
static bool ruby_claim_ok(const Gen3LinkPlayer* partner, uint32_t link_type, bool natdex) {
    static const uint8_t name[11] = {
        0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC, 0xFF, 0xFF, 0xFF, 0xFF};
    Gen3LinkPlayer lp;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    uint8_t want[GEN3_TRAINER_CARD_BLOCK_SIZE];
    memset(&lp, 0xAA, sizeof(lp));
    memset(card, 0xAA, sizeof(card));
    gen3_club_claim_game(&club_cfg_trade, partner, link_type, natdex, &lp, card);
    gen3_trainer_card_build(&lp, want);
    const uint16_t field2 = link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED && natdex ? 0x8000 : 0;
    const bool ok = lp.version == 0x4002 && lp.lp_field_2 == field2 && lp.trainer_id == 22222 &&
                    lp.gender == 0 && lp.id == 0 && lp.language == 2 &&
                    lp.link_type == link_type && memcmp(lp.name, name, sizeof(name)) == 0 &&
                    memcmp(card, want, sizeof(card)) == 0;
    if(!ok) {
        printf(
            "  claim for 0x%04X, link type 0x%04X, natdex %d: version 0x%04X\n",
            partner ? partner->version : 0,
            (unsigned)link_type,
            natdex,
            lp.version);
    }
    return ok;
}

TEST(test_frlg_claim_game) {
    static const uint16_t ruby_versions[] = {0x4001, 0x4002, 0x4003, 0x4000};
    for(size_t t = 0; t < sizeof(link_types) / sizeof(link_types[0]); t++) {
        for(int natdex = 0; natdex <= 1; natdex++) {
            ASSERT_TRUE(frlg_claim_ok(0x4004, link_types[t], link_types[t], natdex));
            ASSERT_TRUE(frlg_claim_ok(0x4005, link_types[t], link_types[t], natdex));
            ASSERT_TRUE(ruby_claim_ok(NULL, link_types[t], natdex));
            for(size_t v = 0; v < sizeof(ruby_versions) / sizeof(ruby_versions[0]); v++) {
                Gen3LinkPlayer partner = bench_frlg_player(ruby_versions[v]);
                partner.link_type = link_types[t];
                ASSERT_TRUE(ruby_claim_ok(&partner, link_types[t], natdex));
            }
            /* A bad block or a reset leaves the club's partner zeroed: Unknown, so Ruby */
            Gen3LinkPlayer zero;
            memset(&zero, 0, sizeof(zero));
            ASSERT_TRUE(ruby_claim_ok(&zero, link_types[t], natdex));
        }
    }
    /* An FRLG GBA in another room gets the FRLG claim too; the link types differ first */
    ASSERT_TRUE(frlg_claim_ok(0x4004, 0x2233, GEN3_LINKTYPE_TRADE_SETUP, false));
}

TEST(test_frlg_gba_block) {
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, true);
    ASSERT_TRUE(bench_to_room(&b));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.partner_valid);
    ASSERT_EQ(st.partner.version, 0x4004);
    ASSERT_EQ(st.partner.lp_field_2, 0x8000);
    ASSERT_EQ(st.partner.name[8], 0x01);
    ASSERT_EQ(st.partner.name[9], 0x00);
    ASSERT_EQ(st.partner.name[10], 0x00); /* a GBA never writes its own progressFlagsCopy */
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(st.partner_rules.national_dex);
    ASSERT_TRUE(st.trainer_valid);
    ASSERT_TRUE(name_is(st.trainer.name, "RED"));
    ASSERT_EQ(st.trainer.trainer_id, (24680u << 16) | 13579u);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_to_room(&b));
    st = club_status(&b);
    ASSERT_EQ(st.partner.name[8], 0x00);
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(!st.partner_rules.national_dex);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_claim_on_wire) {
    Bench b;
    char name[12];
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    /* The Flipper's LinkPlayer as FireRed converted it */
    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->version, 0x4004);
    ASSERT_EQ(p->lp_field_2, 0x8000);
    ASSERT_EQ(p->trainer_id, 22222);
    gen3_text_decode(p->name, sizeof(p->name), name, sizeof(name));
    ASSERT_STR_EQ(name, "FLIPPER");
    ASSERT_EQ(p->name[8], 0x01);
    ASSERT_EQ(p->name[9], 0x00);
    ASSERT_EQ(p->name[10], 0x01);
    /* The FRLG card: Ruby's layout, then the version byte, then zeros (§C2) */
    const uint8_t* card = fake_ruby_partner_card(&b.ruby);
    ASSERT_EQ(card[0x00], 0); /* gender */
    ASSERT_EQ(card[0x01], 0); /* stars */
    ASSERT_EQ(card[0x02], 1); /* hasPokedex */
    ASSERT_EQ(card[0x0E], 0xCE); /* 22222 */
    ASSERT_EQ(card[0x0F], 0x56);
    ASSERT_MEM_EQ(card + 0x30, flipper_name, sizeof(flipper_name));
    ASSERT_EQ(card[0x38], 0x04);
    for(size_t i = 0x39; i < GEN3_TRAINER_CARD_BLOCK_SIZE; i++) ASSERT_EQ(card[i], 0);
}

TEST(test_frlg_leafgreen) {
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_LEAFGREEN, true);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->version, 0x4005);
    ASSERT_EQ(fake_ruby_partner_card(&b.ruby)[0x38], 0x05);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(name_is(st.trainer.name, "LEAF"));
    ASSERT_EQ(st.trainer.gender, 1);
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(st.partner_rules.national_dex);
}

TEST(test_frlg_ruby_claim_refused) {
    /* §C1: FRLG refuses a Ruby partner at the counter unless the save may link with R/S */
    Bench b;
    Gen3LinkPlayer ruby;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    gen3_club_claim_game(&club_cfg_trade, NULL, GEN3_LINKTYPE_TRADE_SETUP, false, &ruby, card);
    ASSERT_EQ(ruby.version, GEN3_VERSION_RUBY);

    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    fake_ruby_open_link(&b.ruby);
    bench_raw_relink(&b, &ruby, 200);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "not ready");
    ASSERT_EQ(b.n_g_close, 0); /* a plain CloseLink: no 5FFF */
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);

    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    fake_ruby_set_frlg_progress(&b.ruby, false, true);
    fake_ruby_open_link(&b.ruby);
    bench_raw_relink(&b, &ruby, 200);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(b.n_g_close, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyLinkup); /* past the gate, awaiting the cards */
    /* An R/S sender's progress bytes are zeroed on receipt (fr: link.c: ProcessRecvCmds) */
    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->version, GEN3_VERSION_RUBY);
    ASSERT_EQ(p->name[8], 0);
    ASSERT_EQ(p->name[9], 0);
    ASSERT_EQ(p->name[10], 0);
}

/* The fake's linkup gate against a raw Flipper LinkPlayer: the Ruby claim with `version` and raw
 * name[8], name[10], against a FireRed save with or without FLAG_SYS_CAN_LINK_WITH_RS. "passed"
 * once both LinkPlayers are in with no error, else the fake's error, or "waiting". */
static const char* gate(uint16_t version, uint8_t name8, uint8_t name10, bool can_link_rs) {
    Bench b;
    Gen3LinkPlayer lp;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    gen3_club_claim_game(&club_cfg_trade, NULL, GEN3_LINKTYPE_TRADE_SETUP, false, &lp, card);
    lp.version = version;
    lp.name[8] = name8;
    lp.name[9] = 0;
    lp.name[10] = name10;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    fake_ruby_set_frlg_progress(&b.ruby, false, can_link_rs);
    fake_ruby_open_link(&b.ruby);
    bench_raw_relink(&b, &lp, 200);
    if(fake_ruby_error(&b.ruby)) return fake_ruby_error(&b.ruby);
    return fake_ruby_partner(&b.ruby)->version == version ? "passed" : "waiting";
}

TEST(test_frlg_fake_gate) {
    /* fr: trade.c: GetGameProgressForLinkTrade. An FRLG partner is always ready */
    ASSERT_STR_EQ(gate(0x4004, 0x00, 0x00, false), "passed");
    ASSERT_STR_EQ(gate(0x4005, 0x00, 0x00, false), "passed");
    /* R/S: only this save's own 0xF0 bits count; the partner's bytes are zeroed */
    ASSERT_STR_EQ(gate(0x4001, 0x10, 0x10, true), "passed");
    ASSERT_STR_EQ(gate(0x4002, 0x10, 0x10, false), "not ready");
    /* Emerald or other: both saves. The partner's copy is the receiver's: name[10] = name[8]
     * (fr: link.c: ConvertLinkPlayerName), whatever was sent in name[10] */
    ASSERT_STR_EQ(gate(0x4003, 0x10, 0x00, true), "passed");
    ASSERT_STR_EQ(gate(0x4000, 0x80, 0x00, true), "passed");
    ASSERT_STR_EQ(gate(0x4003, 0x00, 0x10, true), "not ready");
    ASSERT_STR_EQ(gate(0x4003, 0x01, 0x01, true), "not ready");
    ASSERT_STR_EQ(gate(0x4003, 0x10, 0x10, false), "not ready");
}

/* The club's card for this session, poked before it goes out (white box: the club builds it with
 * its LinkPlayerBlock and sends it card_delay idle bursts later), then the run to the room. */
static const char* card_poked(uint8_t offset, uint8_t value) {
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    fake_ruby_open_link(&b.ruby);
    if(!run_until_phase(&b, Gen3ClubPhasePlayers, 200)) return "no players";
    b.club.own_card[offset] = value;
    if(run_until_room(&b, 400) && fake_ruby_error(&b.ruby) == NULL) return "room";
    return fake_ruby_error(&b.ruby) ? fake_ruby_error(&b.ruby) : "stuck";
}

TEST(test_frlg_fake_bad_card) {
    /* fr: cable_club.c: Task_LinkupAwaitTrainerCardData copies an FRLG partner's card whole */
    ASSERT_STR_EQ(card_poked(0x38, 0x04), "room");
    ASSERT_STR_EQ(card_poked(0x38, 0x00), "bad card"); /* the Ruby card for a FireRed claim */
    ASSERT_STR_EQ(card_poked(0x38, 0x05), "bad card"); /* LeafGreen's card for a FireRed claim */
    ASSERT_STR_EQ(card_poked(0x01, 0x04), "room"); /* 4 stars: the last palette */
    ASSERT_STR_EQ(card_poked(0x01, 0x05), "bad card");
}

TEST(test_frlg_room_and_leave) {
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_to_room(&b));
    run(&b, 1800);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_EQ(st.link.checksum_errors, 0);

    ASSERT_TRUE(leave_room(&b));
    run(&b, 120);
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_EQ(b.sent_cafe, 1); /* nothing but that answer: the GBA never sat */
    ASSERT_EQ(b.sent_5fff, 1);
    ASSERT_EQ(b.n_g_close, 1);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    /* The partner's rules outlive its link */
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(!st.partner_rules.national_dex);
}

TEST(test_frlg_link_test) {
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    gen3_club_init(&b.club, &club_cfg);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->version, 0x4004);
    ASSERT_EQ(fake_ruby_partner_card(&b.ruby)[0x38], 0x04);
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 600);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_swap_cartridge) {
    /* Ruby, then FireRed, then Ruby again in one session: each claim follows the GBA linked now */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    const uint8_t* card = fake_ruby_partner_card(&b.ruby);
    ASSERT_MEM_EQ(card + 0x30, flipper_name, sizeof(flipper_name)); /* Ruby mode keeps it too */
    ASSERT_EQ(card[0x38], 0);
    ASSERT_TRUE(leave_room(&b));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    const Gen3LinkPlayer fr = bench_frlg_player(GEN3_VERSION_FIRERED);
    bench_swap_gba(&b, &fr, false);
    ASSERT_TRUE(bench_to_room(&b));
    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->version, 0x4004);
    ASSERT_EQ(p->name[8], 0x01);
    ASSERT_EQ(fake_ruby_partner_card(&b.ruby)[0x38], 0x04);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(name_is(st.trainer.name, "RED"));
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(!st.partner_rules.national_dex);
    ASSERT_TRUE(leave_room(&b));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    const Gen3LinkPlayer ruby = bench_ruby_player();
    bench_swap_gba(&b, &ruby, false);
    ASSERT_TRUE(bench_to_room(&b));
    p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->version, 0x4002);
    ASSERT_EQ(p->lp_field_2, 0);
    ASSERT_EQ(p->name[7], 0xFF);
    ASSERT_EQ(p->name[8], 0xFF);
    ASSERT_EQ(p->name[9], 0xFF);
    ASSERT_EQ(p->name[10], 0xFF);
    ASSERT_EQ(fake_ruby_partner_card(&b.ruby)[0x38], 0);
    st = club_status(&b);
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameRS);
    ASSERT_TRUE(st.partner_rules.national_dex);
    ASSERT_TRUE(name_is(st.trainer.name, "RUBY"));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_frlg_claim_game);
    RUN(test_frlg_gba_block);
    RUN(test_frlg_claim_on_wire);
    RUN(test_frlg_leafgreen);
    RUN(test_frlg_ruby_claim_refused);
    RUN(test_frlg_fake_gate);
    RUN(test_frlg_fake_bad_card);
    RUN(test_frlg_room_and_leave);
    RUN(test_frlg_link_test);
    RUN(test_frlg_swap_cartridge);
    TEST_EXIT();
}
