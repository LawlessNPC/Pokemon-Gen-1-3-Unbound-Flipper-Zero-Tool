#include <src/gen3/gen3_club.h>

#include <src/gen3/gen3_text.h>

#include <string.h>

#define CMD_SEND_LINK_TYPE     0x2222
#define CMD_READY_EXIT_STANDBY 0x2FFE
#define CMD_READY_CLOSE_LINK   0x5FFF
#define CMD_SEND_HELD_KEYS     0xCAFE
#define CMD_SEND_BLOCK_REQ     0xCCCC

/* Room key codes (§A7, §B1.2) */
#define KEY_CODE_READY     0x16
#define KEY_CODE_EXIT_ROOM 0x17
#define KEY_CODE_IDLE      0x1A
#define KEY_CODE_EXIT_SEAT 0x1D

#define BLOCK_REQ_TRAINER_CARD 2
#define FLIPPER_MULTIPLAYER_ID 0 /* slot 0: block headers carry 0x80 */
#define NAME_MAX_CHARS         7

/* Byte 0x13 of a Pokemon, game form or not (the header is never encrypted): bit 1 is hasSpecies
 * (pokeruby BoxPokemon, PKHeX PK3; Gen3FieldHasSpecies in gen3_pkm.c) */
#define PKM_FLAGS_OFF   0x13
#define PKM_HAS_SPECIES 0x02

static bool send_cmd(Gen3Club* club, uint16_t w0, uint16_t w1) {
    uint16_t cmd[GEN3_CMD_LEN] = {0};
    cmd[0] = w0;
    cmd[1] = w1;
    return gen3_link_send(&club->link, cmd);
}

/* Any non-zero word, as for Gen3LinkEventCommand (an all-zero burst is no command, §A2). */
static bool is_command(const uint16_t cmd[GEN3_CMD_LEN]) {
    for(size_t i = 0; i < GEN3_CMD_LEN; i++) {
        if(cmd[i] != 0) return true;
    }
    return false;
}

/* Bad player block or other room. A checksum error never stops the club: see gen3_club_frame. */
static void set_error(Gen3Club* club, Gen3ClubError error) {
    club->phase = Gen3ClubPhaseError;
    club->last_error = error;
}

/* Per link session, so the seats are per room session. The trainer, the config and the trade
 * context (D3) outlive it. */
static void reset_session(Gen3Club* club) {
    gen3_block_rx_reset(&club->block_rx);
    club->partner_valid = false;
    memset(&club->partner, 0, sizeof(club->partner));
    club->link_type = 0;
    club->card_requested = false;
    club->close_answered = false;
    club->exit_answer_due = false;
    club->idle_bursts = 0;
    club->gba_seated = false;
    club->flipper_seated = false;
    club->round = 0;
    club->round_gba_done = false;
    club->menu = Gen3ClubMenuIdle;
    club->reply_due = false;
    club->gba_slot = 0;
    club->refused = Gen3RefuseNone;
    club->party_shown = false;
}

void gen3_club_claim_game(
    const Gen3ClubConfig* cfg,
    const Gen3LinkPlayer* partner,
    uint32_t link_type,
    bool national_dex,
    Gen3LinkPlayer* lp,
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE]) {
    (void)partner; /* Ruby is claimed whatever the GBA is */
    memset(lp, 0, sizeof(*lp));
    lp->version = GEN3_VERSION_RUBY;
    /* §B4.5: Ruby reads it only from the 0x1144 block */
    lp->lp_field_2 =
        (link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED && national_dex) ? GEN3_LP_NATIONAL_DEX : 0;
    lp->trainer_id = ((uint32_t)cfg->sid << 16) | cfg->tid;
    gen3_text_encode(cfg->name ? cfg->name : "", lp->name, sizeof(lp->name), NAME_MAX_CHARS);
    lp->gender = cfg->gender ? 1 : 0; /* indexes the GBA's avatar table: 0 or 1 only (§A5) */
    lp->link_type = link_type;
    lp->id = 0;
    lp->language = GEN3_LANGUAGE_ENGLISH;
    gen3_trainer_card_build(lp, card);
}

