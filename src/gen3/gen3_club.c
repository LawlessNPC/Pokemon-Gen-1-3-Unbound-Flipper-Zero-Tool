#include <src/gen3/gen3_club.h>

#include <src/gen3/gen3_text.h>

#include <string.h>

#define CMD_SEND_LINK_TYPE     0x2222
#define CMD_READY_EXIT_STANDBY 0x2FFE
#define CMD_READY_CLOSE_LINK   0x5FFF
#define CMD_SEND_HELD_KEYS     0xCAFE
#define CMD_SEND_BLOCK_REQ     0xCCCC

#define KEY_CODE_EXIT_ROOM 0x17

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

static void reset_session(Gen3Club* club) {
    gen3_block_rx_reset(&club->block_rx);
    club->partner_valid = false;
    memset(&club->partner, 0, sizeof(club->partner));
    club->card_requested = false;
    club->close_answered = false;
    club->exit_answer_due = false;
    club->idle_bursts = 0;
}

void gen3_club_init(Gen3Club* club, const Gen3ClubConfig* cfg) {
    memset(club, 0, sizeof(*club));
    gen3_link_init(&club->link);

    Gen3LinkPlayer own;
    memset(&own, 0, sizeof(own));
    own.version = GEN3_VERSION_RUBY;
    own.lp_field_2 = 0;
    own.trainer_id = ((uint32_t)cfg->sid << 16) | cfg->tid;
    gen3_text_encode(cfg->name ? cfg->name : "", own.name, sizeof(own.name), NAME_MAX_CHARS);
    own.gender = cfg->gender ? 1 : 0; /* indexes the GBA's avatar table: 0 or 1 only (§A5) */
    own.link_type = GEN3_LINKTYPE_TRADE_SETUP;
    own.id = 0;
    own.language = GEN3_LANGUAGE_ENGLISH;
    gen3_link_player_block_build(&own, club->own_player_block);
    gen3_trainer_card_build(&own, club->own_card);

    club->card_delay = GEN3_CLUB_CARD_DELAY_BURSTS;
    club->phase = Gen3ClubPhaseWaiting;
    club->last_error = Gen3ClubErrorNone;
}

/* §A6 B1-B7: {2222,1133}, then our LinkPlayerBlock; both fit an empty queue (7 of 24). */
static void on_connected(Gen3Club* club) {
    reset_session(club);
    club->phase = Gen3ClubPhaseLinked;
    send_cmd(club, CMD_SEND_LINK_TYPE, GEN3_LINKTYPE_TRADE_SETUP);
    gen3_block_send(
        &club->link,
        club->own_player_block,
        sizeof(club->own_player_block),
        FLIPPER_MULTIPLAYER_ID);
}

static void on_player_block(Gen3Club* club) {
    Gen3LinkPlayer lp;
    if(club->block_rx.size != GEN3_LINK_PLAYER_BLOCK_SIZE ||
       !gen3_link_player_block_parse(club->block_rx.data, &lp)) {
        set_error(club, Gen3ClubErrorBadPlayerBlock);
    } else if(lp.link_type != GEN3_LINKTYPE_TRADE_SETUP) {
        /* Keep 0x1133: the GBA reports "different selections" and sends 5FFF (§A6). */
        set_error(club, Gen3ClubErrorOtherRoom);
    } else {
        club->partner = lp;
        club->partner_valid = true;
        club->phase = Gen3ClubPhasePlayers;
        club->idle_bursts = 0;
    }
    gen3_block_rx_reset(&club->block_rx);
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
    } else if(cmd[0] == CMD_SEND_HELD_KEYS && cmd[1] == KEY_CODE_EXIT_ROOM) {
        club->exit_answer_due = true;
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
    default: /* Closing: keep bursting until the GBA closes (Disconnected) */
        break;
    }
}

/* §A6 "idle" row: once both player blocks are on the wire, card_delay all-zero bursts, then
 * 0xCCCC type 2 and our trainer card (10 commands into an empty queue). */
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
}

void gen3_club_set_card_delay(Gen3Club* club, uint8_t bursts) {
    club->card_delay = bursts;
}
