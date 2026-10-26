#include "fake_ruby.h"

#include <string.h>

/* Citations are pokeruby names, as in docs/gen3/link-protocol.md. */

#define CMD_SEND_LINK_TYPE     0x2222
#define CMD_READY_EXIT_STANDBY 0x2FFE
#define CMD_READY_CLOSE_LINK   0x5FFF
#define CMD_CONT_BLOCK         0x8888
#define CMD_INIT_BLOCK         0xBBBB
#define CMD_SEND_HELD_KEYS     0xCAFE
#define CMD_SEND_BLOCK_REQ     0xCCCC
#define CMD_NONE               0xEFFF

#define MASTER_HANDSHAKE 0x8FFF
#define SLAVE_HANDSHAKE  0xB9A0

#define LOCAL_ID 1 /* multiplayer id of the child */

/* Room key codes (§A7 "Room command format") */
#define KEY_CODE_EMPTY      0x11
#define KEY_CODE_DPAD_DOWN  0x12
#define KEY_CODE_READY      0x16
#define KEY_CODE_EXIT_ROOM  0x17
#define KEY_CODE_START      0x18
#define KEY_CODE_IDLE       0x1A
#define KEY_CODE_RECV_QUEUE 0x1B
#define KEY_CODE_SEND_QUEUE 0x1C
#define KEY_CODE_EXIT_SEAT  0x1D
#define KEY_CODE_END        0x1E /* sub_8055280 keeps codes 0x11..0x1D, else sends 0x11 */

/* Player link states (gUnknown_03000580) */
#define PLAYER_IDLE    0x80
#define PLAYER_BUSY    0x81
#define PLAYER_READY   0x82
#define PLAYER_EXITING 0x83

#define TRIGGER_HANDSHAKE_RUNS 5 /* Task_TriggerHandshake */
#define LAG_VBLANKS            10 /* LinkVSync: > 10 VBlanks without a serial IRQ */
#define CARD_READY_BURSTS      3 /* own card ready this many bursts after both player blocks */
#define WALK_IN_FRAMES         30 /* zero-command frames between the cards and 0x2FFE */
#define STANDBY_TIMEOUT_FRAMES 600 /* test limit; Ruby itself waits forever */
/* Script pacing in game updates; the player's reading time, not protocol values. */
#define PROMPT_UPDATES    60 /* "Terminate the link?" until the player picks YES */
#define TERMINATE_UPDATES 60 /* "Terminating the link..." until ExitLinkRoom */
#define EXIT_FADE_UPDATES 30 /* FadeScreen and music fade-out before SetCloseLinkCallback */
#define CORRUPT_BIT       0x0001

/* sBlockRequestLookupTable sizes, request types 0..4 */
static const uint16_t block_request_size[] = {200, 200, 100, 220, 40};

/* sMagic: an ASCII C string, compared with strcmp (15 bytes including the NUL) */
static const char link_magic[] = "GameFreak inc.";
#define LP_OFF_MAGIC2 0x2C

static void set_error(FakeRuby* g, const char* reason) {
    if(!g->error) g->error = reason;
    g->serial_on = false;
    g->link_open = false;
    g->state = FakeRubyLinkStart1;
}

/* gLink and the link.c statics, as zeroed by EnableSerial/DisableSerial. */
static void link_reset(FakeRuby* g) {
    g->state = FakeRubyLinkStart0;
    g->sio_send = 0;
    g->irq_counter = 0;
    g->vblanks_without_irq = 0;
    g->hs_prev_count = 0;
    g->player_count = 0;
    g->send_index = 0;
    g->recv_index = 0;
    g->checksum = 0;
    g->checksum_available = false;
    g->bad_checksum = false;
    g->lag = false;
    g->recv_nonzero = false;
    g->send_empty = false;
    g->received_nothing = false;
    g->send_pos = 0;
    g->send_count = 0;
    g->recv_pos = 0;
    g->recv_count = 0;
    memset(g->send_queue, 0, sizeof(g->send_queue));
    memset(g->recv_queue, 0, sizeof(g->recv_queue));
}

