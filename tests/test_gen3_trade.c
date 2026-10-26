#include "test.h"
#include "bulk_file.h"
#include "club_bench.h"
#include "trade_mons.h"
#include <stdbool.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_text.h>

/* The Flipper (club) and fake Ruby through a Cable Club trade (docs/gen3/link-protocol-trade.md,
 * Part B). Unless a test says otherwise it starts with bench_init_trade: the fake is RUBY
 * 12345/54321, the Flipper FLIPPER 22222 offering tm_make(252, 5, 21). */

static bool run_until_fake_phase(Bench* b, FakeRubyPhase phase, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_phase(&b->ruby) == phase) return true;
        if(fake_ruby_error(&b->ruby) != NULL) return false;
    }
    return false;
}

/* Until the GBA's trade menu has both LinkPlayers of its 0x1122 relink. */
static bool run_until_relinked(Bench* b, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(fake_ruby_phase(&b->ruby) == FakeRubyMenu &&
           fake_ruby_relink_frames(&b->ruby) != UINT32_MAX) {
            return true;
        }
    }
    return false;
}

TEST(test_trade_sit_relinks_menu) {
    Bench b;
    char name[16];
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 200 && b.sent_cafe16 == 0; i++) step(&b);
    ASSERT_EQ(b.sent_cafe16, 1);
    ASSERT_TRUE(b.f_cafe16_burst > b.g_cafe16_burst); /* §B1.4: the GBA sits first */
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(st.flipper_seated);

    /* §B1.5: zero bursts from the Flipper's CAFE 0016 until it answers the GBA's 5FFF */
    for(int i = 0; i < 300 && b.sent_5fff == 0; i++) {
        step(&b);
        if(b.sent_5fff != 0 || b.last_tx.count != MLINK_BURST_LEN) continue;
        for(int w = 1; w < MLINK_BURST_LEN; w++) ASSERT_EQ(b.last_tx.word[w], 0);
    }
    ASSERT_EQ(b.sent_5fff, 1);

    ASSERT_TRUE(run_until_relinked(&b, 300));
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyMenu);
    ASSERT_TRUE(fake_ruby_relink_frames(&b.ruby) < 100);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseExchange);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE_CONNECTING);
    ASSERT_TRUE(st.partner_valid);
    ASSERT_EQ(st.partner.id, 1); /* sub_8007270: the GBA's seat */
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    ASSERT_EQ(b.sent_cafe16, 1);

    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->link_type, GEN3_LINKTYPE_TRADE_CONNECTING);
    ASSERT_EQ(p->trainer_id, 22222);
    gen3_text_decode(p->name, sizeof(p->name), name, sizeof(name));
    ASSERT_STR_EQ(name, "FLIPPER");
    ASSERT_EQ(p->lp_field_2, 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_waits_for_gba) {
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    run(&b, 1200);
    ASSERT_EQ(b.sent_cafe16, 0); /* the Flipper never sits first */
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_link_test_never_sits) {
    Bench b;
    uint8_t mon[GEN3_PARTY_SIZE];
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    tm_make(252, 5, 21, mon);
    ASSERT_TRUE(bench_offer(&b, mon));
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 600);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(!st.flipper_seated);
}

TEST(test_trade_no_offer_never_sits) {
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
    ASSERT_TRUE(!club_status(&b).has_offer);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 600);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(!st.flipper_seated);
}

TEST(test_trade_stand_up) {
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 60);
    ASSERT_TRUE(club_status(&b).gba_seated);
    ASSERT_EQ(fake_ruby_link_state(&b.ruby, 1), 0x82); /* READY */
    fake_ruby_stand_up(&b.ruby);
    run(&b, 60);
    ASSERT_TRUE((b.g_cafe_seen & (1u << 0x1D)) != 0);
    ASSERT_TRUE((b.g_cafe_seen & (1u << 0x1A)) != 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
    ASSERT_EQ(fake_ruby_link_state(&b.ruby, 1), 0x80); /* IDLE */
    ASSERT_EQ(fake_ruby_link_state(&b.ruby, 0), 0x80);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(!st.gba_seated);
    ASSERT_TRUE(!st.flipper_seated);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_late_sit) {
    /* R5: the GBA sits while the Flipper has no offer; the Flipper sits once it has one. */
    Bench b;
    uint8_t mon[GEN3_PARTY_SIZE];
    bench_init_trade(&b);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    run(&b, 300);
    ASSERT_EQ(b.sent_cafe16, 0);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(!st.flipper_seated);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    tm_make(252, 5, 21, mon);
    ASSERT_TRUE(bench_offer(&b, mon));
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 200));
    ASSERT_TRUE(run_until_relinked(&b, 200));
    ASSERT_EQ(b.sent_cafe16, 1);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseExchange);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE_CONNECTING);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_relink_timeout_trips_fake) {
    /* §B8: the Flipper never relinks; the GBA's 300-frame task (sub_8083C50) gives up. */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 400)); /* this step's OpenLink */
    bench_mute(&b, true);
    uint32_t frames = 0;
    while(fake_ruby_error(&b.ruby) == NULL && frames < 400) {
        step(&b);
        frames++;
    }
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "link timeout");
    ASSERT_TRUE(frames >= 300 && frames <= 310);
}

TEST(test_trade_link_type_mismatch_trips_fake) {
    /* §B1.6: a Flipper block whose type isn't the GBA's 0x1122 (Ruby hangs silently). */
    Bench b;
    Gen3LinkPlayer lp;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 400));
    gen3_club_claim_game(&club_cfg_trade, &b.me, GEN3_LINKTYPE_TRADE_SETUP, false, &lp, card);
    bench_raw_relink(&b, &lp, 200);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "link type");
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_trade_sit_relinks_menu);
    RUN(test_trade_waits_for_gba);
    RUN(test_trade_link_test_never_sits);
    RUN(test_trade_no_offer_never_sits);
    RUN(test_trade_stand_up);
    RUN(test_trade_late_sit);
    RUN(test_trade_relink_timeout_trips_fake);
    RUN(test_trade_link_type_mismatch_trips_fake);
    TEST_EXIT();
}
