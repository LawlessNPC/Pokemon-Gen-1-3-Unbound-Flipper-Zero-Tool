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

/* bench_init_trade with set_round_delays(first, later); to the room and the GBA sits. When the
 * Flipper's 0x1122 LinkPlayerBlock completes (its first block of size 60 in the GBA's trade
 * menu), the GBA's main loop stalls `stall` frames: round 1's margin is all that absorbs it.
 * Then until Browsing (true) or a fake error (false). */
static bool exchange_with_round1_stall(Bench* b, uint8_t first, uint8_t later, uint8_t stall) {
    bench_init_trade(b);
    gen3_club_set_round_delays(&b->club, first, later);
    if(!bench_to_room(b)) return false;
    fake_ruby_sit(&b->ruby, 1);
    bool stalled = false;
    for(int i = 0; i < 900; i++) {
        const uint8_t n = b->n_f_blocks;
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(fake_ruby_menu_state(&b->ruby) == FakeRubyMenuBrowsing) return true;
        if(!stalled && b->n_f_blocks > n && fake_ruby_phase(&b->ruby) == FakeRubyMenu &&
           b->f_blocks[b->n_f_blocks - 1].size == GEN3_LINK_PLAYER_BLOCK_SIZE) {
            fake_ruby_stall(&b->ruby, stall);
            stalled = true;
        }
    }
    return false;
}

TEST(test_trade_round1_margin) {
    /* §B2.2: the GBA must take round 1's CCCC at least 3 frames after both LinkPlayerBlocks;
     * the club's 10 idle bursts leave room for an 8-frame stall at the worst moment. */
    Bench b;
    ASSERT_TRUE(exchange_with_round1_stall(&b, GEN3_CLUB_ROUND1_DELAY_BURSTS, 6, 8));
    ASSERT_TRUE(!exchange_with_round1_stall(&b, GEN3_CLUB_ROUND1_DELAY_BURSTS, 6, 9));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "stale block");
}

TEST(test_trade_later_rounds_need_no_delay) {
    /* The rounds 2-5 delay is padding (§B2.2, inferred): each CCCC queues on the GBA behind both
     * blocks of the previous round, so even no idle burst works, and the fake invents no
     * hazard. */
    Bench b;
    ASSERT_TRUE(exchange_with_round1_stall(&b, GEN3_CLUB_ROUND1_DELAY_BURSTS, 0, 0));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseMenu);
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
    /* 600 frames into case 3, which began 8 frames before the GBA got round 1's CCCC (G0 + 3
     * against G0 + 11) */
    ASSERT_EQ(frames, 592);
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

/* bench_init_trade offering tm_make_pkm(252, 5, 21) with `field` poked raw to `value` (checksum
 * fixed, past every check), then bench_reach_menu: the fake's error, or NULL. */
static const char* offer_poked(Bench* b, Gen3Field field, uint32_t value) {
    Gen3Pkm p;
    uint8_t mon[GEN3_PARTY_SIZE];
    tm_make_pkm(252, 5, 21, &p);
    gen3_pkm_set(&p, field, value);
    gen3_pkm_fix_checksum(&p);
    gen3_pkm_encrypt(&p, mon);
    bench_init_trade(b);
    if(!bench_offer(b, mon)) return "offer refused";
    bench_reach_menu(b);
    return fake_ruby_error(&b->ruby);
}

TEST(test_trade_bad_move_trips_fake) {
    /* §B2.5 rule 2: moves 0..354 (NUM_MOVES = 355) */
    Bench b;
    const char* error = offer_poked(&b, Gen3FieldMove2, 355);
    ASSERT_TRUE(error != NULL);
    ASSERT_STR_EQ(error, "bad offer");
}

TEST(test_trade_mail_byte_trips_fake) {
    /* §B2.5 rule 4: no mail data goes with the Flipper's Pokemon, so its mail byte stays 0xFF */
    Bench b;
    const char* error = offer_poked(&b, Gen3FieldMail, 0);
    ASSERT_TRUE(error != NULL);
    ASSERT_STR_EQ(error, "bad offer");
}

TEST(test_trade_mail_item_trips_fake) {
    /* §B2.5 rule 4: a mail item (121..132) without mail data */
    Bench b;
    const char* error = offer_poked(&b, Gen3FieldHeldItem, 132);
    ASSERT_TRUE(error != NULL);
    ASSERT_STR_EQ(error, "bad offer");
}

TEST(test_trade_bad_request_trips_fake) {
    /* sBlockRequestLookupTable has 5 entries; Ruby would read past it for type 5. */
    static const uint16_t request[GEN3_CMD_LEN] = {0xCCCC, 5};
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(gen3_link_send(&b.club.link, request)); /* raw, past the club */
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "bad request");
}

