#include <gui/modules/dialog_ex.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_bulk_furi.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_session_furi.h>
#include <src/gen3/gen3_trade_sync.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* End trade (T13: D9, D10), over the Gen III menu, which has chosen what to show: the scene state
 * is a Gen3TradeEndAction other than Stop.
 * - Saving: the GBA is saving; OK returns.
 * - Ask: Stay returns, End asks the GBA to walk out and shows the trade screen to watch it.
 * - Pickup (T12 review (f)): a received Pokemon waits for the SD card. Retry tries the card again
 *   and returns once it is picked up; End anyway stops the session and loses that Pokemon.
 * Back is Stay (or OK). The session runs on behind it, and the tick polls here (the dialog doesn't
 * edit), so a NoData pickup is retried meanwhile too. */

static void gen3_trade_end_dialog_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, result);
}

void pokemon_scene_gen3_trade_end_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;
    const uint32_t action =
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3TradeEnd);

    dialog_ex_reset(dialog_ex);
    if(action == Gen3TradeEndSaving) {
        dialog_ex_set_header(dialog_ex, "The GBA is saving", 64, 0, AlignCenter, AlignTop);
        dialog_ex_set_text(
            dialog_ex,
            "Wait until it's done,\nthen end the trade.",
            64,
            12,
            AlignCenter,
            AlignTop);
        dialog_ex_set_center_button_text(dialog_ex, "OK");
    } else if(action == Gen3TradeEndPickup) {
        dialog_ex_set_header(dialog_ex, "End trade?", 64, 0, AlignCenter, AlignTop);
        dialog_ex_set_text(
            dialog_ex,
            "A traded Pokemon\ncouldn't be loaded.\nReinsert the SD card.",
            64,
            12,
            AlignCenter,
            AlignTop);
        dialog_ex_set_left_button_text(dialog_ex, "Retry");
        dialog_ex_set_right_button_text(dialog_ex, "End anyway");
    } else {
        dialog_ex_set_header(dialog_ex, "End trade?", 64, 0, AlignCenter, AlignTop);
        dialog_ex_set_text(
            dialog_ex,
            "The GBA walks out of\nthe Trade Center by\nitself, then the link\nstops.",
            64,
            12,
            AlignCenter,
            AlignTop);
        dialog_ex_set_left_button_text(dialog_ex, "Stay");
        dialog_ex_set_right_button_text(dialog_ex, "End");
    }
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, gen3_trade_end_dialog_callback);

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

bool pokemon_scene_gen3_trade_end_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    SceneManager* scene_manager = pokemon_fap->scene_manager;
    const uint32_t action = scene_manager_get_scene_state(scene_manager, PokemonSceneGen3TradeEnd);

    /* Back: as Stay or OK, the session still running */
    if(event.type == SceneManagerEventTypeBack) return scene_manager_previous_scene(scene_manager);
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == DialogExResultLeft && action == Gen3TradeEndPickup) {
        /* Retry: open the bulk file again now, then poll once. Picked up: back to the menu,
         * whose End trade (or Back) ends the session now. Still unreadable: stay. */
        if(!gen3_bulk_ready()) gen3_bulk_furi_open();
        if(!pokemon_fap->gen3_session || pokemon_gen3_trade_settled(pokemon_fap, NULL)) {
            scene_manager_previous_scene(scene_manager);
        }
        return true;
    }
    if(event.event == DialogExResultLeft || event.event == DialogExResultCenter) {
        /* Stay, or OK on "The GBA is saving" */
        return scene_manager_previous_scene(scene_manager);
    }
    if(event.event != DialogExResultRight || !pokemon_fap->gen3_session) return false;

    if(action == Gen3TradeEndPickup) {
        /* End anyway, the user's choice: the received Pokemon goes with the club. No trade can
         * be pending while a pickup waits (nothing is offered), so R6 holds; checked anyway. */
        if(gen3_trade_stop_allowed(gen3_menu_status(pokemon_fap, NULL, NULL))) {
            gen3_session_stop(pokemon_fap->gen3_session);
            /* The menu frees it next; meanwhile the tick doesn't start it again */
            pokemon_fap->gen3_trade_ended = true;
            scene_manager_previous_scene(scene_manager);
        }
        return true;
    }

    /* End (D9): the club asks the GBA to walk out; the trade screen shows it leave, and the
     * session stops once the GBA has closed the link (the tick, at left) */
    gen3_session_request_leave(pokemon_fap->gen3_session);
    scene_manager_search_and_switch_to_previous_scene(scene_manager, PokemonSceneGen3Menu);
    scene_manager_next_scene(scene_manager, PokemonSceneGen3Trade);
    return true;
}

void pokemon_scene_gen3_trade_end_on_exit(void* context) {
    UNUSED(context);
}