static void enable_serial(FakeRuby* g) {
    link_reset(g);
    g->serial_on = true;
}

static void disable_serial(FakeRuby* g) {
    link_reset(g);
    g->serial_on = false;
}

/* ---- serial ISR (SerialCB), one call per transfer ---- */

static bool is_handshake_word(uint16_t w) {
    return (w & ~3u) == SLAVE_HANDSHAKE || w == MASTER_HANDSHAKE;
}

/* DoHandshake: write the next word, count players over slots 0..3, success rule. */
static bool do_handshake(FakeRuby* g, const uint16_t slots[4]) {
    g->sio_send = SLAVE_HANDSHAKE; /* handshakeAsMaster is never set on a child */
    uint8_t count = 0;
    for(int i = 0; i < 4; i++) {
        if(is_handshake_word(slots[i])) {
            count++;
        } else {
            if(slots[i] != MLINK_NO_DATA) count = 0;
            break;
        }
    }
    g->player_count = count;
    if(count > 1 && count == g->hs_prev_count && slots[0] == MASTER_HANDSHAKE) return true;
    g->hs_prev_count = count;
    return false;
}

/* DoRecv: transfer #1 checks the previous burst's checksum, #2..#9 fill the receive queue. */
static void do_recv(FakeRuby* g, const uint16_t slots[4]) {
    if(g->send_index == 0) {
        for(uint8_t i = 0; i < g->player_count; i++) {
            if(g->checksum != slots[i] && g->checksum_available) g->bad_checksum = true;
        }
        g->checksum = 0;
        g->checksum_available = true;
        return;
    }
    uint8_t index = (uint8_t)((g->recv_pos + g->recv_count) % FAKE_RUBY_QUEUE_LEN);
    if(g->recv_count < FAKE_RUBY_QUEUE_LEN) {
        /* A full queue is not summed, so it surfaces as a checksum error (§A2). */
        for(uint8_t i = 0; i < g->player_count && i < FAKE_RUBY_PLAYERS; i++) {
            g->checksum = (uint16_t)(g->checksum + slots[i]);
            if(slots[i] != 0) g->recv_nonzero = true;
            g->recv_queue[index][i][g->recv_index] = slots[i];
        }
    }
    g->recv_index++;
    if(g->recv_index == FAKE_RUBY_CMD_LEN && g->recv_nonzero) {
        g->recv_count++;
        g->recv_nonzero = false;
    }
}

/* DoSend: the word for the next transfer; after #9 the checksum, and dequeue. */
static void do_send(FakeRuby* g) {
    if(g->send_index == FAKE_RUBY_CMD_LEN) {
        g->sio_send = g->checksum;
        if(!g->send_empty) {
            g->send_count--;
            g->send_pos = (uint8_t)((g->send_pos + 1) % FAKE_RUBY_QUEUE_LEN);
        } else {
            g->send_empty = false;
        }
        return;
    }
    if(!g->send_empty && g->send_count == 0) g->send_empty = true;
    g->sio_send = g->send_empty ? 0 : g->send_queue[g->send_pos][g->send_index];
    g->send_index++;
}

static void serial_irq(FakeRuby* g, uint16_t master_word, uint16_t own_word) {
    const uint16_t slots[4] = {master_word, own_word, MLINK_NO_DATA, MLINK_NO_DATA};
    if(g->state == FakeRubyLinkConnected) {
        do_recv(g, slots);
        do_send(g);
        if(g->recv_index == FAKE_RUBY_CMD_LEN) { /* SendRecvDone */
            g->send_index = 0;
            g->recv_index = 0;
        }
    } else if(g->state == FakeRubyLinkHandshake) {
        if(do_handshake(g, slots)) {
            /* Child: straight to CONN_ESTABLISHED; send/recv indexes are still 0, so the
             * next transfer is burst #1's checksum slot. */
            g->state = FakeRubyLinkConnected;
            g->phase = FakeRubyLinkup;
        }
    }
    g->irq_counter++;
    g->vblanks_without_irq = 0;
    if(g->irq_counter == 8) g->last_recv_count = g->recv_count;
}