TEST(test_trade_seat_step_while_busy) {
    /* The fake's avatar: a step onto a seat while this GBA's player isn't IDLE (here BUSY in the
     * exit prompt) runs no seat script, but the avatar still ends on the seat tile. */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_TRUE(!fake_ruby_on_seat(&b.ruby));
    fake_ruby_leave_room(&b.ruby);
    for(int i = 0; i < 60 && fake_ruby_link_state(&b.ruby, 1) != 0x81; i++) step(&b);
    ASSERT_EQ(fake_ruby_link_state(&b.ruby, 1), 0x81); /* BUSY */
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 10);
    ASSERT_TRUE(fake_ruby_on_seat(&b.ruby));
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyLeaving); /* no seat script */
    ASSERT_EQ(fake_ruby_link_state(&b.ruby, 1), 0x81);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
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

/* ---- Trade menu (§B3) ---- */

/* The k-th 20-byte (§B3.1) block among blocks[0..n), or NULL. */
static const BenchBlock* trade_block(const BenchBlock* blocks, uint8_t n, size_t k) {
    for(uint8_t i = 0; i < n; i++) {
        if(blocks[i].size != GEN3_TRADE_BLOCK_SIZE) continue;
        if(k-- == 0) return &blocks[i];
    }
    return NULL;
}

static size_t n_trade_blocks(const BenchBlock* blocks, uint8_t n) {
    uint16_t codes[BENCH_BLOCKS];
    return bench_codes(blocks, n, codes, BENCH_BLOCKS);
}

/* blk is a 20-byte block {code, arg}; else prints what it is. */
static bool trade_block_is(const BenchBlock* blk, uint16_t code, uint16_t arg) {
    if(!blk) {
        printf("  no such block\n");
        return false;
    }
    if(blk->code != code || blk->arg != arg) {
        printf("  block {%04X %u}, want {%04X %u}\n", blk->code, blk->arg, code, arg);
        return false;
    }
    return true;
}

/* The Flipper's reply went out after GEN3_CLUB_REPLY_DELAY_BURSTS idle bursts or more, counted
 * from the burst of the last chunk of the GBA's block (§B3.4). */
static bool replied_after(const BenchBlock* gba, const BenchBlock* flipper) {
    return gba && flipper &&
           flipper->first_burst >= gba->last_burst + GEN3_CLUB_REPLY_DELAY_BURSTS + 1;
}

static bool run_until_menu_state(Bench* b, FakeRubyMenuState state, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(fake_ruby_menu_state(&b->ruby) == state) return true;
    }
    return false;
}

TEST(test_trade_offer_confirm_ccdd) {
    /* §B3.5: AABB 2 -> DDDD 0 -> BBBB -> CCDD, each reply 6 idle bursts after the GBA's block */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuConfirming, 60));
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 1);
    const BenchBlock* aabb = trade_block(b.g_blocks, b.n_g_blocks, 0);
    ASSERT_TRUE(trade_block_is(aabb, GEN3_TRADE_READY, 2));
    ASSERT_EQ(aabb->id, 0x81);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* dddd = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(dddd, GEN3_TRADE_SET_MONS, 0));
    ASSERT_EQ(dddd->id, 0x80);
    ASSERT_TRUE(replied_after(aabb, dddd));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.menu, Gen3ClubMenuOffered);
    ASSERT_EQ(st.gba_slot, 2);
    ASSERT_EQ(st.refused, Gen3RefuseNone);
    ASSERT_TRUE(!st.trade_pending);

    /* 20 frames of slide-in, then "Is this trade okay?" after 120 more */
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 200));
    fake_ruby_answer(&b.ruby, true);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuClosing, 60));
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 2);
    const BenchBlock* bbbb = trade_block(b.g_blocks, b.n_g_blocks, 1);
    ASSERT_TRUE(trade_block_is(bbbb, GEN3_TRADE_CONFIRM, 2)); /* §B3.1: arg is stale (AABB's) */
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    const BenchBlock* ccdd = trade_block(b.f_blocks, b.n_f_blocks, 1);
    ASSERT_TRUE(trade_block_is(ccdd, GEN3_TRADE_START, 0));
    ASSERT_TRUE(replied_after(bbbb, ccdd));
    st = club_status(&b);
    ASSERT_EQ(st.menu, Gen3ClubMenuStarting);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_EQ(st.gba_slot, 2);

    /* The GBA fades out and closes the link for the trade (Task 6 goes on from there) */
    const uint32_t answers = b.sent_5fff;
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyTrade, 120));
    ASSERT_EQ(b.sent_5fff, answers + 1);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 60));
    ASSERT_TRUE(club_status(&b).trade_pending); /* trade context (D3) */
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_gba_says_no) {
    /* §B3.4: BBCC gets DDEE; the GBA shows "The trade has been canceled." until A */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, false);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuMessage, 60));
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 2);
    const BenchBlock* bbcc = trade_block(b.g_blocks, b.n_g_blocks, 1);
    ASSERT_TRUE(trade_block_is(bbcc, GEN3_TRADE_CONFIRM_NO, 2));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    ASSERT_TRUE(trade_block_is(trade_block(b.f_blocks, b.n_f_blocks, 0), GEN3_TRADE_SET_MONS, 0));
    const BenchBlock* ddee = trade_block(b.f_blocks, b.n_f_blocks, 1);
    ASSERT_TRUE(trade_block_is(ddee, GEN3_TRADE_PLAYER_CANCEL, 0));
    ASSERT_TRUE(replied_after(bbcc, ddee));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.menu, Gen3ClubMenuIdle);
    ASSERT_TRUE(!st.trade_pending);

    fake_ruby_press_a(&b.ruby);
    ASSERT_TRUE(run_until_browsing(&b, 5));
    run(&b, 300);
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuBrowsing);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_gba_cancels) {
    /* §B3.4, §B6.3: EEAA gets EEBB; the GBA fades out, closes the link and heads for the room */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_cancel_trade(&b.ruby);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuClosing, 60));
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 1);
    const BenchBlock* eeaa = trade_block(b.g_blocks, b.n_g_blocks, 0);
    ASSERT_TRUE(trade_block_is(eeaa, GEN3_TRADE_CANCEL, 0));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* eebb = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(eebb, GEN3_TRADE_BOTH_CANCEL, 0));
    ASSERT_TRUE(replied_after(eeaa, eebb));
    ASSERT_EQ(club_status(&b).menu, Gen3ClubMenuLeaving);

    const uint32_t answers = b.sent_5fff;
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
    ASSERT_EQ(b.sent_5fff, answers + 1); /* the Flipper answered the GBA's 5FFF */
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseClosing);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 60));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1); /* no CCDD */
    ASSERT_TRUE(!club_status(&b).trade_pending);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

