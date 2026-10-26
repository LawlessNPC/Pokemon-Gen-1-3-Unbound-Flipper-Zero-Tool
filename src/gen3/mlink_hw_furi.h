#pragma once

#include <stdbool.h>
#include <furi_hal_gpio.h>
#include <src/gen3/mlink_types.h>

/* GBA multiplayer parent (SIO master) over the Game Link GPIO module.
 *
 * Every TIM2 tick (1073 us start to start, leaving the GBA's ~751.5 us between transfers) may run
 * one 16-bit multiplayer transfer, bit-banged at 115200 baud.
 * Transfers come in frames at the GBA frame rate: transfer t of a frame runs on tick t of that
 * frame, t < tx.count. After a frame's last transfer the frame callback runs, in the TIM2
 * interrupt with interrupts enabled. It gets that frame's result and fills the next frame's tx.
 *
 * Cable guard: SI is read on every idle tick (one that runs no transfer) and latched if high, and
 * read again at each frame start, before any line moves. High means the cable is out or reversed
 * (a reversed cable can read low until the GBA opens its link, so a check at start is not enough;
 * a GBA master's SO, on our SI, dips low during its own transfers, so one read per frame can miss
 * it). If SI is high at the frame start or was latched since the previous one, the frame runs no
 * transfer at all: SC and SO are released to input with pull-up (SD already is, between
 * transfers), and the callback gets the frame at once, every word MLINK_NO_DATA and every
 * timeout_mask bit set, as if the GBA never answered. The next frame that runs transfers first
 * makes SC and SO push-pull outputs again, driven high.
 *
 * alloc/free/check_cable/start/stop are app-context calls; use them from one thread, never from
 * the frame callback. mlink_hw_cable_ok may be called from any thread. */

typedef struct {
    /* SC: frame clock, driven by the parent: push-pull output, idle high. SC and SO keep their
     * pull-up on and are released to input with pull-up while the cable guard trips. */
    const GpioPin* sc;
    /* SD: shared data line, pulled up. Pushed and pulled only for the Flipper's own word, then
     * released to input with pull-up (MLINK_SD_PUSH_PULL 1, the default); or open drain the whole
     * time (MLINK_SD_PUSH_PULL 0). */
    const GpioPin* sd;
    const GpioPin* so; /* SO: goes to the GBA's SI; push-pull output, idle high, released as SC */
    const GpioPin* si; /* SI: grounded by the cable's parent plug */
} MlinkPins;

typedef enum {
    MlinkCableOk,
    MlinkCableReversed,
} MlinkCable;

/* Called in ISR context after each frame's last transfer: rx is that frame's result; fill next.
 * next arrives holding the frame just sent. Must not block, allocate or log; return quickly. */
typedef void (*MlinkFrameCallback)(void* ctx, const MlinkRx* rx, MlinkTx* next);

typedef struct MlinkHw MlinkHw;

MlinkHw* mlink_hw_alloc(const MlinkPins* pins);

/* Stops the driver first if it is running. */
void mlink_hw_free(MlinkHw* hw);

/* SI read low with pull-up => Flipper holds the parent plug. No cable reads as reversed.
 * When stopped, configures SI as input with pull-up, waits 1 ms, reads it, and puts SI back to
 * analog. When running, only reads SI. */
MlinkCable mlink_hw_check_cable(MlinkHw* hw);

/* false (and nothing enabled) if TIM2 is already on (furi_hal_bus_is_enabled), the cable is
 * reversed, or the driver is already running. first is the first frame's tx; count is clamped to
 * 1..MLINK_BURST_LEN. */
bool mlink_hw_start(MlinkHw* hw, MlinkFrameCallback cb, void* ctx, const MlinkTx* first);

/* The cable guard's latest decision: true from mlink_hw_start (its own check passed) until a frame
 * start finds SI high (read then, or latched on an idle tick since the previous frame start), and
 * true again once a frame start finds it low with nothing latched. Keeps its value when stopped;
 * false before the first start. */
bool mlink_hw_cable_ok(const MlinkHw* hw);

/* Pins back to GpioModeAnalog, TIM2 off, ISR cleared. Does nothing if not running. */
void mlink_hw_stop(MlinkHw* hw);
