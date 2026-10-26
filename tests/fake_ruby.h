#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/mlink_types.h>
#include <src/gen3/gen3_link_player.h>

/* Test-only model of Pokemon Ruby as the multiplayer child (slot 1) during the Trade Center
 * linkup, the room and leaving it (docs/gen3/link-protocol.md §A2-A7, §B7), sitting down and the
 * trade menu's relink (docs/gen3/link-protocol-trade.md §B1). It models the
 * child's own link stack and does not use src/gen3/gen3_link.c: the per-transfer ISR
 * (DoHandshake, DoRecv, DoSend), its own send and receive queues, LinkMain1/LinkMain2 with
 * gSendCmd/gRecvCmds, gLinkCallback, the cable_club.c linkup tasks, and in the room the
 * lockstep game update (CB1_OverworldLink: player link states, key intercept callbacks) with
 * the exit and seat scripts, then the trade menu's own callbacks and tasks. One fake_ruby_frame()
 * is one GBA frame: VBlank (LinkVSync), the frame's transfers, then the main loop. Any error
 * stops it answering (0xFFFF from then on). */

#define FAKE_RUBY_CMD_LEN   8 /* CMD_LENGTH */
#define FAKE_RUBY_QUEUE_LEN 50 /* QUEUE_CAPACITY */
#define FAKE_RUBY_PLAYERS   2 /* Flipper (slot 0) + this GBA (slot 1) */
#define FAKE_RUBY_BLOCK_MAX 256 /* BLOCK_BUFFER_SIZE: one gBlockRecvBuffer slot */

typedef enum {
    FakeRubyOff,
    FakeRubyHandshake,
    FakeRubyLinkup,
    FakeRubyWalkIn,
    FakeRubyRoom,
    FakeRubyLeaving,
    FakeRubyClosed,
    FakeRubySeated, /* seat script: "Please wait..." until the trade starts or the seat is left */
    FakeRubyMenu, /* 0x1122: relink, party exchange, trade menu */
    FakeRubyTrade, /* 0x1144: relink, trade animation, ABCD/DCBA */
    FakeRubySave, /* link save */
    FakeRubyReturn, /* after EEBB: back to the room through the 0x1111 re-establish */
} FakeRubyPhase;

/* Private state below; tests use the functions only. */

typedef enum {
    FakeRubyLinkStart0, /* LINK_STATE_START0 */
    FakeRubyLinkStart1, /* LINK_STATE_START1 */
    FakeRubyLinkHandshake, /* LINK_STATE_HANDSHAKE */
    FakeRubyLinkConnected, /* LINK_STATE_CONN_ESTABLISHED */
} FakeRubyLinkState;

typedef enum {
    FakeRubyCbNone,
    FakeRubyCbRequestPlayerData, /* LinkCB_RequestPlayerDataExchange */
    FakeRubyCbBlockSendBegin, /* LinkCB_BlockSendBegin */
    FakeRubyCbBlockSend, /* LinkCB_BlockSend */
    FakeRubyCbBlockSendEnd, /* LinkCB_BlockSendEnd */
    FakeRubyCbReadyCloseLink, /* sub_8008350 [LinkCB_ReadyCloseLink] */
    FakeRubyCbWaitCloseLink, /* sub_800837C [LinkCB_WaitCloseLink] */
    FakeRubyCbStandby, /* sub_80084C8 [LinkCB_Standby] */
    FakeRubyCbStandbyForAll, /* sub_80084F4 [LinkCB_StandbyForAll] */
    FakeRubyCbSendHeldKeys, /* sub_8007B44 [LinkCB_SendHeldKeys] */
} FakeRubyCallback;

