#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_trade_block.h>

/* The app side of a trade session (D6): between UI events the app tick moves the received
 * Pokemon into the editor, hands the editor the GBA player (D7) and publishes the editor's
 * Pokemon as the offer, in that order. Also the trade screen's guide text. App thread only: this
 * reads the bulk file (gen3_edit_load) and is never called from the link ISR. Pure C, no heap. */

/* The GBA player as an OT (D7): name[0..6] raw, TID/SID from trainer_id's low/high halves,
 * gender 0 or 1 (any non-zero gender is 1). */
void gen3_trainer_from_link_player(const Gen3LinkPlayer* lp, Gen3Trainer* out);

/* Why the latest received Pokemon isn't in the editor (O2) */
typedef enum {
    Gen3PickupOk, /* no failed pickup outstanding */
    Gen3PickupNoData, /* gen3_edit_load failed with the bulk file unreadable: retried every poll */
    Gen3PickupBadData, /* the received bytes don't decrypt, or gen3_edit_load refused them: not retried */
} Gen3PickupFail;

typedef struct Gen3TradeSync {
    uint32_t trades_seen; /* trades whose Pokemon the editor has taken (or failed to, O2) */
    bool trainer_known;
    Gen3Trainer trainer; /* last trainer handed to the editor */
    bool offer_valid; /* offer holds the editor's Pokemon, game form; false: offer nothing */
    uint8_t offer[GEN3_PARTY_SIZE]; /* zeros unless offer_valid */
    Gen3Refuse offer_refuse; /* why the editor's Pokemon can't be offered; None if it can */
    bool taken; /* the session took an offer: */
    bool taken_valid; /* the (offer_valid, offer) it holds */
    uint8_t taken_offer[GEN3_PARTY_SIZE];
    Gen3Pkm scratch; /* the received Pokemon, decrypted */
    Gen3PickupFail pickup_fail;
    /* After a failed pickup the editor still holds the Pokemon traded away (O2): its PID and OT
     * ID. It isn't offered again while the editor's PID and OT ID are these. */
    bool gone_valid;
    uint32_t gone_pid;
    uint32_t gone_ot_id;
} Gen3TradeSync;

void gen3_trade_sync_init(Gen3TradeSync* sync);

/* Every session stop or start (T12 review, G15 across sessions): as gen3_trade_sync_init, for a
 * fresh club (trades_seen 0, no offer taken, no trainer known), but the block on the traded-away
 * Pokemon stays: gone_valid, gone_pid, gone_ot_id and a BadData pickup_fail. A NoData retry still
 * waiting becomes BadData: its Pokemon went with the old club. Without a block, the same bytes as
 * gen3_trade_sync_init. */
void gen3_trade_sync_restart(Gen3TradeSync* sync);

/* gen3_trade_sync_poll's result bits */
enum {
    Gen3SyncPickedUp = 1u << 0, /* the received Pokemon is now the editor's */
    Gen3SyncPickupBad = 1u << 1, /* it couldn't be taken: pickup_fail says why */
    Gen3SyncTrainer = 1u << 2, /* the editor got a new GBA player (gen3_edit_set_trainer) */
    Gen3SyncOffer = 1u << 3, /* publish (offer_valid ? offer : no offer), then offer_taken */
};

/* App thread, between UI events. received/received_count come from one
 * gen3_session_get_received. editing (the current scene is neither Gen3Trade nor Gen3Menu):
 * return 0 and do nothing (D6). Else, in order:
 * 1. Pickup, on a new trade (received_count > trades_seen) or to retry a NoData pickup.
 *    A new trade first notes the editor's PID and OT ID (gone_*) and sets trades_seen =
 *    received_count, whatever follows. Then gen3_pkm_decrypt and gen3_edit_load: PickedUp, with
 *    pickup_fail Ok; or PickupBad with the editor unchanged, pickup_fail NoData if the decrypt
 *    worked and the bulk file isn't readable after the load, else BadData.
 *    received_count < trades_seen is a club reset: trades_seen = received_count, no pickup, and a
 *    waiting NoData retry becomes BadData (its Pokemon went with the old club).
 * 2. st->trainer_valid and not the known trainer: gen3_edit_set_trainer, Trainer. While gone_valid,
 *    a set_trainer that changes the editor's PID or OT ID moves gone_* with it.
 * 3. A BadData pickup is cleared once the editor's PID or OT ID differs from gone_*. Then
 *    offer_refuse = gen3_trade_offer_check(&ed->pkm), offer_valid = offer_refuse None and no
 *    failed pickup, offer = the game form when valid (G6: only a Pokemon that passes the check is
 *    ever published). Offer if nothing was taken yet, or (offer_valid, offer) differs from what
 *    was taken. */
unsigned gen3_trade_sync_poll(
    Gen3TradeSync* sync,
    Gen3Editor* ed,
    const Gen3ClubStatus* st,
    const uint8_t received[GEN3_PARTY_SIZE],
    uint32_t received_count,
    bool editing);
/* The session accepted the offer (or "none"): gen3_session_set_offer returned true. */
void gen3_trade_sync_offer_taken(Gen3TradeSync* sync);

#define GEN3_GUIDE_LINES    3
#define GEN3_GUIDE_LINE_LEN 22 /* 21 chars + NUL: FontSecondary across 128 px */

/* What is happening and what to do on the GBA. started: the session runs (or ran and ended);
 * cable_ok: the cable reads right; offer_refuse and pickup_fail: the sync's. Unused lines are "".
 * The guide never reads left as "trade resolved", nor refused alone as "no offer" (an R8 DDEE
 * leaves it None). Written through gen3_trade_guide_copy: false if a text, row or reason, had to
 * be cut to fit (test_guide_line_lengths: never). */
bool gen3_trade_guide(
    const Gen3ClubStatus* st,
    bool started,
    bool cable_ok,
    Gen3Refuse offer_refuse,
    Gen3PickupFail pickup_fail,
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN]);
/* The three lines into out, each cut at GEN3_GUIDE_LINE_LEN - 1 characters and NUL-terminated.
 * false if any was cut. */
bool gen3_trade_guide_copy(
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN],
    const char* l1,
    const char* l2,
    const char* l3);
