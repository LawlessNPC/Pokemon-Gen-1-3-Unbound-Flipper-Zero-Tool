#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <src/include/pokemon_app.h>
#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_cart_furi.h>
#include <src/gen3/gen3_cart_screen.h>
#include <src/gen3/gen3_pkm.h>

/* The Cartridge app's scenes' shared header (pokemon_gen3_cart*.c): their state, kept from the
 * first Connect until the app exits, and the helpers they share, defined in pokemon_gen3_cart.c
 * with the hub. */

/* The cart hub is registered as Gen3Menu (pokemon_cart_scene_config.h): the Gen III editor scenes
 * search back to Gen3Menu, and in the Cartridge app that is this hub. */
#define PokemonSceneGen3Cart PokemonSceneGen3Menu

/* Custom events of the Cartridge scenes, clear of scene ids and the other Gen III events. A tick
 * still queued when a scene is left reaches the next one, which ignores it. */
#define GEN3_CART_EVENT_TICK 0x47330501u /* the progress view's 250 ms timer */
#define GEN3_CART_EVENT_BACK 0x47330502u /* Back on the progress view */
#define GEN3_CART_EVENT_LEFT 0x47330503u /* a dialog's buttons (gen3_cart_ui_dialog) */
#define GEN3_CART_EVENT_CENTER 0x47330504u
#define GEN3_CART_EVENT_RIGHT 0x47330505u

typedef enum {
    Gen3CartEditNone,
    Gen3CartEditExisting,
    Gen3CartEditNew,
    Gen3CartEditLoaded,
} Gen3CartEditMode;

typedef struct Gen3CartUi {
    Gen3CartSession* session;
    /* The Pokemon slot being edited, and its bytes as they were opened */
    uint8_t box, slot;
    Gen3CartEditMode edit_mode;
    uint8_t orig[GEN3_PARTY_SIZE];
    size_t orig_len;
    /* The job Gen3CartJobRun runs (gen3_cart_ui_run_job); arg "" for none */
    Gen3CartJob job;
    char arg[GEN3_CART_NAME_MAX];
    bool job_started; /* the session took it: coming back to Gen3CartJobRun doesn't run it again */
    bool job_stopping; /* Back stopped it, and it hasn't ended yet */
    /* What Gen3CartResult shows */
    Gen3CartResult result;
    char result_header[24];
    const char* result_text; /* a static string; NULL: gen3_cart_result_text(result) */
    uint32_t result_back; /* the scene OK searches back to; PokemonSceneGen3Cart by default */
    Gen3CartRetry retry_mode; /* what Retry does; Gen3CartRetryNone: no Retry */
    /* Working buffers for opening and putting a save's Pokemon: never stack locals */
    uint8_t mon[GEN3_PARTY_SIZE];
    Gen3Pkm pkm;
    char notice[40];
} Gen3CartUi;

/* Allocates on first use the ui, the session (gen3_cart_session_alloc, which starts its worker
 * and creates the log) and the progress view (pokemon_fap->gen3_cart_progress, view id
 * AppViewGen3CartProgress), whose tick and Back post GEN3_CART_EVENT_TICK and
 * GEN3_CART_EVENT_BACK. */
Gen3CartUi* gen3_cart_ui_get(PokemonFap* pokemon_fap);
/* Frees the session (cancelling its job and waiting for it to end), the progress view and the ui.
 * Safe if nothing was allocated. Call with another view shown, before gblink_free and
 * view_dispatcher_free. */
void gen3_cart_ui_end(PokemonFap* pokemon_fap);
/* Leaves the app: cancels any job, then stops the scene manager and the view dispatcher, so the
 * app frees everything and exits. */
void gen3_cart_ui_leave(PokemonFap* pokemon_fap);

/* Copies the session's status into one static buffer and returns it (all zero before the session
 * exists). The status is never a stack local (no app stack local is over 128 bytes). App thread
 * only; the next call overwrites it. */
const Gen3CartSessionStatus* gen3_cart_ui_status(PokemonFap* pokemon_fap);
/* The cart's state: for the app thread, and only while no job runs; NULL before the session
 * exists */
Gen3Cart* gen3_cart_ui_cart(PokemonFap* pokemon_fap);
/* Pokemon slots changed and not written; 0 before the session exists */
uint16_t gen3_cart_ui_changes(PokemonFap* pokemon_fap);

/* Resets the DialogEx and sends its Left, Center and Right as GEN3_CART_EVENT_LEFT, _CENTER and
 * _RIGHT to the current scene */
void gen3_cart_ui_dialog(PokemonFap* pokemon_fap);

/* Shows the progress view for the session's status: state as gen3_cart_screen_progress takes it */
void gen3_cart_ui_progress(PokemonFap* pokemon_fap, Gen3CartScreenState state);

/* Runs job (arg NULL for none) in Gen3CartJobRun: back to it if it is in the scene stack (Retry
 * on its result), else on top of the current scene. Its result's OK returns to the hub. */
void gen3_cart_ui_run_job(PokemonFap* pokemon_fap, Gen3CartJob job, const char* arg);

/* Sets Gen3CartResult's header, text, Retry and the scene its OK returns to, from a job's end
 * (gen3_cart_screen_result); call before scene_manager_next_scene(..., Gen3CartResult) */
void gen3_cart_ui_set_result(
    PokemonFap* pokemon_fap,
    Gen3CartJob job,
    Gen3CartStep step,
    Gen3CartResult r,
    bool dropped,
    uint32_t back);
