#include "test.h"
#include "bulk_file.h"
#include "club_bench.h"
#include "gen3_golden.h"
#include "trade_mons.h"
#include <stdbool.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_text.h>

/* The Flipper (club) and fake Ruby through a Cable Club trade (docs/gen3/link-protocol-trade.md,
 * Part B). Unless a test says otherwise it starts with bench_init_trade: the fake is RUBY
 * 12345/54321 holding bench_init's three Pokemon, the Flipper FLIPPER 22222 offering
 * tm_make(252, 5, 21). */

static const uint8_t zero_mon[GEN3_PARTY_SIZE];

/* Index of the latest block of `size` among blocks[0..n), or -1. */
static int latest_block(const BenchBlock* blocks, uint8_t n, uint16_t size) {
    for(int i = n - 1; i >= 0; i--) {
        if(blocks[i].size == size) return i;
    }
    return -1;
}

static uint32_t later_burst(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

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
    /* §B1.4: the GBA sits first; the Flipper answers in the very next burst */
    ASSERT_EQ(b.f_cafe16_burst, b.g_cafe16_burst + 1);
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
    ASSERT_TRUE(!st.gba_seated); /* a new room session's seats */
    ASSERT_TRUE(!st.flipper_seated);
    /* D2: the Flipper's 0x1122 LinkPlayerBlock starts after the GBA's is complete */
    int fk = latest_block(b.f_blocks, b.n_f_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    int gk = latest_block(b.g_blocks, b.n_g_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    ASSERT_TRUE(fk >= 0 && gk >= 0);
    ASSERT_TRUE(b.f_blocks[fk].first_burst > b.g_blocks[gk].last_burst);

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
    const uint32_t seen = b.g_cafe_seen; /* so the bits below come from the stand-up */
    ASSERT_EQ(seen & ((1u << 0x1D) | (1u << 0x1A)), 0);
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
    ASSERT_EQ(frames, 300);
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

/* Until the GBA's trade menu is Browsing, or a fake error; false if not within max_frames. */
static bool run_until_browsing(Bench* b, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(fake_ruby_menu_state(&b->ruby) == FakeRubyMenuBrowsing) return true;
    }
    return false;
}

TEST(test_trade_exchange_parties) {
    /* §B2.1: five rounds, then the GBA builds its trade menu with the Flipper's one Pokemon. */
    static const uint16_t types[GEN3_TRADE_ROUNDS] = {1, 1, 1, 3, 4};
    static const uint16_t sizes[GEN3_TRADE_ROUNDS] = {200, 200, 200, 220, 40};
    Bench b;
    uint8_t offer[GEN3_PARTY_SIZE];
    uint8_t mon[GEN3_PARTY_SIZE];
    bench_init_trade(&b);
    tm_make(252, 5, 21, offer); /* bench_init_trade's offer */
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 400)); /* this step's 0x1122 OpenLink */
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuLoading);
    ASSERT_TRUE(run_until_browsing(&b, 300));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    /* The GBA's side: CalculateEnemyPartyCount */
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 1);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), offer, GEN3_PARTY_SIZE);
    for(uint8_t i = 1; i < 6; i++) {
        ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, i), zero_mon, GEN3_PARTY_SIZE);
    }

    /* The Flipper's side: the GBA's party */
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseMenu);
    ASSERT_EQ(st.round, 5);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    ASSERT_EQ(b.party_count, 3);
    for(uint8_t i = 0; i < 6; i++) {
        ASSERT_MEM_EQ(fake_ruby_party(&b.ruby, i, false), b.party[i], GEN3_PARTY_SIZE);
        ASSERT_MEM_EQ(fake_ruby_party(&b.ruby, i, true), b.party[i], GEN3_PARTY_SIZE);
        gen3_club_gba_mon(&b.club, i, mon);
        ASSERT_MEM_EQ(mon, i < 3 ? b.party[i] : zero_mon, GEN3_PARTY_SIZE);
    }

    /* On the wire: one CCCC per round, and each side's round blocks after its 0x1122
     * LinkPlayerBlock */
    ASSERT_EQ(b.n_f_cccc, GEN3_TRADE_ROUNDS);
    for(int r = 0; r < GEN3_TRADE_ROUNDS; r++) ASSERT_EQ(b.f_cccc[r], types[r]);
    int fk = latest_block(b.f_blocks, b.n_f_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    int gk = latest_block(b.g_blocks, b.n_g_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    ASSERT_TRUE(fk >= 0 && gk >= 0);
    ASSERT_EQ(b.n_f_blocks, fk + 1 + GEN3_TRADE_ROUNDS);
    ASSERT_EQ(b.n_g_blocks, gk + 1 + GEN3_TRADE_ROUNDS);
    for(int r = 0; r < GEN3_TRADE_ROUNDS; r++) {
        ASSERT_EQ(b.f_blocks[fk + 1 + r].size, sizes[r]); /* {BBBB size 0080} */
        ASSERT_EQ(b.f_blocks[fk + 1 + r].id, 0x80);
        ASSERT_EQ(b.g_blocks[gk + 1 + r].size, sizes[r]);
        ASSERT_EQ(b.g_blocks[gk + 1 + r].id, 0x81);
    }
}

TEST(test_trade_exchange_margins) {
    /* §B2.2: >= 8 idle bursts before round 1's CCCC, >= 5 before each later one (both blocks
     * of the previous step complete); the club leaves 10 and 6. */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    int fk = latest_block(b.f_blocks, b.n_f_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    int gk = latest_block(b.g_blocks, b.n_g_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    ASSERT_TRUE(fk >= 0 && gk >= 0);
    ASSERT_EQ(b.n_f_blocks, fk + 1 + GEN3_TRADE_ROUNDS);
    ASSERT_EQ(b.n_g_blocks, gk + 1 + GEN3_TRADE_ROUNDS);
    ASSERT_EQ(b.n_f_cccc, GEN3_TRADE_ROUNDS);
    /* f_cccc_burst[r] follows the LinkPlayerBlocks (r = 0) or round r's blocks */
    for(int r = 0; r < GEN3_TRADE_ROUNDS; r++) {
        uint32_t done = later_burst(b.f_blocks[fk + r].last_burst, b.g_blocks[gk + r].last_burst);
        int64_t gap = (int64_t)b.f_cccc_burst[r] - (int64_t)done;
        ASSERT_TRUE(gap >= (r == 0 ? 11 : 7));
    }
}

TEST(test_trade_round1_delay_needed) {
    /* §B2.2: round 1's CCCC right after the LinkPlayerBlocks reaches the GBA before its case 1,
     * which then waits for its own block and clears the Flipper's round-1 flag. Its case 3 is
     * left one block behind: a Ruby master would stop there (the black screen), but the club
     * goes on to round 2, which the GBA answers with its round-1 copy. The fake stops at that
     * first wrong block, before its 600-frame stall check. */
    Bench b;
    bench_init_trade(&b);
    gen3_club_set_round_delays(&b.club, 0, GEN3_CLUB_ROUND_DELAY_BURSTS);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 900 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "stale block");
    ASSERT_EQ(club_status(&b).round, 2);
}

TEST(test_trade_missing_block_stalls_fake) {
    /* §B2.1: no timeout in any case. The Flipper's round-1 block never comes (its queue is
     * dropped and only its link layer runs on, sending idle bursts): case 3 waits forever. */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 400 && club_status(&b).round == 0; i++) step(&b);
    ASSERT_EQ(club_status(&b).round, 1); /* round 1's CCCC is on the wire, its block queued */
    gen3_link_flush(&b.club.link);
    uint32_t frames = 0;
    while(fake_ruby_error(&b.ruby) == NULL && frames < 700) {
        MlinkTx tx;
        uint16_t cmd[GEN3_CMD_LEN];
        gen3_link_rx(&b.club.link, &b.rx, cmd);
        gen3_link_tx(&b.club.link, &tx);
        fake_ruby_frame(&b.ruby, &tx, &b.rx);
        frames++;
    }
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "exchange stalled");
    /* 600 frames into case 3, which began a few frames before round 1's CCCC */
    ASSERT_TRUE(frames > 580 && frames <= 600);
}