/* LinkVSync, child branch. */
static void link_vsync(FakeRuby* g) {
    if(g->state != FakeRubyLinkHandshake && g->state != FakeRubyLinkConnected) return;
    if(g->vblanks_without_irq < UINT8_MAX) g->vblanks_without_irq++;
    if(g->vblanks_without_irq > LAG_VBLANKS) {
        if(g->state == FakeRubyLinkConnected) {
            g->lag = true;
        } else {
            g->player_count = 0;
        }
    }
}

/* ---- main loop: LinkMain1, LinkMain2 (ProcessRecvCmds, gLinkCallback), tasks ---- */

static void close_link(FakeRuby* g) {
    g->received_link_players = false;
    g->link_open = false;
    disable_serial(g);
}

static void enqueue_send_cmd(FakeRuby* g) {
    bool nonzero = false;
    /* A full queue would be error 0x4000; unreachable here: one enqueue per frame against one
     * dequeue per burst, and a stalled master trips the lag or checksum check first. */
    if(g->send_count < FAKE_RUBY_QUEUE_LEN) {
        uint8_t off = (uint8_t)((g->send_pos + g->send_count) % FAKE_RUBY_QUEUE_LEN);
        for(int i = 0; i < FAKE_RUBY_CMD_LEN; i++) {
            if(g->send_cmd[i] != 0) nonzero = true;
            g->send_queue[off][i] = g->send_cmd[i];
        }
    }
    memset(g->send_cmd, 0, sizeof(g->send_cmd));
    if(nonzero) g->send_count++;
}

static void dequeue_recv_cmds(FakeRuby* g) {
    g->received_nothing = g->recv_count == 0;
    if(g->recv_count == 0) {
        memset(g->recv_cmds, 0, sizeof(g->recv_cmds));
        return;
    }
    memcpy(g->recv_cmds, g->recv_queue[g->recv_pos], sizeof(g->recv_cmds));
    g->recv_count--;
    g->recv_pos = (uint8_t)((g->recv_pos + 1) % FAKE_RUBY_QUEUE_LEN);
}

static void link_main1(FakeRuby* g) {
    switch(g->state) {
    case FakeRubyLinkStart0:
        disable_serial(g);
        g->state = FakeRubyLinkStart1;
        break;
    case FakeRubyLinkStart1:
        if(g->should_advance) {
            enable_serial(g);
            g->state = FakeRubyLinkHandshake;
        }
        break;
    case FakeRubyLinkHandshake:
        break;
    case FakeRubyLinkConnected:
        enqueue_send_cmd(g);
        dequeue_recv_cmds(g);
        break;
    }
    g->should_advance = false;
}

/* InitBlockSend: refused only while a block is active. Otherwise it replaces gLinkCallback,
 * whatever was running (standby, close, held keys...). The gLinkCallback != NULL check belongs
 * to the master's SendBlockRequest, not to the receiver of 0x2222/0xCCCC (§A4). */
static bool init_block_send(FakeRuby* g, const uint8_t* src, uint16_t size) {
    if(g->block_send_active) return false;
    g->block_send_active = true;
    g->block_send_size = size;
    g->block_send_pos = 0;
    if(src != g->block_send_buffer) memcpy(g->block_send_buffer, src, size);
    g->send_cmd[0] = CMD_INIT_BLOCK;
    g->send_cmd[1] = size;
    g->send_cmd[2] = 0x80 + LOCAL_ID;
    g->callback = FakeRubyCbBlockSendBegin;
    g->block_send_delay = 0;
    return true;
}

static void send_link_player_block(FakeRuby* g) {
    uint8_t block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&g->me, block); /* InitLocalLinkPlayer */
    init_block_send(g, block, sizeof(block));
}

/* HandleReceiveRemoteLinkPlayer */
static void receive_link_player(FakeRuby* g, uint8_t slot) {
    g->link_player_pending[slot] = false;
    int pending = 0;
    for(uint8_t i = 0; i < g->player_count && i < FAKE_RUBY_PLAYERS; i++) {
        pending += g->link_player_pending[i];
    }
    if(pending == 0) g->received_link_players = true;
}

