#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_session_furi.h>
#include <src/gen3/gen3_trade_sync.h>
#include <src/views/gen3_link_test.h>
#include <src/views/gen3_trade.h>

#include <furi_hal_random.h>
#include <stdlib.h>
#include <src/gen3/gen3_bulk_furi.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/scenes/include/pokemon_gen3_menu.h>
#include <src/views/gen3_species_select.h>

/* OK on the "Data missing" dialog. Kept clear of the menu items (scene ids) and the other Gen III
 * events. */
#define GEN3_MENU_EVENT_DATA_OK 0x47330101u

/* The "Data missing" dialog is up instead of the menu */
static bool gen3_menu_dialog_shown = false;

/* gen3_menu_status's buffer */
static Gen3ClubStatus gen3_menu_status_buf;

const Gen3ClubStatus* gen3_menu_status(PokemonFap* pokemon_fap, bool* log_ok, bool* cable_ok) {
    gen3_session_status(pokemon_fap->gen3_session, &gen3_menu_status_buf, log_ok, cable_ok);
    return &gen3_menu_status_buf;
}

static void gen3_menu_item_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    /* Highlight the same item when we come back to this menu */
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Menu, index);

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

/* The Link Test session and its view outlive the Link Test scene while the "Stop link?" dialog
 * is up, so that "Stay" returns to the running link. Every way out of the Link Test for good
 * (Back while not in the Cable Club, or "Stop") comes back to this menu, so it ends here. A trade
 * session and its trade screen live on behind every Gen III scene while it runs (D1), and end
 * here too once it doesn't: not started yet, or stopped after the GBA left. The sync stays until
 * the app exits, restarted (it keeps the block on a traded-away Pokemon, G15). Call with neither
 * view shown. */
static void gen3_menu_end_session(PokemonFap* pokemon_fap) {
    if(pokemon_fap->gen3_session) {
        gen3_session_free(pokemon_fap->gen3_session); /* stops the link, closes the log */
        pokemon_fap->gen3_session = NULL;
    }
    if(pokemon_fap->gen3_link_test) {
        gen3_link_test_free(
            pokemon_fap->view_dispatcher, AppViewGen3LinkTest, pokemon_fap->gen3_link_test);
        pokemon_fap->gen3_link_test = NULL;
    }
    if(pokemon_fap->gen3_trade_view) {
        gen3_trade_view_free(
            pokemon_fap->view_dispatcher, AppViewGen3Trade, pokemon_fap->gen3_trade_view);
        pokemon_fap->gen3_trade_view = NULL;
    }
    if(pokemon_fap->gen3_trade_sync) gen3_trade_sync_restart(pokemon_fap->gen3_trade_sync);
}

/* The trade screen's OK (Edit). App thread (view input): Gen3Trade handles the event. */
static void gen3_menu_trade_edit_callback(void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_TRADE_EVENT_EDIT);
}

/* A new trade session with its trade screen, and the sync the first time. The tick starts it
 * once the cable reads OK. A session left from before (one that stopped while this menu was
 * shown) goes first. */
static void gen3_menu_trade_alloc(PokemonFap* pokemon_fap) {
    gen3_menu_end_session(pokemon_fap); /* restarts a sync kept from an earlier session */

    pokemon_fap->gen3_session = gen3_session_alloc(pokemon_fap->gblink_handle);
    pokemon_fap->gen3_trade_view =
        gen3_trade_view_alloc(pokemon_fap->view_dispatcher, AppViewGen3Trade);
    gen3_trade_view_set_edit_callback(
        pokemon_fap->gen3_trade_view, gen3_menu_trade_edit_callback, pokemon_fap);
    if(!pokemon_fap->gen3_trade_sync) {
        pokemon_fap->gen3_trade_sync = malloc(sizeof(Gen3TradeSync));
        gen3_trade_sync_init(pokemon_fap->gen3_trade_sync);
    }
    pokemon_fap->gen3_trade_ended = false;
}

static void gen3_menu_dialog_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;

    if(result == DialogExResultCenter) {
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_MENU_EVENT_DATA_OK);
    }
}

/* Everything but the Link Test needs gen3_bulk.bin (format v2): the species, move and item names
 * and the learnsets. Deleting apps_assets/pokemon makes the Flipper unpack the app's assets again
 * on the next start; a reinstall alone doesn't if their signature still matches. */
void gen3_menu_show_data_missing(PokemonFap* pokemon_fap) {
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    /* From an editor scene, back to this menu first. From the menu itself this does nothing. */
    scene_manager_search_and_switch_to_previous_scene(
        pokemon_fap->scene_manager, PokemonSceneGen3Menu);

    dialog_ex_reset(dialog_ex);
    dialog_ex_set_header(dialog_ex, "Data missing", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog_ex,
        "Gen III data file missing.\nDelete apps_assets/pokemon\non the SD card, then reopen.",
        64,
        13,
        AlignCenter,
        AlignTop);
    dialog_ex_set_center_button_text(dialog_ex, "OK");
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, gen3_menu_dialog_callback);

    gen3_menu_dialog_shown = true;
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

static void gen3_notice_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;

    if(result == DialogExResultCenter) {
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_EVENT_NOTICE_OK);
    }
}

