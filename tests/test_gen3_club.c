#include "test.h"
#include "fake_ruby.h"
#include <stdbool.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_text.h>

static const Gen3ClubConfig club_cfg = {"FLIPPER", 22222, 0, 0};

/* Flipper (club) and fake Ruby wired back to back. rx is the fake's answer to the last tx. */
typedef struct {
    Gen3Club club;
    FakeRuby ruby;
    Gen3LinkPlayer me; /* the fake's LinkPlayer, kept for fake_ruby_init after a reset */
    MlinkRx rx;
    bool started;
    /* Flipper commands by word 0; sent_cafe17 counts CAFE 0017 */
    uint32_t sent_2222, sent_cccc, sent_5fff, sent_2ffe, sent_8888, sent_cafe, sent_cafe17;
} Bench;

static void bench_init(Bench* b, uint32_t ruby_link_type) {
    memset(b, 0, sizeof(*b));
    b->me.version = GEN3_VERSION_RUBY;
    b->me.trainer_id = (54321u << 16) | 12345u;
    gen3_text_encode("RUBY", b->me.name, sizeof(b->me.name), 7);
    b->me.gender = 0;
    b->me.link_type = ruby_link_type;
    b->me.language = GEN3_LANGUAGE_ENGLISH;
    gen3_club_init(&b->club, &club_cfg);
    fake_ruby_init(&b->ruby, &b->me);
}

/* The GBA restarts (or its player is back at the counter) and links again; same club. */
static void ruby_relink(Bench* b) {
    fake_ruby_init(&b->ruby, &b->me);
    fake_ruby_open_link(&b->ruby);
}

static void count_sent(Bench* b, const MlinkTx* tx) {
    if(tx->count != MLINK_BURST_LEN) return;
    if(tx->word[1] == 0x2222) b->sent_2222++;
    if(tx->word[1] == 0xCCCC) b->sent_cccc++;
    if(tx->word[1] == 0x5FFF) b->sent_5fff++;
    if(tx->word[1] == 0x2FFE) b->sent_2ffe++;
    if(tx->word[1] == 0x8888) b->sent_8888++;
    if(tx->word[1] == 0xCAFE) {
        b->sent_cafe++;
        if(tx->word[2] == 0x0017) b->sent_cafe17++;
    }
}

static void step(Bench* b) {
    MlinkTx tx;
    gen3_club_frame(&b->club, b->started ? &b->rx : NULL, &tx);
    b->started = true;
    count_sent(b, &tx);
    fake_ruby_frame(&b->ruby, &tx, &b->rx);
}

static void run(Bench* b, uint32_t frames) {
    for(uint32_t i = 0; i < frames; i++) step(b);
}

static Gen3ClubStatus club_status(const Bench* b) {
    Gen3ClubStatus st;
    gen3_club_status(&b->club, &st);
    return st;
}

static bool run_until_phase(Bench* b, Gen3ClubPhase phase, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(club_status(b).phase == phase) return true;
    }
    return false;
}

static bool at_cards_or_later(Gen3ClubPhase p) {
    return p == Gen3ClubPhaseCards || p == Gen3ClubPhaseRoom;
}

/* Until both sides are in the Trade Center: the club answered 2FFE, the fake got that answer. */
static bool run_until_room(Bench* b, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        bool club_in = club_status(b).phase == Gen3ClubPhaseRoom;
        if(club_in && fake_ruby_phase(&b->ruby) == FakeRubyRoom) return true;
    }
    return false;
}

static bool reach_room(Bench* b) {
    bench_init(b, GEN3_LINKTYPE_TRADE_SETUP);
    fake_ruby_open_link(&b->ruby);
    return run_until_room(b, 400) && fake_ruby_error(&b->ruby) == NULL;
}

/* The GBA player walks out of the room (§B7); the club must go through Closing to Waiting. */
static bool leave_room(Bench* b) {
    fake_ruby_leave_room(&b->ruby);
    return run_until_phase(b, Gen3ClubPhaseClosing, 600) &&
           run_until_phase(b, Gen3ClubPhaseWaiting, 60);
}

TEST(test_club_reaches_cards) {
    Bench b;
    char name[16];
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    fake_ruby_open_link(&b.ruby);
    run(&b, 400);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(at_cards_or_later(st.phase));
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    ASSERT_EQ(st.link.checksum_errors, 0);
    ASSERT_TRUE(st.partner_valid);
    gen3_text_decode(st.partner.name, sizeof(st.partner.name), name, sizeof(name));
    ASSERT_STR_EQ(name, "RUBY");
    ASSERT_EQ(st.partner.trainer_id & 0xFFFF, 12345);
    ASSERT_EQ(st.partner.trainer_id >> 16, 54321);
    ASSERT_EQ(st.partner.link_type, GEN3_LINKTYPE_TRADE_SETUP);

    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    gen3_text_decode(p->name, sizeof(p->name), name, sizeof(name));
    ASSERT_STR_EQ(name, "FLIPPER");
    ASSERT_EQ(p->trainer_id, 22222);
    ASSERT_EQ(p->version, GEN3_VERSION_RUBY);
    ASSERT_EQ(p->link_type, GEN3_LINKTYPE_TRADE_SETUP);
    ASSERT_EQ(p->language, GEN3_LANGUAGE_ENGLISH);
    ASSERT_EQ(p->gender, 0);
    ASSERT_EQ(p->lp_field_2, 0);
    ASSERT_EQ(p->id, 0);
    FakeRubyPhase gp = fake_ruby_phase(&b.ruby);
    ASSERT_TRUE(gp == FakeRubyWalkIn || gp == FakeRubyRoom); /* both cards are in */

    /* §A4: 0x2222 and 0xCCCC exactly once each */
    ASSERT_EQ(b.sent_2222, 1);
    ASSERT_EQ(b.sent_cccc, 1);
}