typedef enum {
    FakeRubyTaskNone,
    FakeRubyTaskExchange, /* sub_80831F8 [Task_LinkupExchangeDataWithLeader] */
    FakeRubyTaskMakeCard, /* TrainerCard_GenerateCardForPlayer, after the task latency */
    FakeRubyTaskAwaitCards, /* sub_8083314 [Task_LinkupAwaitTrainerCardData] */
    FakeRubyTaskWalkIn, /* script: door animation, warp, map load */
    FakeRubyTaskStandby, /* sub_8080A5C: 0x2FFE standby */
    FakeRubyTaskRoom, /* sending CAFE every frame */
    FakeRubyTaskStop, /* sub_80833C4 [Task_StopLinkup]: closing after "different selections" */
    /* Part B: these run every frame, the link open or not (runs_while_closed in fake_ruby.c) */
    FakeRubyTaskStartTrade, /* sub_8083AAC [Task_StartWiredTrade] */
    FakeRubyTaskStartMenu, /* callback2 sub_8047CD8 [CB2_StartCreateTradeMenu] */
    FakeRubyTaskCreateMenu, /* callback2 sub_8047EC0 [CB2_CreateTradeMenu] */
} FakeRubyTask;

/* gUnknown_03000584 [sPlayerKeyInterceptCallback]: picks the key code this GBA sends next. */
typedef enum {
    FakeRubyKeySelfIdle, /* sub_8055390 [KeyInterCB_SelfIdle] */
    FakeRubyKeyDeferToEventScript, /* sub_80553E4 [KeyInterCB_DeferToEventScript] */
    FakeRubyKeySendExitRoomKey, /* sub_80554E4 [KeyInterCB_SendExitRoomKey] */
    FakeRubyKeyWaitForPlayersToExit, /* sub_80554BC [KeyInterCB_WaitForPlayersToExit] */
    FakeRubyKeySendNothing, /* sub_80554B8 [KeyInterCB_SendNothing] */
    FakeRubyKeyIdle, /* sub_80553E0 [KeyInterCB_Idle] */
    FakeRubyKeySetReady, /* sub_80554A4 [KeyInterCB_SetReady] */
    FakeRubyKeyReady, /* sub_805546C [KeyInterCB_Ready] */
    FakeRubyKeyExitingSeat, /* sub_8055468 [KeyInterCB_ExitingSeat] */
} FakeRubyKeyCb;

/* Room event scripts and the task that takes the player out (§B7); the seat script (§B1.1). */
typedef enum {
    FakeRubyScriptNone,
    FakeRubyScriptPrompt, /* TradeRoom_PromptToCancelLink: the player answers YES */
    FakeRubyScriptTerminate, /* TradeRoom_TerminateLink: message, then ExitLinkRoom */
    FakeRubyScriptExitStart, /* gUnknown_081A4508 -> sub_8081050 case 0: stop CAFE, fade */
    FakeRubyScriptExitFade, /* sub_8081050 case 1: after the fade, SetCloseLinkCallback */
    FakeRubyScriptExitWait, /* sub_8081050 case 2: wait for CloseLink, then warp */
    FakeRubyScriptSeat, /* TradeCenter_EventScript_1A43F0/1A43FA: special sub_8083B5C */
    FakeRubyScriptSeatWait, /* its waitstate, until the seat task's ScriptContext_Enable */
    FakeRubyScriptSeatEnd, /* its end: the script context shuts down and unlocks the controls */
} FakeRubyScript;

/* sub_80839DC [Task_EnterCableClubSeat] */
typedef enum {
    FakeRubySeatNone,
    FakeRubySeatPrint, /* cases 0-1: "Please wait..." prints */
    FakeRubySeatWait, /* case 2: sub_80554F8 [GetCableClubPartnersReady] every update */
    FakeRubySeatCancel, /* case 3 */
} FakeRubySeatTask;

typedef struct FakeRuby FakeRuby;

struct FakeRuby {
    Gen3LinkPlayer me; /* this GBA's LinkPlayer (InitLocalLinkPlayer) */
    FakeRubyPhase phase;
    const char* error;
    bool powered;
    uint32_t frame; /* main-loop frames since init */

