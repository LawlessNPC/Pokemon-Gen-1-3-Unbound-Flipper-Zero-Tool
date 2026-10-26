#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_session_furi.h>
#include <src/views/gen3_link_test.h>

static void gen3_menu_item_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    /* Highlight the same item when we come back to this menu */
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Menu, index);

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

/* The Link Test session and its view outlive the Link Test scene while the "Stop link?" dialog
 * is up, so that "Stay" returns to the running link. Every way out of the Link Test for good
 * (Back while not in the Cable Club, or "Stop") comes back to this menu, so it ends here. */
static void gen3_menu_end_link_test(PokemonFap* pokemon_fap) {
    if(pokemon_fap->gen3_session) {
        gen3_session_free(pokemon_fap->gen3_session); /* stops the link, closes the log */
        pokemon_fap->gen3_session = NULL;
    }
    if(pokemon_fap->gen3_link_test) {
        gen3_link_test_free(
            pokemon_fap->view_dispatcher, AppViewGen3LinkTest, pokemon_fap->gen3_link_test);
        pokemon_fap->gen3_link_test = NULL;
    }
}

void pokemon_scene_gen3_menu_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    submenu_reset(pokemon_fap->submenu);
    submenu_set_header(pokemon_fap->submenu, "Gen III");

    submenu_add_item(
        pokemon_fap->submenu,
        "Link Test",
        PokemonSceneGen3LinkTest,
        gen3_menu_item_callback,
        pokemon_fap);

    submenu_set_selected_item(
        pokemon_fap->submenu,
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Menu));

    /* Switch away from the Link Test view before freeing it */
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
    gen3_menu_end_link_test(pokemon_fap);
}

bool pokemon_scene_gen3_menu_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    /* Only menu items. A Link Test refresh tick still queued when the Link Test closed may
     * arrive here too; ignore it. */
    if(event.type == SceneManagerEventTypeCustom && event.event == PokemonSceneGen3LinkTest) {
        scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_menu_on_exit(void* context) {
    UNUSED(context);
}
