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

#define LOCAL_ID       1 /* multiplayer id of the child */
#define KEY_CODE_EMPTY 0x11

#define TRIGGER_HANDSHAKE_RUNS 5 /* Task_TriggerHandshake */
#define LAG_VBLANKS            10 /* LinkVSync: > 10 VBlanks without a serial IRQ */
#define CARD_READY_BURSTS      3 /* own card ready this many bursts after both player blocks */
#define WALK_IN_FRAMES         30 /* zero-command frames between the cards and 0x2FFE */
#define STANDBY_TIMEOUT_FRAMES 600 /* test limit; Ruby itself waits forever */

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
        if(g->received_link_players) {
            g->send_cmd[0] = CMD_SEND_HELD_KEYS;
            g->send_cmd[1] = KEY_CODE_EMPTY;
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

/* cable_club.c (child side of the Trade Center linkup) and the counter script. */
static void run_linkup_task(FakeRuby* g) {
    switch(g->task) {
    case FakeRubyTaskNone:
    case FakeRubyTaskRoom:
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

void fake_ruby_frame(FakeRuby* g, const MlinkTx* tx, MlinkRx* rx) {
    memset(rx, 0, sizeof(*rx));
    rx->count = tx->count;
    if(g->powered && !g->error) link_vsync(g);
    for(uint8_t t = 0; t < tx->count && t < MLINK_BURST_LEN; t++) {
        if(g->powered && g->serial_on) {
            uint16_t own = g->sio_send;
            rx->word[t] = own;
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