TEST(test_trade_exchange_with_backlog) {
    /* Review Focus 4: the GBA's main loop stalls 4 frames (a map load, a flash write) as each
     * round starts, so its receive queue backs up; the margins absorb it. */
    Bench b;
    uint8_t offer[GEN3_PARTY_SIZE];
    uint8_t round = 0;
    int stalls = 0;
    bench_init_trade(&b);
    tm_make(252, 5, 21, offer);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 900; i++) {
        step(&b);
        if(fake_ruby_error(&b.ruby) != NULL) break;
        if(fake_ruby_menu_state(&b.ruby) == FakeRubyMenuBrowsing) break;
        const uint8_t now = club_status(&b).round;
        if(now != round) {
            fake_ruby_stall(&b.ruby, 4);
            round = now;
            stalls++;
        }
    }
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuBrowsing);
    ASSERT_EQ(stalls, GEN3_TRADE_ROUNDS);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), offer, GEN3_PARTY_SIZE);
}

TEST(test_trade_exchange_empty_offer) {
    /* R8: with no offer the Flipper sends an empty party; Ruby opens its menu with an empty
     * partner column. */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 400));
    ASSERT_EQ(club_status(&b).round, 0); /* before round 1 */
    ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
    ASSERT_TRUE(run_until_browsing(&b, 600));
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 0);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), zero_mon, GEN3_PARTY_SIZE);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseMenu);
    ASSERT_EQ(st.round, 5);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_bad_offer_trips_fake) {
    /* G6: the club trusts its offer; a bad checksum reaches Ruby as a Bad Egg. */
    Bench b;
    uint8_t bad[GEN3_PARTY_SIZE];
    golden_game(bad);
    bad[0x30] ^= 1;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_offer(&b, bad));
    ASSERT_TRUE(!bench_reach_menu(&b));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "bad egg");
}