/* bench_init_trade with the GBA party {tm_make(1, 5, 11), slot1, tm_make(7, 10, 13)}, then
 * bench_reach_menu. */
static bool reach_menu_with_slot1(Bench* b, const uint8_t slot1[GEN3_PARTY_SIZE]) {
    uint8_t party[3][GEN3_PARTY_SIZE];
    tm_make(1, 5, 11, party[0]);
    memcpy(party[1], slot1, GEN3_PARTY_SIZE);
    tm_make(7, 10, 13, party[2]);
    bench_init_trade(b);
    bench_set_party(b, party, 3);
    return bench_reach_menu(b);
}

/* The GBA offers its slot 1, which the Flipper refuses: DDEE, never DDDD; the GBA shows the
 * message. The reason, or -1 on another outcome. */
static int refused_slot1(Bench* b, const uint8_t slot1[GEN3_PARTY_SIZE]) {
    if(!reach_menu_with_slot1(b, slot1)) return -1;
    fake_ruby_offer(&b->ruby, 1);
    if(!run_until_menu_state(b, FakeRubyMenuMessage, 60)) return -1;
    const BenchBlock* aabb = trade_block(b->g_blocks, b->n_g_blocks, 0);
    const BenchBlock* ddee = trade_block(b->f_blocks, b->n_f_blocks, 0);
    if(n_trade_blocks(b->g_blocks, b->n_g_blocks) != 1 ||
       n_trade_blocks(b->f_blocks, b->n_f_blocks) != 1 ||
       !trade_block_is(aabb, GEN3_TRADE_READY, 1) ||
       !trade_block_is(ddee, GEN3_TRADE_PLAYER_CANCEL, 0) || !replied_after(aabb, ddee)) {
        return -1;
    }
    Gen3ClubStatus st = club_status(b);
    if(st.menu != Gen3ClubMenuIdle || st.trade_pending) return -1;
    return (int)st.refused;
}