void gen3_club_init(Gen3Club* club, const Gen3ClubConfig* cfg) {
    memset(club, 0, sizeof(*club));
    gen3_link_init(&club->link);
    club->cfg = *cfg;
    club->card_delay = GEN3_CLUB_CARD_DELAY_BURSTS;
    club->round1_delay = GEN3_CLUB_ROUND1_DELAY_BURSTS;
    club->round_delay = GEN3_CLUB_ROUND_DELAY_BURSTS;
    club->phase = Gen3ClubPhaseWaiting;
    club->last_error = Gen3ClubErrorNone;
}

/* §A6 B1: {2222,1133}. Ruby doesn't read word 1 (§A1), and answers with its LinkPlayerBlock. */
static void on_connected(Gen3Club* club) {
    reset_session(club);
    club->phase = Gen3ClubPhaseLinked;
    send_cmd(club, CMD_SEND_LINK_TYPE, GEN3_LINKTYPE_TRADE_SETUP);
}

/* D2: the Flipper's block carries the GBA's link type only where the club follows the GBA there
 * (§B1.6, §B4.1: a mismatch hangs the GBA silently, before anything is written). Otherwise
 * 0x1133: a Link Test never trades, 0x1144 is followed only for a pending trade (CCDD sent), and
 * the 0x1111 relink comes with Task 7. */
static uint32_t own_link_type(const Gen3Club* club, uint32_t gba_link_type) {
    if(!club->cfg.trading) return GEN3_LINKTYPE_TRADE_SETUP;
    if(gba_link_type == GEN3_LINKTYPE_TRADE_CONNECTING) return gba_link_type;
    if(gba_link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED && club->trade_pending) {
        return gba_link_type;
    }
    return GEN3_LINKTYPE_TRADE_SETUP;
}

static bool is_trade_link_type(uint32_t link_type) {
    return link_type == GEN3_LINKTYPE_TRADE || link_type == GEN3_LINKTYPE_TRADE_CONNECTING ||
           link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED;
}

/* §A6 "Optional" (D2): the Flipper's LinkPlayerBlock follows the GBA's. Six commands; nothing
 * else is queued while the GBA's block comes in. */
static void send_own_player_block(Gen3Club* club, uint32_t link_type) {
    Gen3LinkPlayer lp;
    gen3_club_claim_game(
        &club->cfg, &club->partner, link_type, club->national_dex, &lp, club->own_card);
    gen3_link_player_block_build(&lp, club->own_player_block);
    gen3_block_send(
        &club->link,
        club->own_player_block,
        sizeof(club->own_player_block),
        FLIPPER_MULTIPLAYER_ID);
}

/* The pending trade is over (D5): Done (sync 3) and Assumed (lost after the sync-2 answer) take
 * the GBA's Pokemon, which is then also the offer and the Pokemon the GBA is shown next (§B6.1).
 * Field to field, no local buffer (R2): 300 B. */
static void end_trade(Gen3Club* club, Gen3TradeResult result) {
    if(result == Gen3TradeResultDone || result == Gen3TradeResultAssumed) {
        memcpy(club->received, club->pending_mon, sizeof(club->received));
        memcpy(club->party_mon, club->pending_mon, sizeof(club->party_mon));
        memcpy(club->offer, club->pending_mon, sizeof(club->offer));
        club->has_offer = true;
        club->trades_done++;
    }
    club->trade_pending = false;
    club->save_window = false;
    club->last_result = result;
}

/* The GBA's first block of a session is complete. The Flipper's own block goes out for every
 * completed block, whatever it holds, so the GBA never waits forever for one. A bad block header
 * (Gen3BlockRxBad) sends no block. */
