#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fake_ruby.h"
#include "trade_mons.h"
#include <src/gen3/gen3_block.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_link.h>
#include <src/gen3/gen3_text.h>

/* Test bench shared by the club and trade tests: the Flipper (club) and fake Ruby wired back to
 * back. Every helper is static inline, so a test file that doesn't use one compiles cleanly. */

static const Gen3ClubConfig club_cfg =
    {.name = "FLIPPER", .tid = 22222, .sid = 0, .gender = 0, .trading = false};
static const Gen3ClubConfig club_cfg_trade =
    {.name = "FLIPPER", .tid = 22222, .sid = 0, .gender = 0, .trading = true};

#define BENCH_BLOCKS 96 /* f_blocks, g_blocks */
#define BENCH_CCCC   32 /* f_cccc, f_cccc_burst */

/* One completed block as one side put it on the wire. */
typedef struct {
    uint16_t size, code, arg; /* code, arg: the block's words 0 and 1 */
    uint32_t first_burst, last_burst; /* the header's burst, the last chunk's */
} BenchBlock;

/* Flipper (club) and fake Ruby wired back to back. rx is the fake's answer to the last tx.
 * Bursts count from 1. Every log caps its writes at its size: later entries are dropped. */
typedef struct {
    Gen3Club club;
    FakeRuby ruby;
    Gen3LinkPlayer me; /* the fake's LinkPlayer, kept for fake_ruby_init after a reset */
    MlinkRx rx;
    bool started;
    /* Flipper commands by word 0; sent_cafe17 counts CAFE 0017 */
    uint32_t sent_2222, sent_cccc, sent_5fff, sent_2ffe, sent_8888, sent_cafe, sent_cafe17;

    uint32_t bursts; /* frames with tx.count == 9 so far */
    MlinkTx last_tx; /* the Flipper's tx of the latest step */
    Gen3BlockRx f_rx, g_rx; /* the bench's own parse of each side's blocks (tx / rx words 1..8) */
    uint32_t f_rx_first, g_rx_first; /* burst of the header f_rx / g_rx is parsing */
    /* every completed block of each side, in order */
    BenchBlock f_blocks[BENCH_BLOCKS], g_blocks[BENCH_BLOCKS];
    uint8_t n_f_blocks, n_g_blocks;
    /* The Flipper's CCCC types and bursts since the GBA's latest LinkPlayerBlock completed */
    uint16_t f_cccc[BENCH_CCCC];
    uint32_t f_cccc_burst[BENCH_CCCC];
    uint8_t n_f_cccc;
    uint32_t g_cafe_seen; /* bit k: the GBA sent CAFE k (k < 32) */
    uint32_t sent_cafe16; /* Flipper CAFE 0016 count */
    uint32_t f_cafe16_burst, g_cafe16_burst; /* latest CAFE 0016 burst per side */
    bool muted; /* bench_mute */
} Bench;

static inline void bench_init_cfg(Bench* b, uint32_t ruby_link_type, const Gen3ClubConfig* cfg) {
    memset(b, 0, sizeof(*b));
    b->me.version = GEN3_VERSION_RUBY;
    b->me.trainer_id = (54321u << 16) | 12345u;
    gen3_text_encode("RUBY", b->me.name, sizeof(b->me.name), 7);
    b->me.gender = 0;
    b->me.link_type = ruby_link_type;
    b->me.language = GEN3_LANGUAGE_ENGLISH;
    gen3_club_init(&b->club, cfg);
    fake_ruby_init(&b->ruby, &b->me);
}

/* A Link Test session (trading false). */
static inline void bench_init(Bench* b, uint32_t ruby_link_type) {
    bench_init_cfg(b, ruby_link_type, &club_cfg);
}

/* The Flipper offers mon (game form), as the app would: gen3_club_set_offer with the club's
 * trades_done. */
static inline bool bench_offer(Bench* b, const uint8_t mon[GEN3_PARTY_SIZE]) {
    Gen3ClubStatus st;
    gen3_club_status(&b->club, &st);
    return gen3_club_set_offer(&b->club, mon, st.trades_done);
}

