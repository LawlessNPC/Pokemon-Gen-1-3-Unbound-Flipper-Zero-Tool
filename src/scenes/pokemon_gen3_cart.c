#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/scenes/include/pokemon_gen3_menu.h>

/* The Cartridge app's hub, its first scene. It is registered as Gen3Menu
 * (pokemon_cart_scene_config.h): the Gen III editor scenes search back to Gen3Menu, and in this
 * app that is this hub. For now it only opens the Gen III editor. */

static void gen3_cart_item_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

void pokemon_scene_gen3_cart_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;

    submenu_reset(pokemon_fap->submenu);
    submenu_set_header(pokemon_fap->submenu, "Cartridge");
    submenu_add_item(
        pokemon_fap->submenu,
        "Edit Pokemon",
        PokemonSceneGen3Edit,
        gen3_cart_item_callback,
        pokemon_fap);

    gen3_menu_show_menu(pokemon_fap);
}

bool pokemon_scene_gen3_cart_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == PokemonSceneGen3Edit) {
            if(gen3_menu_editor_ready(pokemon_fap)) {
                scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3Edit);
            } else {
                gen3_menu_show_data_missing(pokemon_fap);
            }
            consumed = true;
        } else if(event.event == GEN3_MENU_EVENT_DATA_OK && gen3_menu_dialog_up()) {
            gen3_menu_show_menu(pokemon_fap);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(gen3_menu_dialog_up()) {
            /* Back on the dialog returns to the hub */
            gen3_menu_show_menu(pokemon_fap);
            consumed = true;
        }
        /* Not consumed: nothing is below the hub, so the view dispatcher stops and the app
         * exits */
    }

    return consumed;
}

void pokemon_scene_gen3_cart_on_exit(void* context) {
    UNUSED(context);
}
