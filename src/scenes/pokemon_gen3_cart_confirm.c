#include <src/scenes/include/pokemon_gen3_cart.h>

#include <furi.h>

/* Leave?: leaving the app drops the edits not written. Stay (or Back) returns to the scene below:
 * the hub, or the instructions after a Retry. */

static char gen3_cart_confirm_text[GEN3_CART_SCREEN_LEAVE_SIZE];

void pokemon_scene_gen3_cart_confirm_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    gen3_cart_screen_leave(gen3_cart_ui_changes(pokemon_fap), gen3_cart_confirm_text);
    gen3_cart_ui_dialog(pokemon_fap);
    dialog_ex_set_header(dialog_ex, "Leave?", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(dialog_ex, gen3_cart_confirm_text, 64, 14, AlignCenter, AlignTop);
    dialog_ex_set_left_button_text(dialog_ex, "Stay");
    dialog_ex_set_right_button_text(dialog_ex, "Leave");
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

bool pokemon_scene_gen3_cart_confirm_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GEN3_CART_EVENT_RIGHT) {
            gen3_cart_ui_leave(pokemon_fap);
            consumed = true;
        } else if(event.event == GEN3_CART_EVENT_LEFT) {
            scene_manager_previous_scene(pokemon_fap->scene_manager);
            consumed = true;
        }
    }
    /* Back is the scene manager's: Stay */

    return consumed;
}

void pokemon_scene_gen3_cart_confirm_on_exit(void* context) {
    UNUSED(context);
}