static void cont_block(FakeRuby* g, uint8_t slot, const uint16_t* cmd) {
    uint8_t* buf = g->block_recv_buffer[slot];
    uint16_t pos = g->block_recv_pos[slot];
    for(int j = 0; j < 7; j++) {
        size_t off = (size_t)pos + 2 * j;
        if(off + 1 < FAKE_RUBY_BLOCK_MAX) {
            buf[off] = (uint8_t)(cmd[1 + j] & 0xFF);
            buf[off + 1] = (uint8_t)(cmd[1 + j] >> 8);
        }
    }
    g->block_recv_pos[slot] = (uint16_t)(pos + 14);
    if(g->block_recv_pos[slot] < g->block_recv_size[slot]) return;

    if(!g->link_player_pending[slot]) {
        g->block_received |= (uint8_t)(1u << slot); /* SetBlockReceivedFlag */
        return;
    }
    /* First block from this slot after OpenLink: always a LinkPlayerBlock. */
    if(memcmp(buf, link_magic, sizeof(link_magic)) != 0 ||
       memcmp(buf + LP_OFF_MAGIC2, link_magic, sizeof(link_magic)) != 0) {
        set_error(g, "magic"); /* CB2_LinkError */
        return;
    }
    gen3_link_player_block_parse(buf, &g->link_players[slot]);
    receive_link_player(g, slot);
}

static void block_request(FakeRuby* g, uint16_t type) {
    if(!g->card_ready) {
        set_error(g, "stale card"); /* would send stale gBlockSendBuffer bytes (U3) */
        return;
    }
    /* Ruby indexes the table without a bounds check; the club never sends type >= 5. */
    if(type >= sizeof(block_request_size) / sizeof(block_request_size[0])) return;
    init_block_send(g, g->block_send_buffer, block_request_size[type]);
}

static void process_recv_cmds(FakeRuby* g) {
    for(uint8_t i = 0; i < FAKE_RUBY_PLAYERS; i++) {
        const uint16_t* cmd = g->recv_cmds[i];
        g->held_keys[i] = 0; /* a key code lasts one game update */
        switch(cmd[0]) {
        case CMD_SEND_LINK_TYPE:
            send_link_player_block(g);
            break;
        case CMD_READY_EXIT_STANDBY:
            g->ready_standby[i] = true;
            break;
        case CMD_READY_CLOSE_LINK:
            g->ready_close[i] = true;
            break;
        case CMD_INIT_BLOCK:
            g->block_recv_pos[i] = 0;
            g->block_recv_size[i] = cmd[1];
            break;
        case CMD_CONT_BLOCK:
            cont_block(g, i, cmd);
            break;
        case CMD_SEND_BLOCK_REQ:
            block_request(g, cmd[1]);
            break;
        case CMD_SEND_HELD_KEYS:
            g->held_keys[i] = cmd[1];
            break;
        default:
            break;
        }
        if(g->error) return;
    }
}

static void block_send_chunk(FakeRuby* g) {
    const uint8_t* src = g->block_send_buffer;
    g->send_cmd[0] = CMD_CONT_BLOCK;
    for(int i = 0; i < 7; i++) {
        size_t off = (size_t)g->block_send_pos + 2 * i;
        uint16_t lo = off < FAKE_RUBY_BLOCK_MAX ? src[off] : 0;
        uint16_t hi = off + 1 < FAKE_RUBY_BLOCK_MAX ? src[off + 1] : 0;
        g->send_cmd[1 + i] = (uint16_t)(lo | (hi << 8));
    }
    g->block_send_pos += 14;
    if(g->block_send_size <= g->block_send_pos) {
        g->block_send_active = false;
        g->callback = FakeRubyCbBlockSendEnd;
    }
}

