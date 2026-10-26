#pragma once

#include <stdbool.h>
#include <furi_hal_gpio.h>
#include <src/gen3/mlink_types.h>

/* GBA multiplayer parent (SIO master) over the Game Link GPIO module.
 *
 * Every TIM2 tick (751.5 us) may run one 16-bit multiplayer transfer, bit-banged at 115200 baud.
 * Transfers come in frames at the GBA frame rate: transfer t of a frame runs on tick t of that
 * frame, t < tx.count. After a frame's last transfer the frame callback runs, in the TIM2
 * interrupt with interrupts enabled. It gets that frame's result and fills the next frame's tx.
 *
 * alloc/free/check_cable/start/stop are app-context calls; use them from one thread, never from
 * the frame callback. */

typedef struct {
    const GpioPin* sc; /* SC: frame clock, driven by the parent */
    const GpioPin* sd; /* SD: shared data line (open drain) */
    const GpioPin* so; /* SO: goes to the GBA's SI */
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

/* Pins back to GpioModeAnalog, TIM2 off, ISR cleared. Does nothing if not running. */
void mlink_hw_stop(MlinkHw* hw);