TEST(test_trade_refuses_egg) {
    /* D8: the Flipper takes no Egg; the GBA may then pick another Pokemon */
    Bench b;
    uint8_t egg[GEN3_PARTY_SIZE];
    tm_make_egg(egg);
    ASSERT_EQ(refused_slot1(&b, egg), Gen3RefuseEgg);
    fake_ruby_press_a(&b.ruby);
    ASSERT_TRUE(run_until_browsing(&b, 5));
    fake_ruby_offer(&b.ruby, 0);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuConfirming, 60));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    ASSERT_TRUE(trade_block_is(trade_block(b.f_blocks, b.n_f_blocks, 1), GEN3_TRADE_SET_MONS, 0));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.refused, Gen3RefuseNone);
    ASSERT_EQ(st.gba_slot, 0);
    ASSERT_EQ(st.menu, Gen3ClubMenuOffered);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_refuses_mail) {
    /* D8: nor a Mail holder (the Flipper would have to keep its mail) */
    Bench b;
    uint8_t mail[GEN3_PARTY_SIZE];
    tm_make_mail(mail);
    ASSERT_EQ(refused_slot1(&b, mail), Gen3RefuseMail);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_refuses_bad_egg) {
    /* A checksum mismatch: Ruby shows it as a Bad Egg */
    Bench b;
    uint8_t bad[GEN3_PARTY_SIZE];
    tm_make_bad_egg(bad);
    ASSERT_EQ(refused_slot1(&b, bad), Gen3RefuseBadEgg);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_only_pokemon_stays) {
    /* §B3.2: TRADE on the GBA's last battle-able Pokemon sends nothing ("That's your only
     * POKéMON for battle."); its party here is that one Pokemon */
    Bench b;
    uint8_t party[1][GEN3_PARTY_SIZE];
    tm_make(1, 5, 11, party[0]);
    bench_init_trade(&b);
    bench_set_party(&b, party, 1);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 0);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuMessage, 5));
    run(&b, 60);
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 0);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

/* bench_init_trade with the offer withdrawn before round 1 (R8): the GBA's menu shows no
 * Flipper Pokemon. Until Browsing. */
static bool reach_menu_empty_offer(Bench* b) {
    bench_init_trade(b);
    if(!bench_to_room(b)) return false;
    fake_ruby_sit(&b->ruby, 1);
    if(!run_until_fake_phase(b, FakeRubyMenu, 400) || club_status(b).round != 0) return false;
    if(!gen3_club_set_offer(&b->club, NULL, 0)) return false;
    return run_until_browsing(b, 600) && fake_ruby_enemy_count(&b->ruby) == 0;
}

TEST(test_trade_no_dddd_to_empty_party) {
    /* R8: the GBA shows no Flipper Pokemon, so a DDDD would point at a partner icon Ruby never
     * made. Its AABB gets DDEE. */
    Bench b;
    ASSERT_TRUE(reach_menu_empty_offer(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuMessage, 60));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    ASSERT_TRUE(
        trade_block_is(trade_block(b.f_blocks, b.n_f_blocks, 0), GEN3_TRADE_PLAYER_CANCEL, 0));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.menu, Gen3ClubMenuIdle);
    ASSERT_EQ(st.refused, Gen3RefuseNone); /* the GBA's Pokemon is fine; the Flipper has none */
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_dddd_to_empty_party_trips_fake) {
    /* R8: a DDDD to an empty partner column, as soon as the GBA's AABB is complete */
    Bench b;
    ASSERT_TRUE(reach_menu_empty_offer(&b));
    fake_ruby_offer(&b.ruby, 2);
    for(int i = 0; i < 60 && n_trade_blocks(b.g_blocks, b.n_g_blocks) == 0; i++) step(&b);
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 1);
    ASSERT_TRUE(bench_inject_code(&b, GEN3_TRADE_SET_MONS, 0));
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "empty party");
}

TEST(test_trade_ccdd_rule_trips_fake) {
    /* §B3.4: Ruby starts the trade on any CCDD, with whatever its cursor holds */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_inject_code(&b, GEN3_TRADE_START, 0));
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "bad ccdd");
}

TEST(test_trade_unasked_dddd_trips_fake) {
    /* §B3.4: DDDD only in reply to AABB */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_inject_code(&b, GEN3_TRADE_SET_MONS, 0));
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "unexpected dddd");
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
    RUN(test_trade_round1_margin);
    RUN(test_trade_later_rounds_need_no_delay);
    RUN(test_trade_missing_block_stalls_fake);
    RUN(test_trade_exchange_with_backlog);
    RUN(test_trade_exchange_empty_offer);
    RUN(test_trade_bad_offer_trips_fake);
    RUN(test_trade_bad_item_trips_fake);
    RUN(test_trade_bad_move_trips_fake);
    RUN(test_trade_mail_byte_trips_fake);
    RUN(test_trade_mail_item_trips_fake);
    RUN(test_trade_bad_request_trips_fake);
    RUN(test_trade_seat_step_while_busy);
    RUN(test_trade_extra_request_trips_fake);
    RUN(test_trade_offer_confirm_ccdd);
    RUN(test_trade_gba_says_no);
    RUN(test_trade_gba_cancels);
    RUN(test_trade_refuses_egg);
    RUN(test_trade_refuses_mail);
    RUN(test_trade_refuses_bad_egg);
    RUN(test_trade_only_pokemon_stays);
    RUN(test_trade_no_dddd_to_empty_party);
    RUN(test_trade_dddd_to_empty_party_trips_fake);
    RUN(test_trade_ccdd_rule_trips_fake);
    RUN(test_trade_unasked_dddd_trips_fake);
    RUN(test_club_size);
    TEST_EXIT();
}