    /* link.c: gLink and its statics */
    bool serial_on; /* SIOCNT in multiplayer mode (EnableSerial .. DisableSerial) */
    bool link_open; /* gLinkOpen */
    FakeRubyLinkState state;
    uint16_t sio_send; /* REG_SIOMLT_SEND: this GBA's word in the next transfer */
    uint8_t irq_counter; /* serialIntrCounter: s8, never reset on a child -> wraps every 256 */
    uint8_t vblanks_without_irq; /* sNumVBlanksWithoutSerialIntr */
    uint8_t hs_prev_count; /* sHandshakePlayerCount */
    uint8_t player_count;
    uint8_t send_index, recv_index; /* sendCmdIndex, recvCmdIndex */
    uint16_t checksum;
    bool checksum_available; /* sChecksumAvailable */
    bool bad_checksum;
    bool lag; /* LAG_SLAVE */
    bool recv_nonzero; /* sRecvNonzeroCheck */
    bool send_empty; /* sSendBufferEmpty */
    uint16_t send_queue[FAKE_RUBY_QUEUE_LEN][FAKE_RUBY_CMD_LEN];
    uint8_t send_pos, send_count;
    uint16_t recv_queue[FAKE_RUBY_QUEUE_LEN][FAKE_RUBY_PLAYERS][FAKE_RUBY_CMD_LEN];
    uint8_t recv_pos, recv_count;
    uint8_t last_recv_count; /* gLastRecvQueueCount */
    uint16_t send_cmd[FAKE_RUBY_CMD_LEN]; /* gSendCmd */
    uint16_t recv_cmds[FAKE_RUBY_PLAYERS][FAKE_RUBY_CMD_LEN]; /* gRecvCmds */
    bool should_advance; /* gShouldAdvanceLinkState */
    bool trigger_active; /* Task_TriggerHandshake */
    uint8_t trigger_runs;
    FakeRubyCallback callback; /* gLinkCallback */

    /* sBlockSend, gBlockSendBuffer */
    bool block_send_active;
    uint16_t block_send_size, block_send_pos;
    uint8_t block_send_delay; /* sBlockSendDelayCounter */
    uint8_t block_send_buffer[FAKE_RUBY_BLOCK_MAX];

    /* sBlockRecv, gBlockRecvBuffer, per slot */
    uint16_t block_recv_size[FAKE_RUBY_PLAYERS], block_recv_pos[FAKE_RUBY_PLAYERS];
    uint8_t block_recv_buffer[FAKE_RUBY_PLAYERS][FAKE_RUBY_BLOCK_MAX];
    bool link_player_pending[FAKE_RUBY_PLAYERS]; /* gLinkPlayerPending */
    bool received_link_players; /* gReceivedRemoteLinkPlayers */
    uint8_t block_received; /* gBlockReceived, bit i = slot i */
    Gen3LinkPlayer link_players[FAKE_RUBY_PLAYERS]; /* gLinkPlayers */
    bool ready_close[FAKE_RUBY_PLAYERS]; /* gReadyToCloseLink */
    bool ready_standby[FAKE_RUBY_PLAYERS]; /* gReadyToExitStandby */
    uint16_t held_keys[FAKE_RUBY_PLAYERS]; /* gLinkPartnersHeldKeys */

    /* cable_club.c tasks and the counter script */
    FakeRubyTask task;
    uint32_t task_frame; /* frame the current task step began */
    bool card_ready; /* own trainer card is in gBlockSendBuffer */
    uint32_t standby_frame; /* frame this GBA built its 0x2FFE */

    /* overworld.c in the Trade Center: CB1_OverworldLink and the room scripts */
    bool received_nothing; /* gLink.receivedNothing: the last dequeue found the queue empty */
    uint8_t link_state[FAKE_RUBY_PLAYERS]; /* gUnknown_03000580 [sPlayerLinkStates] */
    FakeRubyKeyCb key_cb;
    uint16_t held_key_to_send; /* word_3004858 [gHeldKeyCodeToSend] */
    bool controls_locked; /* ArePlayerFieldControlsLocked */
    bool facing_exit; /* on its exit tile facing south, DOWN held (the walk is abstracted) */
    FakeRubyScript script;
    uint16_t script_updates; /* game updates left in the current script step */

    bool corrupt_pending; /* fake_ruby_corrupt_next: glitch the next command word */
    bool corrupt_checksum_pending; /* fake_ruby_corrupt_next_checksum: glitch the next checksum */

