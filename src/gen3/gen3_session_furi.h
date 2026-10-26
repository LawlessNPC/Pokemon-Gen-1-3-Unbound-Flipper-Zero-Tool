#pragma once

#include <stdbool.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/mlink_hw_furi.h>

/* One Gen III link session for the Link Test screen: the multiplayer-parent driver, the Cable
 * Club state machine and the SD-card frame log, wired together.
 *
 * The driver's frame callback (TIM2 ISR) runs the club, feeds the log ring and publishes the
 * club status. A "Gen3Log" thread drains the ring to APP_DATA_PATH("gen3_link.log") every 100 ms.
 * Every function here is an app-context call: use them from one thread, never from an ISR. */

#define GEN3_TEST_TRAINER_NAME "FLIPPER"
#define GEN3_TEST_TRAINER_TID  22222
#define GEN3_TEST_TRAINER_SID  0

typedef struct Gen3Session Gen3Session;

/* Takes the SC/SD/SO/SI pins from the gblink handle (PIN_CLK, PIN_SD, PIN_SEROUT, PIN_SERIN).
 * The pins must not change while the session exists. */
Gen3Session* gen3_session_alloc(void* gblink_handle);

/* Stops the session first if it is running. */
void gen3_session_free(Gen3Session* s);

MlinkCable gen3_session_check_cable(Gen3Session* s);

/* Starts a fresh run: club as GEN3_TEST_TRAINER_*, empty log ring, status reset to Waiting, then
 * the driver; once that is running, opens the log file (FSOM_CREATE_ALWAYS) and starts the log
 * thread. false (nothing started) if the cable is reversed or TIM2 is busy. A missing SD card is
 * not an error: the link runs and log_ok reads false. true if already running. */
bool gen3_session_start(Gen3Session* s); /* false if cable reversed or TIM2 busy */

/* Stops the driver, lets the log thread drain the ring and exit, then closes the log file.
 * Does nothing if not running. The last status stays readable. */
void gen3_session_stop(Gen3Session* s);

/* Latest status published by the frame callback. log_ok (may be NULL): the log file is open and
 * no write to it has failed. cable_ok (may be NULL): the driver's live cable guard
 * (mlink_hw_cable_ok); while it reads false the driver skips every frame's transfers. */
void gen3_session_status(Gen3Session* s, Gen3ClubStatus* out, bool* log_ok, bool* cable_ok);