/* bench_init(b, 0x1133) as a trade session (club_cfg_trade) offering tm_make(252, 5, 21). The
 * test's main must have called bulk_file_open. */
static inline void bench_init_trade(Bench* b) {
    uint8_t mon[GEN3_PARTY_SIZE];
    bench_init_cfg(b, GEN3_LINKTYPE_TRADE_SETUP, &club_cfg_trade);
    tm_make(252, 5, 21, mon);
    bench_offer(b, mon);
}

/* The GBA restarts (or its player is back at the counter) and links again; same club. */
static inline void ruby_relink(Bench* b) {
    fake_ruby_init(&b->ruby, &b->me);
    fake_ruby_open_link(&b->ruby);
}

static inline void count_sent(Bench* b, const MlinkTx* tx) {
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

/* Feeds one side's command words of this burst to that side's block parse (all-zero words are no
 * command). True if they completed a block, which is then logged. */
static inline bool bench_parse(
    Gen3BlockRx* rx,
    uint32_t* first,
    BenchBlock* blocks,
    uint8_t* n,
    const uint16_t cmd[GEN3_CMD_LEN],
    uint32_t burst) {
    bool command = false;
    for(size_t i = 0; i < GEN3_CMD_LEN; i++) command = command || cmd[i] != 0;
    if(!command) return false;
    switch(gen3_block_rx_feed(rx, cmd)) {
    case Gen3BlockRxStarted:
        *first = burst;
        return false;
    case Gen3BlockRxDone:
        if(*n < BENCH_BLOCKS) {
            BenchBlock* blk = &blocks[(*n)++];
            blk->size = rx->size;
            blk->code = (uint16_t)(rx->data[0] | (rx->data[1] << 8));
            blk->arg = (uint16_t)(rx->data[2] | (rx->data[3] << 8));
            blk->first_burst = *first;
            blk->last_burst = burst;
        }
        return true;
    default:
        return false;
    }
}

/* What both sides put on the wire in this step's burst: the Flipper's words first, then the
 * GBA's (a Flipper CCCC in the burst that completes the GBA's LinkPlayerBlock was sent before the
 * Flipper could see that block). */
static inline void bench_record(Bench* b, const MlinkTx* tx) {
    b->last_tx = *tx;
    if(tx->count != MLINK_BURST_LEN) return;
    uint32_t burst = ++b->bursts;
    const uint16_t* f = &tx->word[1];
    const uint16_t* g = &b->rx.word[1];

    bench_parse(&b->f_rx, &b->f_rx_first, b->f_blocks, &b->n_f_blocks, f, burst);
    if(f[0] == 0xCCCC && b->n_f_cccc < BENCH_CCCC) {
        b->f_cccc[b->n_f_cccc] = f[1];
        b->f_cccc_burst[b->n_f_cccc] = burst;
        b->n_f_cccc++;
    }
    if(f[0] == 0xCAFE && f[1] == 0x0016) {
        b->sent_cafe16++;
        b->f_cafe16_burst = burst;
    }

    if(b->rx.count != MLINK_BURST_LEN) return;
    if(bench_parse(&b->g_rx, &b->g_rx_first, b->g_blocks, &b->n_g_blocks, g, burst) &&
       b->g_rx.size == GEN3_LINK_PLAYER_BLOCK_SIZE) {
        b->n_f_cccc = 0; /* a new link session: its CCCCs count from here */
    }
    if(g[0] == 0xCAFE && g[1] < 32) {
        b->g_cafe_seen |= 1u << g[1];
        if(g[1] == 0x0016) b->g_cafe16_burst = burst;
    }
}

/* While muted, step() doesn't call the club: the fake gets frames without transfers (a stalled
 * master). b->rx keeps the answer to the club's last tx for when it runs again. */
static inline void bench_mute(Bench* b, bool muted) {
    b->muted = muted;
}

static inline void step(Bench* b) {
    MlinkTx tx;
    if(b->muted) {
        MlinkRx none;
        memset(&tx, 0, sizeof(tx));
        fake_ruby_frame(&b->ruby, &tx, &none);
        return;
    }
    gen3_club_frame(&b->club, b->started ? &b->rx : NULL, &tx);
    b->started = true;
    count_sent(b, &tx);
    fake_ruby_frame(&b->ruby, &tx, &b->rx);
    bench_record(b, &tx);
}

/* The codes (words 0) of the 20-byte (§B3.1 trade) blocks among blocks[0..n), in order; returns
 * their count. */
static inline size_t bench_codes(const BenchBlock* blocks, uint8_t n, uint16_t* out, size_t max) {
    size_t count = 0;
    for(uint8_t i = 0; i < n && count < max; i++) {
        if(blocks[i].size == 20) out[count++] = blocks[i].code;
    }
    return count;
}

static inline void run(Bench* b, uint32_t frames) {
    for(uint32_t i = 0; i < frames; i++) step(b);
}

static inline Gen3ClubStatus club_status(const Bench* b) {
    Gen3ClubStatus st;
    gen3_club_status(&b->club, &st);
    return st;
}

static inline bool run_until_phase(Bench* b, Gen3ClubPhase phase, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(club_status(b).phase == phase) return true;
    }
    return false;
}

