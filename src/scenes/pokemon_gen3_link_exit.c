#include <gui/modules/dialog_ex.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_session_furi.h>

static void pokemon_scene_gen3_link_exit_dialog_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, result);
}

void pokemon_scene_gen3_link_exit_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    // Clean view
    dialog_ex_reset(dialog_ex);

    dialog_ex_set_left_button_text(dialog_ex, "Stay");
    dialog_ex_set_right_button_text(dialog_ex, "Stop");
    dialog_ex_set_header(dialog_ex, "Stop link?", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog_ex,
        "Your GBA will show a link\nerror and need a restart.\nIt saved before linking.",
        64,
        12,
        AlignCenter,
        AlignTop);
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, pokemon_scene_gen3_link_exit_dialog_callback);

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

bool pokemon_scene_gen3_link_exit_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == DialogExResultLeft) {
            /* Stay: back to the Link Test, whose session never stopped */
            consumed = scene_manager_previous_scene(pokemon_fap->scene_manager);
        } else if(event.event == DialogExResultRight) {
            /* Stop: the Gen III menu frees the stopped session */
            gen3_session_stop(pokemon_fap->gen3_session);
            consumed = scene_manager_search_and_switch_to_previous_scene(
                pokemon_fap->scene_manager, PokemonSceneGen3Menu);
            /* NOTE: The above should never fail */
            furi_check(consumed);
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_link_exit_on_exit(void* context) {
    UNUSED(context);
}