static void on_player_block(Gen3Club* club) {
    Gen3LinkPlayer lp;
    uint32_t own_type = GEN3_LINKTYPE_TRADE_SETUP;
    if(club->block_rx.size != GEN3_LINK_PLAYER_BLOCK_SIZE ||
       !gen3_link_player_block_parse(club->block_rx.data, &lp)) {
        set_error(club, Gen3ClubErrorBadPlayerBlock);
    } else {
        if(club->trade_pending && lp.link_type != GEN3_LINKTYPE_TRADE_DISCONNECTED) {
            /* D3: the GBA restarted or came back from the counter: the trade never began on it */
            end_trade(club, Gen3TradeResultCanceled);
        }
        club->partner = lp;
        club->link_type = lp.link_type;
        own_type = own_link_type(club, lp.link_type);
        if(lp.link_type == GEN3_LINKTYPE_TRADE_SETUP) {
            club->partner_valid = true;
            club->trainer = lp; /* the GBA's player, kept across relinks */
            club->trainer_valid = true;
            club->phase = Gen3ClubPhasePlayers;
            club->idle_bursts = 0;
        } else if(own_type == GEN3_LINKTYPE_TRADE_CONNECTING) {
            /* §B1.6: the trade menu. The party exchange starts after both blocks. */
            club->partner_valid = true;
            club->phase = Gen3ClubPhaseExchange;
            club->idle_bursts = 0;
        } else if(own_type == GEN3_LINKTYPE_TRADE_DISCONNECTED) {
            /* §B4.1: the pending trade's animation, then the GBA's ABCD and its link save */
            club->partner_valid = true;
            club->phase = Gen3ClubPhaseTrade;
        } else if(is_trade_link_type(lp.link_type)) {
            /* A Cable Club trade type the club doesn't follow: the 0x1133 block stalls the GBA. */
            set_error(club, Gen3ClubErrorUnexpected);
        } else {
            /* At a 0x1133 linkup the GBA reports "different selections" and sends 5FFF (§A6). */
            set_error(club, Gen3ClubErrorOtherRoom);
        }
    }
    gen3_block_rx_reset(&club->block_rx);
    send_own_player_block(club, own_type);
}

/* §A7 "What the Flipper must react to while idling": one answer per request; idle bursts stay
 * all zero (U6). G's CAFE 0017 is answered one G command later. In the game update right after
 * G's own 0x17 comes back, Ruby turns our IDLE avatar BUSY and drops that update's key
 * (pokeruby overworld.c sub_8054FF8), so an answer in the very next burst is lost and G waits
 * forever. A later G command is a receive-queue entry of its own, whose update comes between. */
static void on_room_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(club->exit_answer_due) {
        send_cmd(club, CMD_SEND_HELD_KEYS, KEY_CODE_EXIT_ROOM);
        club->exit_answer_due = false;
    }
    if(cmd[0] == CMD_READY_EXIT_STANDBY) {
        send_cmd(club, CMD_READY_EXIT_STANDBY, 0);
    } else if(cmd[0] == CMD_SEND_HELD_KEYS) {
        switch(cmd[1]) {
        case KEY_CODE_EXIT_ROOM:
            club->exit_answer_due = true;
            break;
        case KEY_CODE_READY: /* §B1.2: the GBA sat down */
            club->gba_seated = true;
            break;
        case KEY_CODE_EXIT_SEAT: /* B on "Please wait..." */
        case KEY_CODE_IDLE: /* its seat script ended */
            club->gba_seated = false;
            break;
        default:
            break;
        }
    }
}

/* §B1.4 with R5: in a trade session with an offer, the Flipper sits as soon as the GBA sits, or
 * later once it has an offer (the GBA polls GetCableClubPartnersReady with no timeout). It never
 * sits first. One CAFE 0016, then zero bursts only, so G's 5FFF isn't delayed (§B1.5). It stays
 * READY if the GBA stands up (§B1.4: legal). */