static inline bool at_cards_or_later(Gen3ClubPhase p) {
    return p == Gen3ClubPhaseCards || p == Gen3ClubPhaseRoom;
}

/* Until both sides are in the Trade Center: the club answered 2FFE, the fake got that answer. */
static inline bool run_until_room(Bench* b, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        bool club_in = club_status(b).phase == Gen3ClubPhaseRoom;
        if(club_in && fake_ruby_phase(&b->ruby) == FakeRubyRoom) return true;
    }
    return false;
}

static inline bool reach_room(Bench* b) {
    bench_init(b, GEN3_LINKTYPE_TRADE_SETUP);
    fake_ruby_open_link(&b->ruby);
    return run_until_room(b, 400) && fake_ruby_error(&b->ruby) == NULL;
}

/* As reach_room, on a bench already initialised (no re-init). */
static inline bool bench_to_room(Bench* b) {
    fake_ruby_open_link(&b->ruby);
    return run_until_room(b, 420) && fake_ruby_error(&b->ruby) == NULL;
}

/* For `frames` frames a fresh Gen3Link is the master instead of the club: it handshakes, and on
 * Connected queues {2222, 1133} and lp's LinkPlayerBlock once, then idles. The club doesn't run,
 * so it is out of step with the wire afterwards. */
static inline void bench_raw_relink(Bench* b, const Gen3LinkPlayer* lp, uint32_t frames) {
    static const uint16_t req[GEN3_CMD_LEN] = {0x2222, GEN3_LINKTYPE_TRADE_SETUP};
    uint8_t block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    Gen3Link link;
    bool sent = false;
    gen3_link_player_block_build(lp, block);
    gen3_link_init(&link);
    for(uint32_t i = 0; i < frames; i++) {
        MlinkTx tx;
        uint16_t cmd[GEN3_CMD_LEN];
        /* b->rx answers the club's last tx: a fresh link starts without it */
        if(i > 0 && gen3_link_rx(&link, &b->rx, cmd) == Gen3LinkEventConnected && !sent) {
            gen3_link_send(&link, req);
            gen3_block_send(&link, block, sizeof(block), 0);
            sent = true;
        }
        gen3_link_tx(&link, &tx);
        fake_ruby_frame(&b->ruby, &tx, &b->rx);
        bench_record(b, &tx);
    }
}

/* The GBA player walks out of the room (§B7); the club must go through Closing to Waiting. */
static inline bool leave_room(Bench* b) {
    fake_ruby_leave_room(&b->ruby);
    return run_until_phase(b, Gen3ClubPhaseClosing, 600) &&
           run_until_phase(b, Gen3ClubPhaseWaiting, 60);
}
