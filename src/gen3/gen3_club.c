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

static void send_cmd(Gen3Club* club, uint16_t w0, uint16_t w1) {
    uint16_t cmd[GEN3_CMD_LEN] = {0};
    cmd[0] = w0;
    cmd[1] = w1;
    gen3_link_send(&club->link, cmd);
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
 * (§B1.6: a mismatch hangs the GBA silently, before anything is written). Otherwise 0x1133: a
 * Link Test never trades, and the 0x1144 and 0x1111 relinks come with Tasks 6 and 7. */
static uint32_t own_link_type(const Gen3Club* club, uint32_t gba_link_type) {
    if(club->cfg.trading && gba_link_type == GEN3_LINKTYPE_TRADE_CONNECTING) return gba_link_type;
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
        &club->cfg, &club->partner, link_type, /* national_dex */ false, &lp, club->own_card);
    gen3_link_player_block_build(&lp, club->own_player_block);
    gen3_block_send(
        &club->link,
        club->own_player_block,
        sizeof(club->own_player_block),
        FLIPPER_MULTIPLAYER_ID);
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
            /* §B1.6: the trade menu. The party exchange starts after both blocks (Task 4). */
            club->partner_valid = true;
            club->phase = Gen3ClubPhaseExchange;
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
    send_cmd(club, CMD_SEND_HELD_KEYS, KEY_CODE_READY);
    club->flipper_seated = true;
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
    default: /* Exchange: idle until Task 4's rounds. Closing: keep bursting until the GBA closes
                (Disconnected). */
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
        switch(gen3_link_rx(&club->link, rx, cmd)) {
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
            reset_session(club);
            club->phase = Gen3ClubPhaseWaiting; /* last_error is kept */
            break;
        }
    }
    if(club->phase == Gen3ClubPhaseRoom) club->room_frames++;
    request_cards(club);
    try_sit(club);
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