static void try_sit(Gen3Club* club) {
    if(club->phase != Gen3ClubPhaseRoom || !club->cfg.trading || !club->has_offer ||
       !club->gba_seated || club->flipper_seated) {
        return;
    }
    /* A full queue sends nothing: try again next frame */
    if(send_cmd(club, CMD_SEND_HELD_KEYS, KEY_CODE_READY)) club->flipper_seated = true;
}

/* §B2.1: the GBA's block of the round in progress. Rounds 1-3 carry its party, two slots each;
 * rounds 4 (mail) and 5 (gift ribbons) are not kept. A block the club didn't ask for, or of
 * another size, leaves the GBA and the club out of step. */
static void on_exchange_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    Gen3BlockRxResult r = gen3_block_rx_feed(&club->block_rx, cmd);
    if(r == Gen3BlockRxBad) {
        set_error(club, Gen3ClubErrorUnexpected);
        return;
    }
    if(r != Gen3BlockRxDone) return;
    if(club->round == 0 || club->round_gba_done ||
       club->block_rx.size != gen3_trade_round_size(club->round)) {
        set_error(club, Gen3ClubErrorUnexpected);
        return;
    }
    if(club->round <= 3) {
        memcpy(club->gba_party[2 * (club->round - 1)], club->block_rx.data, 2 * GEN3_PARTY_SIZE);
    }
    club->round_gba_done = true;
}

/* Round r: {CCCC, type} and the Flipper's block, 17 or 18 commands into an empty queue. Round 1
 * fixes the Pokemon the GBA is shown: the offer now, or zeros without one (R8). */
static void start_round(Gen3Club* club, uint8_t round) {
    if(round == 1 && club->has_offer) {
        memcpy(club->party_mon, club->offer, sizeof(club->party_mon));
    } else if(round == 1) {
        memset(club->party_mon, 0, sizeof(club->party_mon));
    }
    /* R8: Ruby counts the Flipper's party up to the first species 0. The offer has a species
     * (G6); without one the GBA's partner column is empty. The hasSpecies bit is required too
     * (fix round 1): conservative, Ruby counts by the species alone. */
    if(round == 1) {
        club->party_shown = club->has_offer &&
                            (club->party_mon[PKM_FLAGS_OFF] & PKM_HAS_SPECIES) != 0;
    }
    gen3_trade_round_block(round, round == 1 ? club->party_mon : NULL, club->round_block);
    send_cmd(club, CMD_SEND_BLOCK_REQ, gen3_trade_round_request(round));
    gen3_block_send(
        &club->link, club->round_block, gen3_trade_round_size(round), FLIPPER_MULTIPLAYER_ID);
    club->round = round;
    club->round_gba_done = false;
    club->idle_bursts = 0;
    /* Drops a partial block, if any: no full gen3_block_rx_reset (a 262-B memset) in the ISR.
     * Chunks are ignored until the next header, which sets size and pos = 0, and Done comes only
     * once that block's chunks have rewritten every byte below size. */
    club->block_rx.active = false;
}

/* §B2.2. A step is complete once our block has left (the queue is empty) and the GBA's is in:
 * both LinkPlayerBlocks before round 1, both blocks of round r before round r + 1. Then
 * round1_delay idle bursts (the GBA's case 1 must have run) or round_delay (padding, see
 * gen3_club.h). After round 5: the trade menu. */
static void exchange_step(Gen3Club* club) {
    if(club->phase != Gen3ClubPhaseExchange) return;
    if(club->round > 0 && !club->round_gba_done) return;
    if(gen3_link_queued(&club->link) != 0) return;
    if(club->round == GEN3_TRADE_ROUNDS) {
        club->phase = Gen3ClubPhaseMenu;
        return;
    }
    if(club->idle_bursts < (club->round == 0 ? club->round1_delay : club->round_delay)) {
        club->idle_bursts++; /* the burst built below is idle */
        return;
    }
    start_round(club, (uint8_t)(club->round + 1));
}

