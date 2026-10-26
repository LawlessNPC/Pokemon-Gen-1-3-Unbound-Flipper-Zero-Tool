#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/mlink_types.h>
#include <src/gen3/gen3_link.h>
#include <src/gen3/gen3_block.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_trade_block.h>

/* Cable Club (Trade Center) state machine, Flipper as multiplayer master: linkup, the room and
 * leaving it (docs/gen3/link-protocol.md §A6-A8, §B7), and in a trade session sitting down, the
 * relink for the trade menu and its party exchange (docs/gen3/link-protocol-trade.md §B1, §B2).
 * Pure C, no heap; gen3_club_frame runs in the link frame ISR. */

#define GEN3_CLUB_CARD_DELAY_BURSTS 10 /* idle bursts between both player blocks and 0xCCCC (§A6, U3 margin >= 5) */
/* Idle bursts before a party-exchange CCCC (§B2.2). Round 1's follows both LinkPlayerBlocks: the
 * GBA must take it 3 frames after them or later, and 10 also absorbs a GBA main-loop stall of up
 * to 8 frames there. Rounds 2-5 follow both blocks of the previous round: this delay is
 * defensive padding (§B2.2's ">= 5" is inferred), not required by pokeruby. The CCCC reaches the
 * GBA's receive queue behind both blocks, so the GBA takes it no earlier than the frame it ends
 * the round, and its copy for the next round (a frame later) precedes the block's first chunk
 * (3 frames after the request). */
#define GEN3_CLUB_ROUND1_DELAY_BURSTS 10
#define GEN3_CLUB_ROUND_DELAY_BURSTS  6
#define GEN3_CLUB_GBA_PARTY           6 /* the GBA's party slots */

/* The values are the phase numbers in the link log: append only. */
typedef enum {
    Gen3ClubPhaseWaiting = 0,
    Gen3ClubPhaseLinked = 1,
    Gen3ClubPhasePlayers = 2,
    Gen3ClubPhaseCards = 3,
    Gen3ClubPhaseRoom = 4,
    Gen3ClubPhaseClosing = 5,
    Gen3ClubPhaseError = 6, /* bad player block, other room, unexpected: only 5FFF is answered */
    Gen3ClubPhaseExchange = 7, /* 0x1122 relink: party exchange (trade menu loading) */
    Gen3ClubPhaseMenu = 8, /* trade menu */
    Gen3ClubPhaseTrade = 9, /* 0x1144 relink: trade animation */
    Gen3ClubPhaseSave = 10, /* the GBA's link save */
} Gen3ClubPhase;

typedef enum {
    Gen3ClubErrorNone = 0,
    Gen3ClubErrorChecksum = 1, /* recorded only: the phase goes on (the GBA may be fine) */
    Gen3ClubErrorBadPlayerBlock = 2,
    Gen3ClubErrorOtherRoom = 3,
    Gen3ClubErrorUnexpected = 4, /* a link type the club didn't expect at this point */
} Gen3ClubError;

/* name must outlive the club (a string literal): the club keeps a copy of the config. */
typedef struct {
    const char* name; /* up to 7 chars are sent */
    uint16_t tid;
    uint16_t sid;
    uint8_t gender; /* 0 male, anything else is sent as 1 */
    bool trading; /* trade session; false (Link Test) never sits down */
} Gen3ClubConfig;

typedef struct {
    Gen3ClubPhase phase;
    Gen3ClubError last_error; /* last_error survives the reset to Waiting */
    bool partner_valid; /* the GBA's LinkPlayer of this session was accepted */
    Gen3LinkPlayer partner;
    uint32_t frames;
    uint32_t room_frames; /* frames spent in Room since init, over all sessions */
    Gen3LinkStats link;
    uint32_t link_type; /* the GBA's link type this session; 0 until its LinkPlayerBlock arrives */
    bool trainer_valid; /* trainer: the GBA's LinkPlayer from the last 0x1133 linkup, kept across */
    Gen3LinkPlayer trainer; /* disconnects and relinks */
    bool has_offer;
    bool gba_seated; /* this room session: the GBA sent CAFE 0016 and hasn't sent 001D/001A since */
    bool flipper_seated; /* this room session: the Flipper sent CAFE 0016 */
    uint32_t trades_done; /* finalized trades since init; stays 0 until Task 6 */
    uint8_t round; /* Exchange/Menu: the round in progress or done, 1..5; 0 before round 1 */
} Gen3ClubStatus;

typedef struct Gen3Club Gen3Club;