static void run_callback(FakeRuby* g) {
    switch(g->callback) {
    case FakeRubyCbNone:
        break;
    case FakeRubyCbRequestPlayerData:
        g->callback = FakeRubyCbNone; /* only a master builds 0x2222 */
        break;
    case FakeRubyCbBlockSendBegin:
        if(++g->block_send_delay > 2) g->callback = FakeRubyCbBlockSend;
        break;
    case FakeRubyCbBlockSend:
        block_send_chunk(g);
        break;
    case FakeRubyCbBlockSendEnd:
        g->callback = FakeRubyCbNone;
        break;
    case FakeRubyCbReadyCloseLink:
        if(g->last_recv_count == 0) {
            g->send_cmd[0] = CMD_READY_CLOSE_LINK;
            g->callback = FakeRubyCbWaitCloseLink;
        }
        break;
    case FakeRubyCbWaitCloseLink:
        if(g->ready_close[0] && g->ready_close[1]) {
            close_link(g);
            g->callback = FakeRubyCbNone;
            g->phase = FakeRubyClosed;
        }
        break;
    case FakeRubyCbStandby:
        if(g->last_recv_count == 0) {
            g->send_cmd[0] = CMD_READY_EXIT_STANDBY;
            g->callback = FakeRubyCbStandbyForAll;
            g->standby_frame = g->frame;
        }
        break;
    case FakeRubyCbStandbyForAll:
        if(g->ready_standby[0] && g->ready_standby[1]) {
            memset(g->ready_standby, 0, sizeof(g->ready_standby));
            g->callback = FakeRubyCbNone;
        }
        break;
    case FakeRubyCbSendHeldKeys:
        /* BuildSendCmd(0xCAFE), every frame, even when the game update is skipped */
        if(g->received_link_players && g->held_key_to_send != 0) {
            g->send_cmd[0] = CMD_SEND_HELD_KEYS;
            g->send_cmd[1] = g->held_key_to_send;
        }
        break;
    }
}

static void link_main2(FakeRuby* g) {
    if(!g->link_open) return;
    memset(g->send_cmd, 0, sizeof(g->send_cmd));
    if(g->state != FakeRubyLinkConnected) return;
    process_recv_cmds(g);
    if(g->error) return;
    run_callback(g);
    if(!g->link_open) return; /* closed by WaitCloseLink this frame */
    /* CheckErrorStatus */
    if(g->bad_checksum) {
        set_error(g, "checksum");
    } else if(g->lag) {
        set_error(g, "lag");
    }
}

static void next_task(FakeRuby* g, FakeRubyTask task) {
    g->task = task;
    g->task_frame = g->frame;
}

/* ---- Trade Center room: overworld.c, field_fadetransition.c (§A7, §B7) ---- */

static bool any_player_in(const FakeRuby* g, uint8_t state) { /* sub_8054FC0 */
    for(int i = 0; i < FAKE_RUBY_PLAYERS; i++) {
        if(g->link_state[i] == state) return true;
    }
    return false;
}

static bool all_players_in(const FakeRuby* g, uint8_t state) { /* sub_8054F88 */
    for(int i = 0; i < FAKE_RUBY_PLAYERS; i++) {
        if(g->link_state[i] != state) return false;
    }
    return true;
}

/* sub_8055824 (exit prompt) / sub_805585C (forced exit): the script takes over the local player.
 * Neither script unlocks the controls before ExitLinkRoom replaces the key intercept. */
static void start_leave_script(FakeRuby* g, FakeRubyScript script, uint16_t updates) {
    g->key_cb = FakeRubyKeyDeferToEventScript;
    g->controls_locked = true;
    g->script = script;
    g->script_updates = updates;
    g->phase = FakeRubyLeaving;
}

/* sub_8054FF8 [HandleLinkPlayerKeyInput], per slot per game update. While anyone is
 * EXITING_ROOM, an IDLE player is made BUSY and that update's key is dropped (early return), so
 * only a player that is already not IDLE takes 0x17. A partner's 0x17 processed in the update
 * right after our own echoed 0x17 is lost, and we then wait forever for all EXITING_ROOM. */