/* §B3.2: the GBA's trade-menu codes, each answered once (§B3.4). Every other code gets no reply,
 * the R-button debug block (word 0 = 0) among them. menu_code is the one list of them:
 * on_menu_command files a reply only for these, and menu_reply covers each (-Wswitch). */
typedef enum {
    MenuCodeNone,
    MenuCodeReady, /* AABB */
    MenuCodeConfirm, /* BBBB */
    MenuCodeConfirmNo, /* BBCC */
    MenuCodeCancel, /* EEAA */
} MenuCode;

static MenuCode menu_code(uint16_t code) {
    switch(code) {
    case GEN3_TRADE_READY:
        return MenuCodeReady;
    case GEN3_TRADE_CONFIRM:
        return MenuCodeConfirm;
    case GEN3_TRADE_CONFIRM_NO:
        return MenuCodeConfirmNo;
    case GEN3_TRADE_CANCEL:
        return MenuCodeCancel;
    default:
        return MenuCodeNone;
    }
}

/* §B3.3: the GBA's 20-byte blocks in the trade menu. menu_step answers each once. In Starting
 * and Leaving the GBA is on its way out: no reply. */
static void on_menu_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(gen3_block_rx_feed(&club->block_rx, cmd) != Gen3BlockRxDone) return;
    if(club->block_rx.size != GEN3_TRADE_BLOCK_SIZE) return;
    if(club->menu == Gen3ClubMenuStarting || club->menu == Gen3ClubMenuLeaving) return;
    uint16_t arg;
    const uint16_t code = gen3_trade_block_code(club->block_rx.data, &arg);
    if(menu_code(code) == MenuCodeNone) return;
    club->gba_code = code;
    club->gba_arg = arg;
    club->reply_due = true;
    club->idle_bursts = 0;
}

/* §B3.4 and D4, decided as the reply goes out: the code to send for the GBA's block, and the
 * arbiter's new state. Every reply's arg is 0 (the Flipper offers its slot 0). AABB is the one
 * decrypt of the frame (G4); an AABB while Offered is the GBA's new pick and is decided afresh. */
static uint16_t menu_reply(Gen3Club* club) {
    const uint16_t slot = club->gba_arg;
    switch(menu_code(club->gba_code)) {
    case MenuCodeReady:
        club->refused = slot < GEN3_CLUB_GBA_PARTY ?
                            gen3_trade_check_incoming(club->gba_party[slot], &club->scratch) :
                            Gen3RefuseEmpty;
        /* R8: never a DDDD to an empty partner column */
        if(club->refused != Gen3RefuseNone || !club->party_shown) {
            club->menu = Gen3ClubMenuIdle;
            return GEN3_TRADE_PLAYER_CANCEL;
        }
        club->gba_slot = (uint8_t)slot;
        club->menu = Gen3ClubMenuOffered;
        return GEN3_TRADE_SET_MONS;
    case MenuCodeConfirm:
        /* Save safety: Offered means the GBA's AABB got our DDDD, and no block of ours since */
        if(club->menu != Gen3ClubMenuOffered) break;
        memcpy(club->pending_mon, club->gba_party[club->gba_slot], sizeof(club->pending_mon));
        club->trade_pending = true;
        club->save_syncs = 0; /* no earlier trade's syncs through this one */
        club->sync2_unconfirmed = false;
        club->menu = Gen3ClubMenuStarting;
        return GEN3_TRADE_START;
    case MenuCodeCancel:
        club->menu = Gen3ClubMenuLeaving;
        return GEN3_TRADE_BOTH_CANCEL;
    case MenuCodeConfirmNo:
    case MenuCodeNone: /* never filed: on_menu_command */
        break;
    }
    club->menu = Gen3ClubMenuIdle;
    return GEN3_TRADE_PLAYER_CANCEL;
}