/* Fully defined so callers can embed it. Fields are private: use the API only. */
struct Gen3Club {
    Gen3Link link;
    Gen3BlockRx block_rx; /* the GBA's current block (its LinkPlayerBlock, then its card) */
    Gen3ClubConfig cfg;
    /* The Flipper's blocks for this session, from gen3_club_claim_game when the GBA's
     * LinkPlayerBlock arrives */
    uint8_t own_player_block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    uint8_t own_card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    Gen3ClubPhase phase;
    Gen3ClubError last_error;
    bool partner_valid;
    Gen3LinkPlayer partner;
    uint32_t link_type;
    bool trainer_valid; /* never reset after init */
    Gen3LinkPlayer trainer;
    bool card_requested; /* 0xCCCC and own card queued this session */
    bool close_answered; /* {5FFF} queued this session */
    bool exit_answer_due; /* the GBA sent CAFE 0017: answer after its next command */
    uint8_t card_delay; /* idle bursts to wait before 0xCCCC */
    uint8_t idle_bursts;
    uint32_t frames;
    uint32_t room_frames;
    /* Room seats, per room session (§B1.3, §B1.4) */
    bool gba_seated;
    bool flipper_seated;
    /* Party exchange, per session (§B2) */
    uint8_t round; /* 0 before round 1, then the round in progress or done */
    bool round_gba_done; /* the GBA's block of this round is in */
    uint8_t round1_delay; /* idle bursts before round 1's CCCC */
    uint8_t round_delay; /* before the CCCC of rounds 2..5 */
    uint8_t round_block[GEN3_TRADE_ROUND_MAX]; /* the Flipper's block of this round (R2) */
    /* Trade context: survives relinks (D3) */
    uint8_t offer[GEN3_PARTY_SIZE]; /* the Flipper's one party Pokemon, game form; zeros if none */
    bool has_offer;
    uint32_t trades_done;
    uint8_t party_mon[GEN3_PARTY_SIZE]; /* the Flipper mon the GBA was shown: offer at round 1 */
    uint8_t gba_party[GEN3_CLUB_GBA_PARTY][GEN3_PARTY_SIZE]; /* game form, rounds 1-3 */
};

/* The one place that decides which game the Flipper claims to be (D11). Fills the Flipper's
 * LinkPlayer for a session of link_type, and its trainer card. Plan 3 always claims Ruby:
 * version GEN3_VERSION_RUBY, name cfg->name (7 chars, EOS-filled to 11), trainer_id sid<<16|tid,
 * gender 0/1, id 0, language GEN3_LANGUAGE_ENGLISH, card gen3_trainer_card_build (§A5). lp_field_2
 * is GEN3_LP_NATIONAL_DEX only for GEN3_LINKTYPE_TRADE_DISCONNECTED with national_dex set, else 0.
 * partner (the GBA's LinkPlayer of this session) is unused until plan 4 claims FireRed/LeafGreen
 * for an FRLG partner (§C1, §C2). partner may be zeroed (bad block) or unaccepted (other type);
 * plan 4's FRLG choice must default to Ruby then. Pure: safe in the link ISR. */
void gen3_club_claim_game(
    const Gen3ClubConfig* cfg,
    const Gen3LinkPlayer* partner,
    uint32_t link_type,
    bool national_dex,
    Gen3LinkPlayer* lp,
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE]);

void gen3_club_init(Gen3Club* club, const Gen3ClubConfig* cfg);
/* One call per frame from the driver callback. rx is NULL on the first call. */
void gen3_club_frame(Gen3Club* club, const MlinkRx* rx, MlinkTx* tx);
void gen3_club_status(const Gen3Club* club, Gen3ClubStatus* out);
/* Sets (game != NULL) or clears the offer: the Flipper's one party Pokemon, game form. false and
 * nothing changed if trades_seen != the club's trades_done (D6; trades_done is 0 until Task 6).
 * Unchecked (G6): the app offers only a Pokemon that passes gen3_trade_offer_check. */
bool gen3_club_set_offer(Gen3Club* club, const uint8_t* game, uint32_t trades_seen);
/* Tests only; production uses GEN3_CLUB_CARD_DELAY_BURSTS. */
void gen3_club_set_card_delay(Gen3Club* club, uint8_t bursts);
/* Tests only; production uses GEN3_CLUB_ROUND1_DELAY_BURSTS and GEN3_CLUB_ROUND_DELAY_BURSTS. */
void gen3_club_set_round_delays(Gen3Club* club, uint8_t first, uint8_t later);
/* The GBA's party slot 0..5 (game form) from the last exchange; zeros for another slot or a slot
 * no exchange has filled. Each of rounds 1-3 refills two slots. */
void gen3_club_gba_mon(const Gen3Club* club, uint8_t slot, uint8_t out[GEN3_PARTY_SIZE]);