static void handle_link_player_key(FakeRuby* g, uint8_t slot, uint16_t key) {
    bool local = slot == LOCAL_ID;
    uint8_t* state = &g->link_state[slot];
    if(*state == PLAYER_IDLE) {
        /* No coord-event script (sub_8055648): no avatar ever takes a step in this model. */
        if(any_player_in(g, PLAYER_EXITING)) {
            *state = PLAYER_BUSY;
            if(local) start_leave_script(g, FakeRubyScriptTerminate, TERMINATE_UPDATES);
            return;
        }
        switch(key) {
        case KEY_CODE_START:
        case KEY_CODE_RECV_QUEUE:
        case KEY_CODE_SEND_QUEUE:
            *state = PLAYER_BUSY; /* the local player never sends these in this model */
            break;
        case KEY_CODE_DPAD_DOWN: /* the exit prompt needs the south exit, facing south */
            if(local && g->facing_exit) {
                *state = PLAYER_BUSY;
                start_leave_script(g, FakeRubyScriptPrompt, PROMPT_UPDATES);
            }
            break;
        default: /* 0x19 (A) needs map data and is not modelled */
            break;
        }
    }
    switch(key) {
    case KEY_CODE_EXIT_ROOM:
        *state = PLAYER_EXITING;
        break;
    case KEY_CODE_READY:
        *state = PLAYER_READY;
        break;
    case KEY_CODE_IDLE:
        *state = PLAYER_IDLE;
        if(local) g->key_cb = FakeRubyKeySelfIdle;
        break;
    case KEY_CODE_EXIT_SEAT:
        if(*state == PLAYER_READY) *state = PLAYER_BUSY;
        break;
    default:
        break;
    }
}

/* gUnknown_03000584: this GBA's next key code, after every player's key was handled. */
static uint16_t key_intercept(FakeRuby* g) {
    switch(g->key_cb) {
    case FakeRubyKeySelfIdle:
        /* 0x1B/0x1C flow control needs a queue above 4; one burst per frame keeps both <= 1. */
        if(g->controls_locked) return KEY_CODE_EMPTY;
        return g->facing_exit ? KEY_CODE_DPAD_DOWN : KEY_CODE_EMPTY;
    case FakeRubyKeySendExitRoomKey:
        g->key_cb = FakeRubyKeyWaitForPlayersToExit;
        return KEY_CODE_EXIT_ROOM;
    case FakeRubyKeyWaitForPlayersToExit:
        if(all_players_in(g, PLAYER_EXITING)) {
            g->script = FakeRubyScriptExitStart; /* ScriptContext_SetupScript(gUnknown_081A4508) */
            g->key_cb = FakeRubyKeySendNothing;
        }
        return KEY_CODE_EMPTY;
    case FakeRubyKeyDeferToEventScript: /* controls stay locked: see start_leave_script */
    case FakeRubyKeySendNothing:
        return KEY_CODE_EMPTY;
    }
    return KEY_CODE_EMPTY;
}

/* Callback2: the running script, then sub_8081050 [Task_ReturnFromLinkRoom]. */
static void run_room_script(FakeRuby* g) {
    switch(g->script) {
    case FakeRubyScriptNone:
    case FakeRubyScriptExitWait: /* WaitCloseLink closes the link; then the warp to PC 2F */
        break;
    case FakeRubyScriptPrompt:
        if(--g->script_updates == 0) { /* YES: goto TradeRoom_TerminateLink */
            g->script = FakeRubyScriptTerminate;
            g->script_updates = TERMINATE_UPDATES;
        }
        break;
    case FakeRubyScriptTerminate:
        if(--g->script_updates == 0) {
            g->key_cb = FakeRubyKeySendExitRoomKey; /* special ExitLinkRoom -> sub_805559C */
            g->script = FakeRubyScriptNone;
        }
        break;
    case FakeRubyScriptExitStart:
        g->callback = FakeRubyCbNone; /* ClearLinkCallback_2: no more CAFE */
        g->script = FakeRubyScriptExitFade;
        g->script_updates = EXIT_FADE_UPDATES;
        break;
    case FakeRubyScriptExitFade:
        if(--g->script_updates == 0) {
            if(g->callback == FakeRubyCbNone) g->callback = FakeRubyCbReadyCloseLink;
            g->script = FakeRubyScriptExitWait;
        }
        break;
    }
}

/* One game update in the room: callback1 (sub_8055354 [CB1_OverworldLink]: every player's key
 * in lockstep, own echo included, then sub_8055280 keeps 0x11..0x1D), then callback2. */
