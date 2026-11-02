#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <furi.h>

/* The Unbound editor: the ui's Gen3UbEditor as rows (gen3_cart_screen_ub_edit_rows), each opening
 * what changes it: the pick scene (Gen3CartOmegaPick) for the species, the held item, the moves,
 * the nature, the ability, shininess, the IVs and the EVs; the number scene (Gen3CartOmegaNumber)
 * for the level; the text scene (Gen3CartOmegaNickname) for the nickname. Back puts the Pokemon
 * into the Omega's slot (box, slot): an edit at once, only if it changed; a new Pokemon once Put?
 * (Gen3CartOmegaPut) is confirmed. Nothing reaches the card until Write changes. The scene state
 * keeps the highlighted row. */

/* Row k's item */
#define GEN3_CART_ITEM_OMEGA_EDIT 0x473305A0u
enum {
    Gen3CartUbRowSpecies,
    Gen3CartUbRowLevel,
    Gen3CartUbRowNickname,
    Gen3CartUbRowItem,
    Gen3CartUbRowMove1,
    Gen3CartUbRowMove4 = Gen3CartUbRowMove1 + 3,
    Gen3CartUbRowNature,
    Gen3CartUbRowAbility,
    Gen3CartUbRowShiny,
    Gen3CartUbRowIvs,
    Gen3CartUbRowEvs,
};
_Static_assert(Gen3CartUbRowEvs + 1 == GEN3_CART_SCREEN_UB_ROWS, "a row each");

/* The rows made on each entry (the submenu keeps copies), and the header */
static char gen3_cart_omega_edit_rows[GEN3_CART_SCREEN_UB_ROWS][GEN3_CART_SCREEN_ROW_SIZE];
static char gen3_cart_omega_edit_header[GEN3_CART_SCREEN_HEADER_SIZE];

static void gen3_cart_omega_edit_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

void pokemon_scene_gen3_cart_omega_edit_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    const Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Submenu* submenu = pokemon_fap->submenu;

    if(ui->box == GEN3_SAVE_PARTY) {
        snprintf(
            gen3_cart_omega_edit_header,
            sizeof(gen3_cart_omega_edit_header),
            "Party slot %u",
            (unsigned)ui->slot + 1u);
    } else {
        snprintf(
            gen3_cart_omega_edit_header,
            sizeof(gen3_cart_omega_edit_header),
            "BOX %u slot %u",
            (unsigned)ui->box + 1u,
            (unsigned)ui->slot + 1u);
    }
    gen3_cart_screen_ub_edit_rows(&ui->ub_edit, gen3_cart_omega_edit_rows);
    submenu_reset(submenu);
    submenu_set_header(submenu, gen3_cart_omega_edit_header);
    for(uint32_t k = 0; k < GEN3_CART_SCREEN_UB_ROWS; k++) {
        submenu_add_item(
            submenu,
            gen3_cart_omega_edit_rows[k],
            GEN3_CART_ITEM_OMEGA_EDIT + k,
            gen3_cart_omega_edit_callback,
            pokemon_fap);
    }
    submenu_set_selected_item(
        submenu,
        GEN3_CART_ITEM_OMEGA_EDIT +
            scene_manager_get_scene_state(
                pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaEdit));
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

/* A list for the row: its letters (or its one list) first */
static void gen3_cart_omega_edit_pick(PokemonFap* pokemon_fap, Gen3CartUbPick pick) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    ui->ub_pick = (uint8_t)pick;
    ui->ub_page = GEN3_CART_UB_LETTERS;
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaPick, 0);
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaPick);
}

static void gen3_cart_omega_edit_row(PokemonFap* pokemon_fap, uint32_t row) {
    SceneManager* scene_manager = pokemon_fap->scene_manager;

    switch(row) {
    case Gen3CartUbRowSpecies:
        gen3_cart_omega_edit_pick(pokemon_fap, Gen3CartUbPickSpecies);
        break;
    case Gen3CartUbRowLevel:
        /* the number scene's state 0: the level */
        scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartOmegaNumber, 0);
        scene_manager_next_scene(scene_manager, PokemonSceneGen3CartOmegaNumber);
        break;
    case Gen3CartUbRowNickname:
        scene_manager_next_scene(scene_manager, PokemonSceneGen3CartOmegaNickname);
        break;
    case Gen3CartUbRowItem:
        gen3_cart_omega_edit_pick(pokemon_fap, Gen3CartUbPickItem);
        break;
    case Gen3CartUbRowNature:
        gen3_cart_omega_edit_pick(pokemon_fap, Gen3CartUbPickNature);
        break;
    case Gen3CartUbRowAbility:
        gen3_cart_omega_edit_pick(pokemon_fap, Gen3CartUbPickAbility);
        break;
    case Gen3CartUbRowShiny:
        gen3_cart_omega_edit_pick(pokemon_fap, Gen3CartUbPickShiny);
        break;
    case Gen3CartUbRowIvs:
        gen3_cart_omega_edit_pick(pokemon_fap, Gen3CartUbPickIvs);
        break;
    case Gen3CartUbRowEvs:
        gen3_cart_omega_edit_pick(pokemon_fap, Gen3CartUbPickEvs);
        break;
    default:
        /* Move 1 .. Move 4 */
        gen3_cart_omega_edit_pick(
            pokemon_fap, (Gen3CartUbPick)(Gen3CartUbPickMove1 + (row - Gen3CartUbRowMove1)));
        break;
    }
}

/* Back: the Pokemon into its slot, then the list */
static void gen3_cart_omega_edit_back(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3CartMon m = Gen3CartMonOk;

    /* A picture job holds the Omega: wait for it before the put; the edit stays if it won't end */
    if(!gen3_cart_ui_picture_wait(pokemon_fap)) {
        gen3_cart_ui_busy_notice(pokemon_fap, PokemonSceneGen3CartOmegaEdit);
        return;
    }
    Gen3Omega* o = gen3_cart_ui_omega(pokemon_fap);
    if(ui->ub_new) {
        gen3_cart_ui_confirm(pokemon_fap, PokemonSceneGen3CartOmegaPut);
        return;
    }
    if(ui->ub_edit.changed) {
        m = o ? gen3_omega_mon_put(o, ui->box, ui->slot, &ui->ub_edit.mon, false) :
                Gen3CartMonBadArg;
        ui->ub_edit.changed = false;
    }
    if(m == Gen3CartMonOk) {
        scene_manager_search_and_switch_to_previous_scene(
            pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaSlots);
    } else {
        gen3_cart_ui_omega_mon_result(pokemon_fap, true, m);
    }
}

bool pokemon_scene_gen3_cart_omega_edit_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    if(event.type == SceneManagerEventTypeBack) {
        gen3_cart_ui_trace(pokemon_fap, "back omega_edit put");
        gen3_cart_omega_edit_back(pokemon_fap);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event < GEN3_CART_ITEM_OMEGA_EDIT ||
       event.event >= GEN3_CART_ITEM_OMEGA_EDIT + GEN3_CART_SCREEN_UB_ROWS) {
        return false;
    }
    const uint32_t row = event.event - GEN3_CART_ITEM_OMEGA_EDIT;
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaEdit, row);
    gen3_cart_omega_edit_row(pokemon_fap, row);
    return true;
}

void pokemon_scene_gen3_cart_omega_edit_on_exit(void* context) {
    UNUSED(context);
}
