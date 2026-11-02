#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <furi.h>
#include <src/gen3/gen3_ub_data.h>
#include <src/gen3/gen3_ub_data_furi.h>

/* One Unbound box's 30 slots, or the party's 6, headed by its name: "01 BULBASAUR Lv5",
 * "05 EGG", "07 -- empty --", with '*' after a slot changed and not written, read from the
 * Omega's working copy one at a time. Choosing a Pokemon opens it in the Unbound editor
 * (Gen3CartOmegaEdit), which puts it back if it was edited; an egg isn't opened. Choosing an
 * empty box slot offers New Pokemon (an empty party slot offers nothing: new Pokemon go into the
 * boxes), whose species comes first, then the editor, then Put? The scene state keeps the
 * highlighted row. */

#define GEN3_CART_ITEM_OMEGA_SLOTS 0x47330580u
#define GEN3_CART_ITEM_OMEGA_NEW 0x4733059Eu

/* The free box slot's menu is up instead of the list */
static bool gen3_cart_omega_slots_menu_up = false;

static void gen3_cart_omega_slots_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

static void gen3_cart_omega_slots_list(PokemonFap* pokemon_fap) {
    Submenu* submenu = pokemon_fap->submenu;
    /* The picker's last fetch may still run (Back from the letters of New Pokemon): wait for it,
     * or the list would find no Omega and go to the hub */
    Gen3Omega* o = gen3_cart_ui_omega_idle(pokemon_fap);
    const uint8_t box = gen3_cart_ui_get(pokemon_fap)->box;
    const bool party = box == GEN3_SAVE_PARTY;
    char header[12];
    char label[GEN3_CART_SCREEN_ROW_SIZE];
    char name[GEN3_UB_NAME_LEN + 1u];
    Gen3UbSummary m;

    gen3_cart_omega_slots_menu_up = false;
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
            gen3_cart_screen_ub_mark(gen3_omega_mon_changed(o, box, k), label);
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
    } else {
        /* No backup to list: the hub */
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_CART_EVENT_HUB);
    }
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

/* A free box slot: what to put there */
static void gen3_cart_omega_slots_menu(PokemonFap* pokemon_fap) {
    const Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Submenu* submenu = pokemon_fap->submenu;
    char header[GEN3_CART_SCREEN_HEADER_SIZE]; /* "BOX 24 slot 30" */

    gen3_cart_omega_slots_menu_up = true;
    snprintf(
        header, sizeof(header), "BOX %u slot %u", (unsigned)ui->box + 1u, (unsigned)ui->slot + 1u);
    submenu_reset(submenu);
    submenu_set_header(submenu, header);
    submenu_add_item(
        submenu,
        "New Pokemon",
        GEN3_CART_ITEM_OMEGA_NEW,
        gen3_cart_omega_slots_callback,
        pokemon_fap);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

/* The Unbound data file, opened again if it wasn't (the SD card was out at the start) */
static bool gen3_cart_omega_slots_data(void) {
    if(!gen3_ub_data_ok()) gen3_ub_data_furi_open();
    return gen3_ub_data_ok();
}

/* What keeps the editor shut, for any slot: the data file, and a game file not 2.1.1.1's, whose
 * slots gen3_omega_mon_put refuses. NULL: none. */
static const char* gen3_cart_omega_slots_shut(const Gen3Omega* o) {
    if(!gen3_cart_omega_slots_data()) return gen3_cart_screen_mon_text(Gen3CartMonNoData, NULL);
    if(!o->verified) return gen3_cart_result_text(Gen3CartRefused);
    return NULL;
}

/* Row k chosen */
static void gen3_cart_omega_slots_open(PokemonFap* pokemon_fap, uint8_t k) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3Omega* o = gen3_cart_ui_omega(pokemon_fap);
    Gen3UbSummary s;

    ui->slot = k;
    ui->ub_new = false;
    if(!o) return; /* a job runs: the list stays */
    Gen3CartMon r = gen3_omega_summary(o, ui->box, k, &s);
    if(r == Gen3CartMonEmpty) {
        if(ui->box != GEN3_SAVE_PARTY) gen3_cart_omega_slots_menu(pokemon_fap);
        return;
    }
    if(r == Gen3CartMonOk && s.egg) r = Gen3CartMonEgg;
    const char* shut = r == Gen3CartMonOk ? gen3_cart_omega_slots_shut(o) : NULL;
    if(shut) {
        gen3_cart_ui_omega_notice(pokemon_fap, GEN3_CART_SCREEN_CANT_EDIT, shut);
        return;
    }
    if(r == Gen3CartMonOk) r = gen3_omega_mon_get(o, ui->box, k, &ui->ub_mon);
    if(r != Gen3CartMonOk) {
        gen3_cart_ui_omega_mon_result(pokemon_fap, false, r);
        return;
    }
    gen3_ub_edit_load(&ui->ub_edit, &ui->ub_mon, o->has_trainer ? &o->trainer : NULL);
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaEdit, 0);
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaEdit);
}

/* New Pokemon: its OT is a party Pokemon's (gen3_omega_owner), so there must be one; then its
 * species */
static void gen3_cart_omega_slots_new(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3Omega* o = gen3_cart_ui_omega(pokemon_fap);

    if(!o) return;
    const char* shut = gen3_cart_omega_slots_shut(o);
    if(shut) {
        gen3_cart_ui_omega_notice(pokemon_fap, GEN3_CART_SCREEN_CANT_PUT, shut);
        return;
    }
    if(!gen3_omega_owner(o, &ui->ub_mon)) {
        gen3_cart_ui_omega_notice(pokemon_fap, "New Pokemon", GEN3_CART_SCREEN_UB_NO_OWNER);
        return;
    }
    ui->ub_pick = Gen3CartUbPickNew;
    ui->ub_page = GEN3_CART_UB_LETTERS;
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaPick, 0);
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaPick);
}

void pokemon_scene_gen3_cart_omega_slots_on_enter(void* context) {
    gen3_cart_omega_slots_list(context);
}

bool pokemon_scene_gen3_cart_omega_slots_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    SceneManager* scene_manager = pokemon_fap->scene_manager;

    if(event.type == SceneManagerEventTypeBack) {
        /* The free slot's menu: the list again; the list: the boxes */
        if(!gen3_cart_omega_slots_menu_up) return false;
        gen3_cart_omega_slots_list(pokemon_fap);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event == GEN3_CART_EVENT_HUB) {
        scene_manager_search_and_switch_to_previous_scene(scene_manager, PokemonSceneGen3Cart);
        return true;
    }
    if(event.event == GEN3_CART_ITEM_OMEGA_NEW) {
        if(gen3_cart_omega_slots_menu_up) gen3_cart_omega_slots_new(pokemon_fap);
        return true;
    }
    if(event.event >= GEN3_CART_ITEM_OMEGA_SLOTS &&
       event.event < GEN3_CART_ITEM_OMEGA_SLOTS + GEN3_UB_BOX_SLOTS) {
        if(gen3_cart_omega_slots_menu_up) return true; /* a row's press still queued */
        scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartOmegaSlots, event.event);
        gen3_cart_omega_slots_open(
            pokemon_fap, (uint8_t)(event.event - GEN3_CART_ITEM_OMEGA_SLOTS));
        return true;
    }
    return false;
}

void pokemon_scene_gen3_cart_omega_slots_on_exit(void* context) {
    UNUSED(context);
}