static void room_update(FakeRuby* g) {
    for(uint8_t i = 0; i < FAKE_RUBY_PLAYERS; i++) {
        handle_link_player_key(g, i, g->held_keys[i]);
    }
    uint16_t key = key_intercept(g);
    g->held_key_to_send = (key >= KEY_CODE_EMPTY && key < KEY_CODE_END) ? key : KEY_CODE_EMPTY;
    run_room_script(g);
}

/* cable_club.c (child side of the Trade Center linkup) and the counter script. */
static void run_linkup_task(FakeRuby* g) {
    switch(g->task) {
    case FakeRubyTaskNone:
        break;
    case FakeRubyTaskRoom:
        room_update(g);
        break;
    case FakeRubyTaskExchange:
        if(!g->received_link_players) break;
        /* GetLinkPlayerDataExchangeStatusTimed: every linkType must equal slot 0's. */
        if(g->link_players[0].link_type != g->link_players[1].link_type) {
            if(g->callback == FakeRubyCbNone) g->callback = FakeRubyCbReadyCloseLink;
            next_task(g, FakeRubyTaskStop);
            break;
        }
        next_task(g, FakeRubyTaskMakeCard);
        break;
    case FakeRubyTaskMakeCard:
        /* Generated in the main loop of frame P+2 (P = blocks complete), after that frame's
         * ProcessRecvCmds: a 0xCCCC processed in burst P+3 or later gets the real card. */
        if(g->frame - g->task_frame + 1 < CARD_READY_BURSTS) break;
        gen3_trainer_card_build(&g->me, g->block_send_buffer);
        g->card_ready = true;
        next_task(g, FakeRubyTaskAwaitCards);
        break;
    case FakeRubyTaskAwaitCards:
        if(g->block_received != 0x3) break; /* Flipper's card and our own echo */
        g->block_received = 0; /* ResetBlockReceivedFlags */
        g->phase = FakeRubyWalkIn;
        next_task(g, FakeRubyTaskWalkIn);
        break;
    case FakeRubyTaskWalkIn:
        if(g->frame - g->task_frame < WALK_IN_FRAMES) break;
        g->callback = FakeRubyCbStandby; /* SetLinkStandbyCallback */
        next_task(g, FakeRubyTaskStandby);
        break;
    case FakeRubyTaskStandby:
        if(g->callback == FakeRubyCbStandbyForAll && !g->ready_standby[0] &&
           g->frame - g->standby_frame >= STANDBY_TIMEOUT_FRAMES) {
            set_error(g, "standby timeout");
            break;
        }
        /* IsLinkTaskFinished: gLinkCallback == NULL. A stray block that replaced the standby
         * callback also ends this wait once it is sent, as on Ruby. */
        if(g->callback != FakeRubyCbNone) break;
        g->callback = FakeRubyCbSendHeldKeys; /* StartSendingKeysToLink */
        /* sub_8054F70 at the map load; CB1 ran through the walk-in with the controls locked
         * (0x11), and sub_8080A5C unlocks them now. */
        memset(g->link_state, PLAYER_IDLE, sizeof(g->link_state));
        g->key_cb = FakeRubyKeySelfIdle;
        g->held_key_to_send = KEY_CODE_EMPTY;
        g->controls_locked = false;
        g->phase = FakeRubyRoom;
        next_task(g, FakeRubyTaskRoom);
        break;
    case FakeRubyTaskStop:
        break;
    }
}

static void run_tasks(FakeRuby* g) {
    if(g->trigger_active && ++g->trigger_runs == TRIGGER_HANDSHAKE_RUNS) {
        g->should_advance = true;
        g->trigger_active = false;
    }
    if(g->link_open) run_linkup_task(g);
}

static void main_loop(FakeRuby* g) {
    g->frame++;
    link_main1(g);
    link_main2(g);
    if(g->error) return;
    /* UpdateLinkAndCallCallbacks: while sending keys, a frame whose dequeue found nothing skips
     * the whole game update (lockstep, §A7). LinkMain2 above still built this frame's CAFE. */
    if(g->received_nothing && g->callback == FakeRubyCbSendHeldKeys) return;
    run_tasks(g);
}

