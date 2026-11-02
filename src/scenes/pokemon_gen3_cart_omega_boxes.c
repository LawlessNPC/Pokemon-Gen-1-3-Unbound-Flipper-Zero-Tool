#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <furi.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* An EZ-Flash Omega's Unbound save: the party with its count, then the 24 boxes (gen3_omega.h).
 * Choosing one lists its slots, where the Unbound editor opens. The scene state keeps the
 * highlighted row. */

/* Row k's item: 0 the party, 1..24 the boxes */
#define GEN3_CART_ITEM_OMEGA_BOXES 0x47330560u

static void gen3_cart_omega_boxes_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

void pokemon_scene_gen3_cart_omega_boxes_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Submenu* submenu = pokemon_fap->submenu;
    Gen3Omega* o = gen3_cart_ui_omega(pokemon_fap);
    char label[20];

    submenu_reset(submenu);
    submenu_set_header(submenu, "Party and boxes");
    if(o && gen3_omega_browsable(o)) {
        snprintf(label, sizeof(label), "Party (%u/6)", (unsigned)gen3_omega_party_count(o));
        submenu_add_item(
            submenu, label, GEN3_CART_ITEM_OMEGA_BOXES, gen3_cart_omega_boxes_callback, pokemon_fap);
        for(uint8_t box = 0; box < GEN3_UB_BOXES; box++) {
            snprintf(label, sizeof(label), "BOX %u", (unsigned)box + 1u);
            submenu_add_item(
                submenu,
                label,
                GEN3_CART_ITEM_OMEGA_BOXES + 1u + box,
                gen3_cart_omega_boxes_callback,
                pokemon_fap);
        }
        submenu_set_selected_item(
            submenu,
            scene_manager_get_scene_state(
                pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaBoxes));
    } else {
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_CART_EVENT_HUB);
    }
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_cart_omega_boxes_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    SceneManager* scene_manager = pokemon_fap->scene_manager;

    if(event.type != SceneManagerEventTypeCustom) return false; /* Back: the hub */
    if(event.event == GEN3_CART_EVENT_HUB) {
        scene_manager_search_and_switch_to_previous_scene(scene_manager, PokemonSceneGen3Cart);
        return true;
    }
    if(event.event < GEN3_CART_ITEM_OMEGA_BOXES ||
       event.event > GEN3_CART_ITEM_OMEGA_BOXES + GEN3_UB_BOXES) {
        return false;
    }
    const uint32_t row = event.event - GEN3_CART_ITEM_OMEGA_BOXES;
    scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartOmegaBoxes, event.event);
    gen3_cart_ui_get(pokemon_fap)->box = row == 0 ? GEN3_SAVE_PARTY : (uint8_t)(row - 1u);
    scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartOmegaSlots, 0);
    scene_manager_next_scene(scene_manager, PokemonSceneGen3CartOmegaSlots);
    return true;
}

void pokemon_scene_gen3_cart_omega_boxes_on_exit(void* context) {
    UNUSED(context);
}
