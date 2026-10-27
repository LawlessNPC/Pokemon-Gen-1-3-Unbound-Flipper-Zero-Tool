#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_save.h>

/* What the Cartridge app's screens show and offer, decided from the session's status and the
 * cart: the progress screen, the hub's items, Cart info, the result after a job, the leave
 * warning, and the party and box screens. Pure C, no heap, no file-scope state, so the host tests
 * check every text fits the 128-pixel screen, which breaks lines only at '\n': a line of at most
 * 21 characters, a title or header of at most 16. */

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

/* The cart as the hub reads it, once no job runs */
typedef struct {
    bool busy; /* a job runs: the rest isn't read (the cart is the job's) */
    bool linked; /* gen3_cart_linked */
    bool backup_current;
    bool can_read_save;
    bool browsable; /* gen3_cart_browsable */
    bool writable; /* gen3_cart_writable */
    uint16_t changes; /* gen3_cart_changes */
} Gen3CartScreenHubIn;
/* The hub's items beyond Cart info, which is always there */
typedef struct {
    bool browse; /* "Party and boxes" */
    bool write; /* "Write changes (<n>)": writable, with changes */
    bool writing_off; /* "Writing off: <why>", which opens Cart info: not writable */
    bool discard; /* "Discard changes": changes */
    /* not linked: a command was lost, or a connect (a Reconnect) didn't run to its end */
    bool reconnect;
    bool backup; /* "Back up again": no current backup of a save the helper can read */
} Gen3CartScreenHub;
/* While a job runs, none of them */
void gen3_cart_screen_hub(const Gen3CartScreenHubIn* in, Gen3CartScreenHub* out);

/* The result screen after job ended with r */
typedef struct {
    const char* header; /* at most 16 characters */
    const char* text; /* at most 3 lines of at most 21 characters */
    Gen3CartRetry retry;
} Gen3CartScreenResult;
/* step: the job's last step (the status's, once it ended). dropped: Connect's, the edits pending
 * from before were dropped. linked: gen3_cart_linked once it ended; a job other than Connect or
 * Reconnect that failed and left the cart unlinked offers Reconnect. A job Back stopped gets a
 * neutral header. A Write or Restore that lost the link once it may have committed (Commit, Verify
 * or PostBackup) says the cart may load either save: after the last commit the game loads the new
 * one. */
void gen3_cart_screen_result(
    Gen3CartJob job,
    Gen3CartStep step,
    Gen3CartResult r,
    bool dropped,
    bool linked,
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

/* ---- The party and box screens ---- */

/* The headers when a Pokemon isn't opened in the editor, or isn't put into a slot */
#define GEN3_CART_SCREEN_CANT_EDIT "Can't edit"
#define GEN3_CART_SCREEN_CANT_PUT "Can't put"
/* Why, in at most 3 lines; "" for Ok, Empty, and a value outside the enum */
const char* gen3_cart_screen_mon_text(Gen3CartMon m);

/* A slot's row, numbered from 1 (slot 6 is "07"): "07 TREECKO Lv5", "07 EGG", "07 BAD EGG" or
 * "07 -- empty --", with '*' after it when changed. s NULL: the working copy couldn't give it
 * ("07 ?"). species: the Ok kind's name (gen3_species_name_get). */
#define GEN3_CART_SCREEN_ROW_SIZE 24u
void gen3_cart_screen_row(
    uint8_t slot,
    const Gen3MonSummary* s,
    const char* species,
    bool changed,
    char out[GEN3_CART_SCREEN_ROW_SIZE]);

/* The Put dialog: header "Put <SPECIES> Lv<n>", text "in <box>\nslot <k>?" (slot numbered from
 * 1). A header over 16 characters drops the level, which the text then starts with:
 * "Put CHARMANDER", "Lv100 in BOX 14\nslot 30?". box: at most 8 characters. */
#define GEN3_CART_SCREEN_PUT_SIZE 32u
void gen3_cart_screen_put(
    const char* species,
    uint8_t level,
    const char* box,
    uint8_t slot,
    char header[GEN3_CART_SCREEN_HEADER_SIZE],
    char text[GEN3_CART_SCREEN_PUT_SIZE]);

/* Under "Write changes?": "<n> changes. The save\nis backed up. Keep\nthe GBA on."; under
 * "Discard changes?": "<n> changes." ("1 change" for one) */
#define GEN3_CART_SCREEN_ASK_SIZE 64u
void gen3_cart_screen_write_ask(uint16_t changes, char out[GEN3_CART_SCREEN_ASK_SIZE]);
void gen3_cart_screen_discard_ask(uint16_t changes, char out[GEN3_CART_SCREEN_ASK_SIZE]);

/* gen3_check_text, its one long line broken to fit: Load failed's text */
const char* gen3_cart_screen_check_text(Gen3Check c);

/* Breaks text in place to lines of at most 21 characters, a space at a time ('\n' for ' '), after
 * dropping any leading '\n': "\nFixed: mail moves level ability" (gen3_load_changes) becomes
 * "Fixed: mail moves\nlevel ability". A word over 21 characters stays whole. */
void gen3_cart_screen_wrap(char* text);