/* ---- API ---- */

void fake_ruby_init(FakeRuby* g, const Gen3LinkPlayer* me) {
    memset(g, 0, sizeof(*g));
    g->me = *me;
    g->powered = true;
    g->phase = FakeRubyOff;
    g->state = FakeRubyLinkStart1; /* link closed: serial off */
}

void fake_ruby_open_link(FakeRuby* g) {
    if(!g->powered || g->error) return;
    /* OpenLink: ResetSerial, InitLink (EnableSerial, gSendCmd = 0xEFFF filler) */
    enable_serial(g);
    for(int i = 0; i < FAKE_RUBY_CMD_LEN; i++) g->send_cmd[i] = CMD_NONE;
    g->link_open = true;
    g->callback = FakeRubyCbRequestPlayerData;
    g->block_send_active = false;
    g->block_received = 0;
    g->received_link_players = false;
    memset(g->link_players, 0, sizeof(g->link_players));
    memset(g->block_recv_size, 0, sizeof(g->block_recv_size));
    memset(g->block_recv_pos, 0, sizeof(g->block_recv_pos));
    memset(g->ready_close, 0, sizeof(g->ready_close));
    memset(g->ready_standby, 0, sizeof(g->ready_standby));
    for(int i = 0; i < FAKE_RUBY_PLAYERS; i++) g->link_player_pending[i] = true;
    g->trigger_active = true; /* CreateTask(Task_TriggerHandshake) */
    g->trigger_runs = 0;
    g->card_ready = false;
    next_task(g, FakeRubyTaskExchange);
    g->phase = FakeRubyHandshake;
}

void fake_ruby_power_off(FakeRuby* g) {
    g->powered = false;
    g->serial_on = false;
    g->link_open = false;
    g->state = FakeRubyLinkStart1;
    g->phase = FakeRubyOff;
}

void fake_ruby_leave_room(FakeRuby* g) {
    if(g->phase != FakeRubyRoom || g->error) return;
    g->facing_exit = true; /* SelfIdle now sends 0x12 until the prompt takes over */
    g->phase = FakeRubyLeaving;
}

void fake_ruby_corrupt_next(FakeRuby* g) {
    g->corrupt_pending = true;
}

void fake_ruby_corrupt_next_checksum(FakeRuby* g) {
    g->corrupt_checksum_pending = true;
}

void fake_ruby_frame(FakeRuby* g, const MlinkTx* tx, MlinkRx* rx) {
    memset(rx, 0, sizeof(*rx));
    rx->count = tx->count;
    if(g->powered && !g->error) link_vsync(g);
    for(uint8_t t = 0; t < tx->count && t < MLINK_BURST_LEN; t++) {
        if(g->powered && g->serial_on) {
            uint16_t own = g->sio_send;
            rx->word[t] = own;
            /* The Flipper may read a glitched word, while this GBA's ISR below sums and checks
             * the word it sent. send_index 1..8: transfers #2..#9 carry command words; 0 is the
             * checksum slot, checked by the Flipper once checksum_available. */
            const bool connected = g->state == FakeRubyLinkConnected;
            if(g->corrupt_pending && connected && g->send_index != 0) {
                rx->word[t] ^= CORRUPT_BIT;
                g->corrupt_pending = false;
            } else if(
                g->corrupt_checksum_pending && connected && g->send_index == 0 &&
                g->checksum_available) {
                rx->word[t] ^= CORRUPT_BIT;
                g->corrupt_checksum_pending = false;
            }
            serial_irq(g, tx->word[t], own);
        } else {
            rx->word[t] = MLINK_NO_DATA;
            rx->timeout_mask |= (uint16_t)(1u << t);
        }
    }
    if(g->powered && !g->error) main_loop(g);
}

FakeRubyPhase fake_ruby_phase(const FakeRuby* g) {
    return g->phase;
}

const char* fake_ruby_error(const FakeRuby* g) {
    return g->error;
}

const Gen3LinkPlayer* fake_ruby_partner(const FakeRuby* g) {
    return &g->link_players[0];
}
