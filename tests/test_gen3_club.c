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
    MlinkRx rx;
    bool started;
    uint32_t sent_2222, sent_cccc, sent_5fff; /* Flipper commands by word 0 */
} Bench;

static void bench_init(Bench* b, uint32_t ruby_link_type) {
    Gen3LinkPlayer me;
    memset(b, 0, sizeof(*b));
    memset(&me, 0, sizeof(me));
    me.version = GEN3_VERSION_RUBY;
    me.trainer_id = (54321u << 16) | 12345u;
    gen3_text_encode("RUBY", me.name, sizeof(me.name), 7);
    me.gender = 0;
    me.link_type = ruby_link_type;
    me.language = GEN3_LANGUAGE_ENGLISH;
    gen3_club_init(&b->club, &club_cfg);
    fake_ruby_init(&b->ruby, &me);
}

static void count_sent(Bench* b, const MlinkTx* tx) {
    if(tx->count != MLINK_BURST_LEN) return;
    if(tx->word[1] == 0x2222) b->sent_2222++;
    if(tx->word[1] == 0xCCCC) b->sent_cccc++;
    if(tx->word[1] == 0x5FFF) b->sent_5fff++;
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

int main(void) {
    RUN(test_club_reaches_cards);
    RUN(test_club_gba_not_ready);
    RUN(test_club_other_room);
    RUN(test_club_card_delay_needed);
    TEST_EXIT();
}