/* §B3.4: true once the reply to the GBA's block may go out (the caller then sends it): an empty
 * queue, and GEN3_CLUB_REPLY_DELAY_BURSTS idle bursts after the one that carried the block's last
 * chunk. */
static bool reply_ready(Gen3Club* club) {
    if(!club->reply_due || gen3_link_queued(&club->link) != 0) return false;
    if(club->idle_bursts < GEN3_CLUB_REPLY_DELAY_BURSTS) {
        club->idle_bursts++; /* the burst built below is idle */
        return false;
    }
    club->reply_due = false;
    return true;
}

/* The one reply: a 20-byte block {code, 0}, 3 commands into an empty queue. */
static void send_reply(Gen3Club* club, uint16_t code) {
    uint8_t block[GEN3_TRADE_BLOCK_SIZE];
    gen3_trade_block_build(code, 0, block);
    gen3_block_send(&club->link, block, sizeof(block), FLIPPER_MULTIPLAYER_ID);
}

static void menu_step(Gen3Club* club) {
    if(club->phase == Gen3ClubPhaseMenu && reply_ready(club)) send_reply(club, menu_reply(club));
}

/* §B4.2-B4.3: the animation runs without link traffic, then the GBA sends ABCD once. Any other
 * block of the GBA's gets no reply. */
static void on_trade_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(gen3_block_rx_feed(&club->block_rx, cmd) != Gen3BlockRxDone) return;
    if(club->block_rx.size != GEN3_TRADE_BLOCK_SIZE) return;
    if(gen3_trade_block_code(club->block_rx.data, NULL) != GEN3_TRADE_ANIM_DONE) return;
    club->reply_due = true;
    club->idle_bursts = 0;
}

/* §B4.3: one DCBA, only after the GBA's ABCD (an early one makes it save the untraded party);
 * then its link save. */
static void trade_step(Gen3Club* club) {
    if(club->phase != Gen3ClubPhaseTrade || !reply_ready(club)) return;
    send_reply(club, GEN3_TRADE_ANIM_ACK);
    club->phase = Gen3ClubPhaseSave;
}

/* §B5.1, §B5.3: the GBA's link save. Each of its 2FFEs gets exactly one 2FFE, at once (it commits
 * 1-2 frames after our sync-2 answer); an extra one would let a later sync finish without us.
 * From our sync-1 answer the GBA may hold a half-written save (save_window, D10). Sync 3 comes
 * only after its commit: the trade is done. */
static void on_save_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(cmd[0] != CMD_READY_EXIT_STANDBY) return;
    /* The queue is empty here; a full one would drop the answer, so the sync isn't counted */
    if(!send_cmd(club, CMD_READY_EXIT_STANDBY, 0)) return;
    if(club->save_syncs >= GEN3_CLUB_SAVE_SYNCS) return;
    club->save_syncs++;
    if(club->save_syncs == 1) club->save_window = true;
    if(club->save_syncs == 2) club->sync2_unconfirmed = true; /* until a burst carries it */
    if(club->save_syncs == GEN3_CLUB_SAVE_SYNCS) end_trade(club, Gen3TradeResultDone);
}

/* D5, §B5.3 (as ruled in fix round 1): the link is lost with a trade pending.
 * - In the trade's own 0x1144 session, by the 2FFEs that reached the GBA: none, Canceled; sync 1
 *   only, RolledBack; sync 2 too, Assumed (it commits 1-2 frames after our answer). A sync-2
 *   answer still unconfirmed counts as not received.
 * - In the 0x1122 session of its CCDD, before its 5FFF close was answered: the GBA can't have
 *   left the trade menu (it closes only after our 5FFF), so the trade never began: Canceled.
 * - Elsewhere (the normal relink between the two) it waits for the GBA's next LinkPlayerBlock
 *   (D3). */
