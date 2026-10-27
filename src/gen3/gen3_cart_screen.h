#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_cart_furi.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/gen3_save.h>

/* What the Cartridge app's screens show and offer, decided from the session's status and the
 * cart: the progress screen, the hub's items, Cart info, the result after a job, and the leave
 * warning. Pure C, no heap, no file-scope state, so the host tests check every text fits the
 * 128-pixel screen, which breaks lines only at '\n': a line of at most 21 characters, a title or
 * header of at most 16. gen3_cart_furi.h is included for the session's job and status types only;
 * it includes no furi header. */

#define GEN3_CART_SCREEN_LINE_LEN 21u
#define GEN3_CART_SCREEN_TITLE_LEN 16u
/* MbWait: how long the sender looks for the GBA before the screen says how to start it */
#define GEN3_CART_SCREEN_HINT_US 3000000u

/* What a result's Retry does */
typedef enum {
    Gen3CartRetryNone, /* no Retry: OK only */
    Gen3CartRetryConnect, /* back to the instructions, to send the helper again */
    Gen3CartRetryReconnect, /* the Reconnect job */
} Gen3CartRetry;

/* Back on the progress screen while job runs */
typedef enum {
    /* Back does nothing: Write, Restore and Discard run to their end, a cancel or not */
    Gen3CartBackIgnore,
    Gen3CartBackStop, /* gen3_cart_session_cancel, and wait for the job to end */
} Gen3CartBack;
Gen3CartBack gen3_cart_screen_back(Gen3CartJob job);

/* Which status the progress screen shows */
typedef enum {
    /* the job was just handed to the session: its status may still hold the last job's step */
    Gen3CartScreenStarting,
    Gen3CartScreenRunning,
    Gen3CartScreenStopping, /* Back stopped the job, which hasn't ended yet */
} Gen3CartScreenState;

/* The progress screen's texts: static strings. line1 may be the cable warning, the one text
 * longer than 21 characters ("Cable: plug in/swap ends", as the Link Test shows it). */
typedef struct {
    const char* title;
    const char* line1;
    const char* line2;
    int8_t percent; /* -1: no bar */
    const char* hint; /* "Back: stop", "Do not power off" or "" */
} Gen3CartScreenProgress;

/* From the status of the job running (st->busy): its job, step, done and total, cable_ok and, in
 * MbWait, mb. It never reads link_up, which a Connect not yet taken by the worker leaves as the
 * last job left it. */
void gen3_cart_screen_progress(
    const Gen3CartSessionStatus* st,
    Gen3CartScreenState state,
    Gen3CartScreenProgress* out);

/* The power-on hint ("Hold START+SELECT / and power on the GBA"): Connect in MbWait with the cable
 * in, GEN3_CART_SCREEN_HINT_US without a GBA, and the sender has had no failure yet, so never
 * while it recovers from a glitch with the GBA on. */
bool gen3_cart_screen_power_hint(const Gen3CartSessionStatus* st);

/* The hub's items beyond Cart info, which is always there */
typedef struct {
    /* the helper didn't answer the last command, or the last job was a Reconnect that didn't run
     * to its end */
    bool reconnect;
    bool backup; /* "Back up again": no current backup of a save the helper can read */
} Gen3CartScreenHub;
/* st: the status now, with the last job and its result. While a job runs (busy) neither shows,
 * and link_up isn't read. */
void gen3_cart_screen_hub(
    const Gen3CartSessionStatus* st,
    bool backup_current,
    bool can_read_save,
    Gen3CartScreenHub* out);

/* The result screen after job ended with r */
typedef struct {
    const char* header; /* at most 16 characters */
    const char* text; /* at most 3 lines of at most 21 characters */
    Gen3CartRetry retry;
} Gen3CartScreenResult;
/* step: the job's last step (the status's, once it ended). dropped: Connect's, the edits pending
 * from before were dropped. A Write or Restore that lost the link once it may have committed
 * (Commit, Verify or PostBackup) says the cart may load either save: after the last commit the
 * game loads the new one. */
void gen3_cart_screen_result(
    Gen3CartJob job,
    Gen3CartStep step,
    Gen3CartResult r,
    bool dropped,
    Gen3CartScreenResult* out);

/* Cart info: a header and 5 lines */
#define GEN3_CART_SCREEN_HEADER_SIZE (GEN3_CART_SCREEN_TITLE_LEN + 1u)
#define GEN3_CART_SCREEN_INFO_SIZE (5u * (GEN3_CART_SCREEN_LINE_LEN + 1u))
typedef struct {
    const Gen3CartId* id; /* after a connect that ran to its end */
    bool backup_current;
    const char* backup_name; /* gen3_cart_backup_name */
    const Gen3SaveSlots* slots; /* NULL: no analysis (no current backup, or not can_browse) */
    const Gen3SaveTrainer* trainer; /* NULL: no valid save */
    bool writable;
    Gen3CartRefuse why; /* when not writable (gen3_cart_writable) */
} Gen3CartScreenInfo;
/* header "<code> v<version>"; text: the flash chip, the save the game loads, the trainer, whether
 * writing is on (else why not), and the backup's stamp (MMDD-HHMMSS) */
void gen3_cart_screen_info(
    const Gen3CartScreenInfo* in,
    char header[GEN3_CART_SCREEN_HEADER_SIZE],
    char text[GEN3_CART_SCREEN_INFO_SIZE]);

/* The leave warning: "<n> changes not\nwritten." ("1 change" for one) */
#define GEN3_CART_SCREEN_LEAVE_SIZE 32u
void gen3_cart_screen_leave(uint16_t changes, char out[GEN3_CART_SCREEN_LEAVE_SIZE]);
