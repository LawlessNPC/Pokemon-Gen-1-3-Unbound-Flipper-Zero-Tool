#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/gen3_save.h>

/* The Cartridge session: the link driver in stream mode, running first the BIOS multiboot sender
 * (gen3_mb.h) and then the helper's packet pump (gen3_hlink.h) in its frame callback; a worker
 * thread that runs one cartridge job at a time (gen3_cart.h) on top of them and the SD store
 * (gen3_store_furi.h); and a log, apps_data/pokemon/gen3_cart.log, which only the worker writes.
 *
 * The frame callback inside runs in the TIM2 interrupt and is ISR-safe: it calls the two cores
 * and nothing else. Every function declared here is app-side. */

typedef enum {
    Gen3CartJobConnect, /* send the helper (again), then gen3_cart_connect and gen3_cart_attach */
    Gen3CartJobBackup,
    Gen3CartJobWrite,
    Gen3CartJobDiscard,
    Gen3CartJobRestoreCheck,
    Gen3CartJobRestore,
    Gen3CartJobRom,
    Gen3CartJobReconnect,
} Gen3CartJob;

/* About 100 B: never a stack local in a scene; copy it into a static buffer. */
typedef struct {
    bool busy; /* from gen3_cart_session_run until the job's result is in */
    Gen3CartJob job;
    Gen3CartResult result; /* the last job's, valid when !busy */
    Gen3CartStep step;
    uint32_t done, total;
    /* false while Connect waits for the cable (no cable, or the wrong end in the Flipper); while
     * the driver runs, its cable guard's latest decision */
    bool cable_ok;
    Gen3MbStatus mb; /* Connect: the multiboot sender's, during MbWait and MbSend */
    bool link_up; /* the helper answered the last command */
    bool dropped; /* Connect: edits that were pending were dropped (gen3_cart_attach) */
    Gen3SaveTrainer restore_trainer; /* RestoreCheck: the backup's trainer */
    bool restore_has_trainer;
} Gen3CartSessionStatus;

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
 * ignore it. false, and nothing started, while a job runs. A job before the first Connect has
 * loaded the helper file ends BadArg. */
bool gen3_cart_session_run(Gen3CartSession* s, Gen3CartJob job, const char* arg);
/* Asks the running job to stop where it can (gen3_cart.h says where; Write and Restore never do,
 * nor Discard). Until then it stays busy. */
void gen3_cart_session_cancel(Gen3CartSession* s);
/* Copies the status, under the session's lock, into out: a static buffer in app code. */
void gen3_cart_session_status(Gen3CartSession* s, Gen3CartSessionStatus* out);
/* The jobs' cart state, for the GUI thread and only while no job runs: mon_get and mon_put, names,
 * ids. All zero, with no cart, no backup and no change, until the first Connect loads the helper
 * file. */
Gen3Cart* gen3_cart_session_cart(Gen3CartSession* s);
