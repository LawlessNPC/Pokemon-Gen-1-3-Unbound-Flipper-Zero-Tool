#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/gen3_omega.h>
#include <src/gen3/gen3_save.h>

/* The Cartridge session: the link driver in stream mode, running first the BIOS multiboot sender
 * (gen3_mb.h) and then the helper's packet pump (gen3_hlink.h) in its frame callback; a worker
 * thread that runs one cartridge job at a time (gen3_cart.h) on top of them and the SD store
 * (gen3_store_furi.h); and a log, apps_data/pokemon/gen3_cart.log, which only the worker writes.
 *
 * The frame callback inside runs in the TIM2 interrupt and is ISR-safe: it calls the two cores
 * and nothing else. Every function declared here is app-side. Its jobs (Gen3CartJob) and status
 * (Gen3CartSessionStatus) are in gen3_cart.h. */

typedef struct Gen3CartSession Gen3CartSession;

/* App thread. The driver's pins come from gblink_handle, as gen3_session_alloc takes them. Starts
 * the worker and creates the log (none without an SD card). Nothing runs on the link until the
 * first Connect. */
Gen3CartSession* gen3_cart_session_alloc(void* gblink_handle);
/* Cancels any job and waits for it to end (a Write or Restore always runs to its end), then stops
 * the driver and frees everything. */
void gen3_cart_session_free(Gen3CartSession* s);
/* Starts a job on the worker. arg: Restore and RestoreCheck, the backup's name relative to the
 * store's folder ("backups/....sav"); Rom, "resume" to go on from the .part, or NULL; others
 * ignore it. false, and nothing started, while a job runs. Not Gen3CartJobPicture (false):
 * gen3_cart_session_picture starts that. A job before the first Connect has
 * loaded the helper file ends BadArg. */
bool gen3_cart_session_run(Gen3CartSession* s, Gen3CartJob job, const char* arg);
/* Starts Gen3CartJobPicture for species on the worker: an Omega's picture, fetched into the
 * store's GEN3_OMEGA_PICS folder (gen3_omega_picture) after the one-time gen3_omega_pics_find. It
 * sets status.busy and pic_busy at once and ends with pic_result; nothing else of the status
 * changes, and the log gets one line, `job picture <species> end <result>`. false, and nothing
 * started, while a job runs. A job before an Omega's Connect ends BadArg. */
bool gen3_cart_session_picture(Gen3CartSession* s, uint16_t species);
/* Asks the running job to stop where it can (gen3_cart.h says where; Write and Restore never do,
 * nor Discard). Until then it stays busy. */
void gen3_cart_session_cancel(Gen3CartSession* s);
/* Copies the status, under the session's lock, into out: a static buffer in app code. */
void gen3_cart_session_status(Gen3CartSession* s, Gen3CartSessionStatus* out);
/* The jobs' cart state, for the GUI thread and only while no job runs: mon_get and mon_put, names,
 * ids. All zero, with no cart, no backup and no change, until the first Connect loads the helper
 * file. */
Gen3Cart* gen3_cart_session_cart(Gen3CartSession* s);
/* An EZ-Flash Omega's state (status.omega), under the same rules as gen3_cart_session_cart: all
 * zero, not connected and with no backup, until a Connect finds one */
Gen3Omega* gen3_cart_session_omega(Gen3CartSession* s);