static void on_link_lost(Gen3Club* club) {
    if(!club->trade_pending) return;
    if(club->link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED) {
        if(club->save_syncs == 0) {
            end_trade(club, Gen3TradeResultCanceled);
        } else if(club->save_syncs == 1 || club->sync2_unconfirmed) {
            end_trade(club, Gen3TradeResultRolledBack);
        } else {
            end_trade(club, Gen3TradeResultAssumed);
        }
    } else if(club->link_type == GEN3_LINKTYPE_TRADE_CONNECTING && !club->close_answered) {
        end_trade(club, Gen3TradeResultCanceled);
    }
}

static void on_command(Gen3Club* club, const uint16_t cmd[GEN3_CMD_LEN]) {
    if(cmd[0] == CMD_READY_CLOSE_LINK) {
        /* §A7: always answer with exactly one 5FFF, whatever the phase. */
        if(!club->close_answered) {
            send_cmd(club, CMD_READY_CLOSE_LINK, 0);
            club->close_answered = true;
        }
        club->phase = Gen3ClubPhaseClosing;
        return;
    }

    Gen3BlockRxResult r;
    switch(club->phase) {
    case Gen3ClubPhaseLinked:
        r = gen3_block_rx_feed(&club->block_rx, cmd);
        if(r == Gen3BlockRxDone) {
            on_player_block(club);
        } else if(r == Gen3BlockRxBad) {
            set_error(club, Gen3ClubErrorBadPlayerBlock);
        }
        break;
    case Gen3ClubPhasePlayers:
        if(!club->card_requested) break;
        r = gen3_block_rx_feed(&club->block_rx, cmd);
        if(r == Gen3BlockRxDone) {
            club->phase = Gen3ClubPhaseCards; /* the GBA's card stays in block_rx */
        } else if(r == Gen3BlockRxBad) {
            set_error(club, Gen3ClubErrorBadPlayerBlock);
        }
        break;
    case Gen3ClubPhaseCards:
        /* §A7 entering: zero bursts through the walk-in, then the GBA's standby 2FFE. */
        if(cmd[0] == CMD_READY_EXIT_STANDBY) {
            send_cmd(club, CMD_READY_EXIT_STANDBY, 0);
            club->phase = Gen3ClubPhaseRoom;
        }
        break;
    case Gen3ClubPhaseRoom:
        on_room_command(club, cmd);
        break;
    case Gen3ClubPhaseExchange:
        on_exchange_command(club, cmd);
        break;
    case Gen3ClubPhaseMenu:
        on_menu_command(club, cmd);
        break;
    case Gen3ClubPhaseTrade:
        on_trade_command(club, cmd);
        break;
    case Gen3ClubPhaseSave:
        on_save_command(club, cmd);
        break;
    default: /* Closing: keep bursting until the GBA closes (Disconnected). */
        break;
    }
}

/* §A6 "idle" row: once both player blocks are on the wire (ours goes last), card_delay all-zero
 * bursts, then 0xCCCC type 2 and our trainer card (10 commands into an empty queue). */
static void request_cards(Gen3Club* club) {
    if(club->phase != Gen3ClubPhasePlayers || club->card_requested) return;
    if(gen3_link_queued(&club->link) != 0) return; /* our own block is still going out */
    if(club->idle_bursts < club->card_delay) {
        club->idle_bursts++; /* the burst built below is idle */
        return;
    }
    send_cmd(club, CMD_SEND_BLOCK_REQ, BLOCK_REQ_TRAINER_CARD);
    gen3_block_send(&club->link, club->own_card, sizeof(club->own_card), FLIPPER_MULTIPLAYER_ID);
    club->card_requested = true;
    gen3_block_rx_reset(&club->block_rx);
}