TEST(test_club_gba_not_ready) {
    Bench b;
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    run(&b, 600);
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyOff);
    fake_ruby_open_link(&b.ruby);
    run(&b, 400);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_TRUE(at_cards_or_later(club_status(&b).phase));
}

TEST(test_club_other_room) {
    Bench b;
    bench_init(&b, 0x2233); /* the GBA player chose the Colosseum */
    fake_ruby_open_link(&b.ruby);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 400));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorOtherRoom);
    ASSERT_TRUE(!st.partner_valid);
    ASSERT_EQ(b.sent_5fff, 0);

    run(&b, 400);
    ASSERT_EQ(b.sent_5fff, 1);
    ASSERT_EQ(b.sent_cccc, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorOtherRoom);
}

TEST(test_club_card_delay_needed) {
    Bench b;
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    gen3_club_set_card_delay(&b.club, 0);
    fake_ruby_open_link(&b.ruby);
    run(&b, 400);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "stale card");
}

TEST(test_club_room_idle) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    run(&b, 3600);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_TRUE(st.room_frames >= 3600);
    ASSERT_EQ(st.link.checksum_errors, 0);
    ASSERT_EQ(b.sent_cafe, 0); /* idle is all-zero commands, not CAFE 0011 (U6) */
}

TEST(test_club_answers_2ffe_once) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    run(&b, 600);
    ASSERT_EQ(b.sent_2ffe, 1);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
}

TEST(test_club_leave_room) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    ASSERT_TRUE(leave_room(&b));
    run(&b, 120);
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_EQ(b.sent_cafe, 1); /* nothing but that answer */
    ASSERT_EQ(b.sent_5fff, 1);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
}

TEST(test_club_second_linkup) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    ASSERT_TRUE(leave_room(&b));
    ruby_relink(&b);
    ASSERT_TRUE(run_until_room(&b, 400));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    ASSERT_TRUE(st.partner_valid);
    ASSERT_EQ(b.sent_2222, 2);
    ASSERT_EQ(b.sent_cccc, 2);
    ASSERT_EQ(b.sent_2ffe, 2);
}

TEST(test_club_gba_reset) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    fake_ruby_power_off(&b.ruby);
    run(&b, 120);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    ASSERT_TRUE(!st.partner_valid);
    ruby_relink(&b);
    ASSERT_TRUE(run_until_room(&b, 400));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_club_checksum_error) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    fake_ruby_corrupt_next(&b.ruby);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 10));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorChecksum);
    ASSERT_EQ(st.link.checksum_errors, 1);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "checksum");
    ASSERT_EQ(gen3_link_queued(&b.club.link), 0);
    fake_ruby_power_off(&b.ruby);
    run(&b, 120);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorChecksum);
    ruby_relink(&b); /* the player restarts the GBA: the same club links again */
    ASSERT_TRUE(run_until_room(&b, 400));
}

TEST(test_club_checksum_error_mid_block) {
    /* Review focus 3: a glitch while our trainer card streams drops the rest of the block. */
    Bench b;
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    fake_ruby_open_link(&b.ruby);
    for(int i = 0; i < 400 && b.sent_cccc == 0; i++) step(&b);
    ASSERT_EQ(b.sent_cccc, 1);
    uint32_t chunks_before = b.sent_8888;
    fake_ruby_corrupt_next(&b.ruby);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 10));
    ASSERT_EQ(club_status(&b).last_error, Gen3ClubErrorChecksum);
    ASSERT_EQ(gen3_link_queued(&b.club.link), 0);
    uint32_t chunks_sent = b.sent_8888 - chunks_before;
    ASSERT_TRUE(chunks_sent < 8); /* the card is 8 chunks: it was cut short */
    run(&b, 30);
    ASSERT_EQ(b.sent_8888 - chunks_before, chunks_sent);
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseWaiting);
}

int main(void) {
    RUN(test_club_reaches_cards);
    RUN(test_club_gba_not_ready);
    RUN(test_club_other_room);
    RUN(test_club_card_delay_needed);
    RUN(test_club_room_idle);
    RUN(test_club_answers_2ffe_once);
    RUN(test_club_leave_room);
    RUN(test_club_second_linkup);
    RUN(test_club_gba_reset);
    RUN(test_club_checksum_error);
    RUN(test_club_checksum_error_mid_block);
    TEST_EXIT();
}