TEST(test_trade_bad_item_trips_fake) {
    /* §B2.5 rule 3: the ENIGMA BERRY, poked raw past every check. */
    Bench b;
    Gen3Pkm p;
    uint8_t mon[GEN3_PARTY_SIZE];
    tm_make_pkm(252, 5, 21, &p);
    gen3_pkm_set(&p, Gen3FieldHeldItem, GEN3_ITEM_ENIGMA_BERRY);
    gen3_pkm_encrypt(&p, mon);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_offer(&b, mon));
    ASSERT_TRUE(!bench_reach_menu(&b));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "bad offer");
}

TEST(test_trade_extra_request_trips_fake) {
    /* U3: a sixth CCCC after the exchange would get gBlockSendBuffer's round-5 bytes. */
    static const uint16_t request[GEN3_CMD_LEN] = {0xCCCC, 1};
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(gen3_link_send(&b.club.link, request)); /* raw, past the club */
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "stale block");
}

TEST(test_club_size) {
    ASSERT_TRUE(sizeof(Gen3Club) <= 2600);
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
    RUN(test_trade_exchange_parties);
    RUN(test_trade_exchange_margins);
    RUN(test_trade_round1_delay_needed);
    RUN(test_trade_missing_block_stalls_fake);
    RUN(test_trade_exchange_with_backlog);
    RUN(test_trade_exchange_empty_offer);
    RUN(test_trade_bad_offer_trips_fake);
    RUN(test_trade_bad_item_trips_fake);
    RUN(test_trade_extra_request_trips_fake);
    RUN(test_club_size);
    TEST_EXIT();
}
