#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <furi.h>
#include <src/gen3/gen3_ub_data.h>

/* One Unbound box's 30 slots, or the party's 6, headed by its name: "01 BULBASAUR Lv5",
 * "05 EGG", "07 -- empty --", read from the Omega's backup one at a time. Read-only: choosing a
 * row does nothing yet. The scene state keeps the highlighted row. */

#define GEN3_CART_ITEM_OMEGA_SLOTS 0x47330580u

static void gen3_cart_omega_slots_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

void pokemon_scene_gen3_cart_omega_slots_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Submenu* submenu = pokemon_fap->submenu;
    Gen3Omega* o = gen3_cart_ui_omega(pokemon_fap);
    const uint8_t box = gen3_cart_ui_get(pokemon_fap)->box;
    const bool party = box == GEN3_SAVE_PARTY;
    char header[12];
    char label[GEN3_CART_SCREEN_ROW_SIZE];
    char name[GEN3_UB_NAME_LEN + 1u];
    Gen3UbSummary m;

    submenu_reset(submenu);
    if(party) {
        snprintf(header, sizeof(header), "Party");
    } else {
        snprintf(header, sizeof(header), "BOX %u", (unsigned)box + 1u);
    }
    submenu_set_header(submenu, header);
    if(o && gen3_omega_browsable(o)) {
        const uint8_t rows = party ? GEN3_UB_PARTY_MAX : GEN3_UB_BOX_SLOTS;
        for(uint8_t k = 0; k < rows; k++) {
            const Gen3CartMon r = gen3_omega_summary(o, box, k, &m);
            name[0] = '\0';
            if(r == Gen3CartMonOk && !m.egg) gen3_ub_species_name(m.species, name);
            gen3_cart_screen_ub_row(k, &m, name, r, label);
            submenu_add_item(
                submenu,
                label,
                GEN3_CART_ITEM_OMEGA_SLOTS + k,
                gen3_cart_omega_slots_callback,
                pokemon_fap);
        }
        submenu_set_selected_item(
            submenu,
            scene_manager_get_scene_state(
                pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaSlots));
    }
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_cart_omega_slots_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    if(event.type != SceneManagerEventTypeCustom) return false; /* Back: the boxes */
    if(event.event >= GEN3_CART_ITEM_OMEGA_SLOTS &&
       event.event < GEN3_CART_ITEM_OMEGA_SLOTS + GEN3_UB_BOX_SLOTS) {
        /* Read-only for now: the row only stays highlighted */
        scene_manager_set_scene_state(
            pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaSlots, event.event);
        return true;
    }
    return false;
}

void pokemon_scene_gen3_cart_omega_slots_on_exit(void* context) {
    UNUSED(context);
}