void gen3_club_frame(Gen3Club* club, const MlinkRx* rx, MlinkTx* tx) {
    club->frames++;
    if(rx) {
        uint16_t cmd[GEN3_CMD_LEN];
        const Gen3LinkEvent event = gen3_link_rx(&club->link, rx, cmd);
        /* The last burst was a simultaneous exchange: unless the GBA has gone, it got every word
         * we sent so far, the sync-2 answer too once nothing is left queued */
        if(event != Gen3LinkEventDisconnected && gen3_link_queued(&club->link) == 0) {
            club->sync2_unconfirmed = false;
        }
        switch(event) {
        case Gen3LinkEventNone:
            break;
        case Gen3LinkEventConnected:
            on_connected(club);
            break;
        case Gen3LinkEventCommand:
            on_command(club, cmd);
            break;
        case Gen3LinkEventChecksumError:
            /* Transfer #1 carries each side's checksum and is not summed (§A2). A glitch on G's
             * word fails only our check: G is healthy and still expects every answer, so keep
             * the phase and the queue. If G's check failed too, G closes the link and its slot
             * reads 0xFFFF: Disconnected resets us. An Error phase keeps the error it shows. */
            if(club->phase != Gen3ClubPhaseError) club->last_error = Gen3ClubErrorChecksum;
            if(is_command(cmd)) on_command(club, cmd); /* e.g. G's one 2FFE or CAFE 0017 */
            break;
        case Gen3LinkEventDisconnected:
            on_link_lost(club);
            reset_session(club);
            club->phase = Gen3ClubPhaseWaiting; /* last_error is kept */
            break;
        }
    }
    if(club->phase == Gen3ClubPhaseRoom) club->room_frames++;
    request_cards(club);
    try_sit(club);
    exchange_step(club);
    menu_step(club);
    trade_step(club);
    gen3_link_tx(&club->link, tx);
}

void gen3_club_status(const Gen3Club* club, Gen3ClubStatus* out) {
    memset(out, 0, sizeof(*out));
    out->phase = club->phase;
    out->last_error = club->last_error;
    out->partner_valid = club->partner_valid;
    out->partner = club->partner;
    out->frames = club->frames;
    out->room_frames = club->room_frames;
    gen3_link_stats(&club->link, &out->link);
    out->link_type = club->link_type;
    out->trainer_valid = club->trainer_valid;
    out->trainer = club->trainer;
    out->has_offer = club->has_offer;
    out->gba_seated = club->gba_seated;
    out->flipper_seated = club->flipper_seated;
    out->trades_done = club->trades_done;
    out->round = club->round;
    out->menu = club->menu;
    out->gba_slot = club->gba_slot;
    out->refused = club->refused;
    out->trade_pending = club->trade_pending;
    out->save_syncs = club->save_syncs;
    out->save_window = club->save_window;
    out->last_result = club->last_result;
}

bool gen3_club_set_offer(Gen3Club* club, const uint8_t* game, uint32_t trades_seen) {
    if(trades_seen != club->trades_done) return false;
    if(game) {
        memcpy(club->offer, game, sizeof(club->offer));
    } else {
        memset(club->offer, 0, sizeof(club->offer));
    }
    club->has_offer = game != NULL;
    return true;
}

void gen3_club_set_card_delay(Gen3Club* club, uint8_t bursts) {
    club->card_delay = bursts;
}

void gen3_club_set_round_delays(Gen3Club* club, uint8_t first, uint8_t later) {
    club->round1_delay = first;
    club->round_delay = later;
}

void gen3_club_gba_mon(const Gen3Club* club, uint8_t slot, uint8_t out[GEN3_PARTY_SIZE]) {
    if(slot < GEN3_CLUB_GBA_PARTY) {
        memcpy(out, club->gba_party[slot], GEN3_PARTY_SIZE);
    } else {
        memset(out, 0, GEN3_PARTY_SIZE);
    }
}

void gen3_club_set_national_dex(Gen3Club* club, bool on) {
    club->national_dex = on;
}

void gen3_club_get_received(
    const Gen3Club* club,
    uint8_t out[GEN3_PARTY_SIZE],
    uint32_t* trades_done) {
    memcpy(out, club->received, GEN3_PARTY_SIZE);
    if(trades_done) *trades_done = club->trades_done;
}