void gen3_menu_show_notice(PokemonFap* pokemon_fap, const char* header, const char* text) {
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    dialog_ex_reset(dialog_ex);
    dialog_ex_set_header(dialog_ex, header, 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(dialog_ex, text, 64, 13, AlignCenter, AlignTop);
    dialog_ex_set_center_button_text(dialog_ex, "OK");
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, gen3_notice_callback);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

static void gen3_menu_show_menu(PokemonFap* pokemon_fap) {
    gen3_menu_dialog_shown = false;
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

/* The Pokemon being edited lives until the app exits. Its nickname and moves come from the bulk
 * file, so it is created only once the file reads, and Trade, Edit, Load and Save need the file:
 * false if it can't be read. A read that failed since (the SD card pulled) closed it, so it is
 * opened again here first. */
bool gen3_menu_editor_ready(PokemonFap* pokemon_fap) {
    if(!gen3_bulk_ready()) gen3_bulk_furi_open();
    if(!gen3_bulk_ready()) return false;
    if(!pokemon_fap->gen3_editor) {
        pokemon_fap->gen3_editor = malloc(sizeof(Gen3Editor));
        if(!gen3_edit_init(pokemon_fap->gen3_editor, furi_hal_random_get())) {
            free(pokemon_fap->gen3_editor);
            pokemon_fap->gen3_editor = NULL;
        }
    }
    return pokemon_fap->gen3_editor != NULL;
}

void pokemon_scene_gen3_menu_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    /* The bulk data file is opened once, then kept open until the app exits (free_app closes
     * it). Until it opens, each visit to this menu tries again. A read that fails later (the SD
     * card pulled and put back) uninstalls it, so the next visit opens it again. */
    if(!gen3_bulk_ready()) gen3_bulk_furi_open();
    if(!pokemon_fap->gen3_species_select) {
        pokemon_fap->gen3_species_select =
            gen3_species_select_alloc(pokemon_fap->view_dispatcher, AppViewGen3SpeciesSelect);
    }

    /* A trade session runs on behind this menu (D1) */
    const bool trading = gen3_session_trading(pokemon_fap->gen3_session);

    submenu_reset(pokemon_fap->submenu);
    submenu_set_header(pokemon_fap->submenu, "Gen III");

    submenu_add_item(
        pokemon_fap->submenu, "Trade", PokemonSceneGen3Trade, gen3_menu_item_callback, pokemon_fap);

    submenu_add_item(
        pokemon_fap->submenu,
        "Edit Pokemon",
        PokemonSceneGen3Edit,
        gen3_menu_item_callback,
        pokemon_fap);

    submenu_add_item(
        pokemon_fap->submenu,
        "Load .pk3",
        PokemonSceneGen3Load,
        gen3_menu_item_callback,
        pokemon_fap);

    submenu_add_item(
        pokemon_fap->submenu,
        "Save .pk3",
        PokemonSceneGen3SaveName,
        gen3_menu_item_callback,
        pokemon_fap);

    /* TIM2 runs one link at a time: no Link Test while a trade session runs */
    if(!trading) {
        submenu_add_item(
            pokemon_fap->submenu,
            "Link Test",
            PokemonSceneGen3LinkTest,
            gen3_menu_item_callback,
            pokemon_fap);
    }

    submenu_set_selected_item(
        pokemon_fap->submenu,
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Menu));

    /* Switch away from the Link Test or trade view before freeing it */
    gen3_menu_show_menu(pokemon_fap);
    if(!trading) gen3_menu_end_session(pokemon_fap);
}

/* Edit Pokemon or Load .pk3 (scene): with a trade session running, the club holds the offer
 * first (O1); if it can't (a trade is pending, or a pickup is being retried), the trade screen
 * says why instead. begin_edit and the scene switch share this handler, so no tick runs between
 * them. */
static void gen3_menu_open_editor(PokemonFap* pokemon_fap, uint32_t scene) {
    if(!gen3_menu_editor_ready(pokemon_fap)) {
        gen3_menu_show_data_missing(pokemon_fap);
    } else if(pokemon_gen3_trade_begin_edit(pokemon_fap)) {
        scene_manager_next_scene(pokemon_fap->scene_manager, scene);
    } else {
        scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3Trade);
    }
}

bool pokemon_scene_gen3_menu_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    /* Only menu items and the dialog's OK. A Link Test refresh tick still queued when the Link
     * Test closed may arrive here too; ignore it. */
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == PokemonSceneGen3LinkTest) {
            scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
            consumed = true;
        } else if(event.event == PokemonSceneGen3Trade) {
            /* The tick needs the editor: it hands the GBA player to it and offers its Pokemon */
            if(!gen3_menu_editor_ready(pokemon_fap)) {
                gen3_menu_show_data_missing(pokemon_fap);
            } else {
                if(!gen3_session_trading(pokemon_fap->gen3_session)) {
                    gen3_menu_trade_alloc(pokemon_fap);
                }
                scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
            }
            consumed = true;
        } else if(event.event == PokemonSceneGen3Edit || event.event == PokemonSceneGen3Load) {
            gen3_menu_open_editor(pokemon_fap, event.event);
            consumed = true;
        } else if(event.event == PokemonSceneGen3SaveName) {
            if(gen3_menu_editor_ready(pokemon_fap)) {
                gen3_pk3_default_name(
                    &pokemon_fap->gen3_editor->pkm,
                    pokemon_fap->gen3_file_name,
                    sizeof(pokemon_fap->gen3_file_name));
                /* The text scene's mode, as the Gen III editor sets it for a nickname */
                scene_manager_set_scene_state(
                    pokemon_fap->scene_manager, PokemonSceneGen3Nickname, event.event);
                scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
            } else {
                gen3_menu_show_data_missing(pokemon_fap);
            }
            consumed = true;
        } else if(event.event == GEN3_MENU_EVENT_DATA_OK && gen3_menu_dialog_shown) {
            gen3_menu_show_menu(pokemon_fap);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack && gen3_menu_dialog_shown) {
        /* Back on the dialog returns to this menu, not to the main menu */
        gen3_menu_show_menu(pokemon_fap);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_menu_on_exit(void* context) {
    UNUSED(context);
}