    /* Part B: the seat (overworld.c, cable_club.c) and the trade menu (trade.c) */
    bool sit_pending; /* fake_ruby_sit: a step onto sit_seat finishes at the next room update */
    uint8_t sit_seat;
    bool b_press; /* fake_ruby_stand_up: B is new at the next room update */
    bool on_seat; /* the avatar stands on a seat tile */
    uint8_t seat; /* VAR_0x8005 of the seat script */
    FakeRubySeatTask seat_task;
    uint16_t seat_updates; /* game updates left in FakeRubySeatPrint */
    uint8_t start_case; /* sub_8083AAC data[0] */
    uint8_t menu_state; /* sub_8047EC0 gMain.state */
    uint8_t menu_count; /* its unk_00b4 */
    bool wait_task; /* sub_8083C50 [Task_WaitForLinkPlayerConnection] runs */
    uint16_t wait_count; /* its data[0] */
    uint32_t open_frame; /* main-loop frame of the latest OpenLink */
    uint32_t relink_frames; /* open_frame -> gReceivedRemoteLinkPlayers; UINT32_MAX while waiting */
};

/* me.link_type picks the counter */
void fake_ruby_init(FakeRuby* g, const Gen3LinkPlayer* me);
/* player chose the counter: link opens after 6 frames (§A3 timeline) */
void fake_ruby_open_link(FakeRuby* g);
/* answers 0xFFFF from now on; fake_ruby_init powers it on again */
void fake_ruby_power_off(FakeRuby* g);
/* In the room: the player walks onto its exit, faces south and confirms "Terminate the link?"
 * (§B7). It sends CAFE 0017 and waits, with no timeout, until every player is EXITING_ROOM; then
 * stops sending keys, fades out, sends 5FFF, waits for the Flipper's 5FFF and closes (0xFFFF). */
void fake_ruby_leave_room(FakeRuby* g);
/* Flips one bit of the next command word this GBA puts on the wire, as the Flipper reads it.
 * This GBA sums its own word, so both checksums diverge, as with a glitch at the receiver. */
void fake_ruby_corrupt_next(FakeRuby* g);
/* Flips one bit of the next checked checksum word (transfer #1, not the first burst's stale
 * 0xB9A0) this GBA puts on the wire, as the Flipper reads it. Transfer #1 is not summed (§A2) and
 * this GBA checks the Flipper's word and the one it sent, so only the Flipper sees a mismatch:
 * this GBA stays healthy, as with a glitch at the Flipper's receiver. */
void fake_ruby_corrupt_next_checksum(FakeRuby* g);
/* Room: the player walks onto seat 0 (4,5) or 1 (7,5). It acts on the next game update in the
 * room if this GBA's player is IDLE and not on a seat, else it has no effect: standing on a seat
 * never triggers it again (§B1.1). */
void fake_ruby_sit(FakeRuby* g, uint8_t seat);
/* "Please wait...": the player presses B, seen by the next game update in the room (§B1.2). */
void fake_ruby_stand_up(FakeRuby* g);
/* gUnknown_03000580 [sPlayerLinkStates]: 0x80 IDLE, 0x81 BUSY, 0x82 READY, 0x83 EXITING_ROOM */
uint8_t fake_ruby_link_state(const FakeRuby* g, uint8_t slot);
/* Frames from the latest OpenLink to gReceivedRemoteLinkPlayers; UINT32_MAX while waiting. */
uint32_t fake_ruby_relink_frames(const FakeRuby* g);
/* simulates one frame of transfers */
void fake_ruby_frame(FakeRuby* g, const MlinkTx* tx, MlinkRx* rx);
FakeRubyPhase fake_ruby_phase(const FakeRuby* g);
/* NULL, or "checksum", "magic", "stale card", "lag", "standby timeout", "link timeout" (no
 * LinkPlayers 300 frames after a Part B OpenLink), "link type" (the trade menu's link types
 * differ; Ruby hangs silently there, §B8) */
const char* fake_ruby_error(const FakeRuby* g);
/* the Flipper's block as Ruby parsed it */
const Gen3LinkPlayer* fake_ruby_partner(const FakeRuby* g);
