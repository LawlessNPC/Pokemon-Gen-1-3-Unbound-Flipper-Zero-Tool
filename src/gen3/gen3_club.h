#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/mlink_types.h>
#include <src/gen3/gen3_link.h>
#include <src/gen3/gen3_block.h>
#include <src/gen3/gen3_link_player.h>

/* Cable Club (Trade Center) state machine, Flipper as multiplayer master: linkup, the room and
 * leaving it (docs/gen3/link-protocol.md §A6-A8, §B7). Pure C, no heap; gen3_club_frame runs in
 * the link frame ISR. */

#define GEN3_CLUB_CARD_DELAY_BURSTS 10 /* idle bursts between both player blocks and 0xCCCC (§A6, U3 margin >= 5) */

typedef enum {
    Gen3ClubPhaseWaiting,
    Gen3ClubPhaseLinked,
    Gen3ClubPhasePlayers,
    Gen3ClubPhaseCards,
    Gen3ClubPhaseRoom,
    Gen3ClubPhaseClosing,
    Gen3ClubPhaseError, /* bad player block or other room: only the GBA's 5FFF is answered */
} Gen3ClubPhase;

typedef enum {
    Gen3ClubErrorNone,
    Gen3ClubErrorChecksum, /* recorded only: the phase goes on (the GBA may be fine) */
    Gen3ClubErrorBadPlayerBlock,
    Gen3ClubErrorOtherRoom,
} Gen3ClubError;

typedef struct {
    const char* name; /* up to 7 chars are sent */
    uint16_t tid;
    uint16_t sid;
    uint8_t gender; /* 0 male, anything else is sent as 1 */
} Gen3ClubConfig;

typedef struct {
    Gen3ClubPhase phase;
    Gen3ClubError last_error; /* last_error survives the reset to Waiting */
    bool partner_valid;
    Gen3LinkPlayer partner;
    uint32_t frames;
    uint32_t room_frames; /* frames spent in Room since init, over all sessions */
    Gen3LinkStats link;
} Gen3ClubStatus;

typedef struct Gen3Club Gen3Club;

/* Fully defined so callers can embed it. Fields are private: use the API only. */
struct Gen3Club {
    Gen3Link link;
    Gen3BlockRx block_rx; /* the GBA's current block (its LinkPlayerBlock, then its card) */
    uint8_t own_player_block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    uint8_t own_card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    Gen3ClubPhase phase;
    Gen3ClubError last_error;
    bool partner_valid;
    Gen3LinkPlayer partner;
    bool card_requested; /* 0xCCCC and own card queued this session */
    bool close_answered; /* {5FFF} queued this session */
    bool exit_answer_due; /* the GBA sent CAFE 0017: answer after its next command */
    uint8_t card_delay; /* idle bursts to wait before 0xCCCC */
    uint8_t idle_bursts;
    uint32_t frames;
    uint32_t room_frames;
};

void gen3_club_init(Gen3Club* club, const Gen3ClubConfig* cfg);
/* One call per frame from the driver callback. rx is NULL on the first call. */
void gen3_club_frame(Gen3Club* club, const MlinkRx* rx, MlinkTx* tx);
void gen3_club_status(const Gen3Club* club, Gen3ClubStatus* out);
/* Tests only; production uses GEN3_CLUB_CARD_DELAY_BURSTS. */
void gen3_club_set_card_delay(Gen3Club* club, uint8_t bursts);
