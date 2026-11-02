#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <furi.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_rules.h>
#include <src/gen3/gen3_ub_data.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* The Unbound editor's lists, one scene for all of them; which one is the ui's ub_pick
 * (Gen3CartUbPick), set before entering.
 * - The species, the held item and the moves: a letter list (gen3_cart_screen_ub_pages: a page
 *   per first letter, a long letter in pages of its own), then the names on the page chosen
 *   (ub_page), from the sorted lists. The held item and Move 2..4 also offer None at the top.
 *   Back on the names returns to the letters. A move in another slot isn't offered again.
 * - The natures, the species' abilities by name (slots it doesn't have left out; " (H)" for the
 *   hidden one), Shiny Yes or No, IVs All 31 or All 0, and the six EVs, each to the number scene.
 * A choice changes the editor's Pokemon (gen3_ub_edit.h) and returns to the editor, but for New
 * Pokemon's species, which makes the Pokemon (gen3_ub_new_box_mon, level 5, the OT of
 * gen3_omega_owner) and opens the editor on it. One the editor refuses (no PID has that nature,
 * shininess or ability for this OT, or the data couldn't be read) says so and changes nothing. The
 * scene state keeps the letter or EV chosen last (its row's item), to highlight it again; 0 (as the
 * editor enters it) highlights the Pokemon's own. */

#define GEN3_CART_ITEM_UB_PICK 0x47330600u /* + a page, or a value (a nature, an ability slot) */
#define GEN3_CART_ITEM_UB_NONE 0x473306FFu
#define GEN3_CART_ITEM_UB_NAME 0x47331000u /* + a list index */
#define GEN3_CART_ITEM_UB_NAMES 0x800u /* past every list's length */
#define GEN3_CART_UB_EVENT_NO_DATA 0x473305BEu /* to itself, from on_enter */
#define GEN3_CART_UB_LEVEL 5u /* a new Pokemon's */

/* The letter pages of the list last shown, kept: the data file doesn't change while the app runs */
static Gen3CartScreenUbPage gen3_cart_omega_pages[GEN3_CART_SCREEN_UB_PAGES];
static size_t gen3_cart_omega_page_count = 0;
static int gen3_cart_omega_pages_of = -1; /* the Gen3UbSort they are of; -1 none */
/* gen3_menu_show_notice is up over the list */
static bool gen3_cart_omega_pick_notice = false;
static char gen3_cart_omega_pick_header[24];

static const char* const gen3_cart_omega_pick_stats[Gen3StatCount] =
    {"HP", "Atk", "Def", "Spe", "SpA", "SpD"};

static void gen3_cart_omega_pick_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

static void gen3_cart_omega_pick_add(PokemonFap* pokemon_fap, const char* label, uint32_t item) {
    submenu_add_item(
        pokemon_fap->submenu, label, item, gen3_cart_omega_pick_callback, pokemon_fap);
}

/* The sorted list a lettered pick shows; false for the others */
static bool gen3_cart_omega_pick_sort(uint8_t pick, Gen3UbSort* s) {
    switch(pick) {
    case Gen3CartUbPickSpecies:
    case Gen3CartUbPickNew:
        *s = Gen3UbSortSpecies;
        return true;
    case Gen3CartUbPickItem:
        *s = Gen3UbSortItems;
        return true;
    case Gen3CartUbPickMove1:
    case Gen3CartUbPickMove2:
    case Gen3CartUbPickMove3:
    case Gen3CartUbPickMove4:
        *s = Gen3UbSortMoves;
        return true;
    default:
        return false;
    }
}

/* The id the editor's Pokemon has for a lettered pick: its species, item or move; 0 for none */
static uint16_t gen3_cart_omega_pick_current(const Gen3CartUi* ui) {
    const Gen3UbMon* m = &ui->ub_edit.mon;
    uint16_t moves[4];

    switch(ui->ub_pick) {
    case Gen3CartUbPickSpecies:
        return gen3_ub_mon_species(m);
    case Gen3CartUbPickItem:
        return gen3_ub_mon_item(m);
    case Gen3CartUbPickMove1:
    case Gen3CartUbPickMove2:
    case Gen3CartUbPickMove3:
    case Gen3CartUbPickMove4:
        gen3_ub_mon_moves(m, moves);
        return moves[ui->ub_pick - Gen3CartUbPickMove1];
    default:
        return 0;
    }
}

/* "Pokemon", "Held item", "Move 2", ... */
static const char* gen3_cart_omega_pick_title(uint8_t pick) {
    static const char* const titles[] = {
        [Gen3CartUbPickSpecies] = "Pokemon",
        [Gen3CartUbPickNew] = "New Pokemon",
        [Gen3CartUbPickItem] = "Held item",
        [Gen3CartUbPickMove1] = "Move 1",
        [Gen3CartUbPickMove2] = "Move 2",
        [Gen3CartUbPickMove3] = "Move 3",
        [Gen3CartUbPickMove4] = "Move 4",
        [Gen3CartUbPickNature] = "Nature",
        [Gen3CartUbPickAbility] = "Ability",
        [Gen3CartUbPickShiny] = "Shiny",
        [Gen3CartUbPickIvs] = "IVs",
        [Gen3CartUbPickEvs] = "EVs",
    };
    return pick < COUNT_OF(titles) ? titles[pick] : "";
}

/* The letters of s, computed the first time */
static bool gen3_cart_omega_pick_pages(Gen3UbSort s) {
    if(gen3_cart_omega_pages_of == (int)s) return true;
    gen3_cart_omega_page_count =
        gen3_cart_screen_ub_pages(s, gen3_cart_omega_pages, GEN3_CART_SCREEN_UB_PAGES);
    gen3_cart_omega_pages_of = gen3_cart_omega_page_count ? (int)s : -1;
    return gen3_cart_omega_page_count > 0;
}

/* The letter list, highlighting the one last chosen, else the current name's first letter, else
 * None. false if it can't be read. */
static bool gen3_cart_omega_pick_letters(PokemonFap* pokemon_fap, Gen3UbSort s) {
    const Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const uint16_t current = gen3_cart_omega_pick_current(ui);
    uint32_t selected =
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaPick);
    char name[GEN3_CART_SCREEN_UB_NAME_SIZE];

    if(!gen3_cart_omega_pick_pages(s)) return false;
    submenu_set_header(pokemon_fap->submenu, gen3_cart_omega_pick_title(ui->ub_pick));
    /* the item and Move 2..4 may be emptied; Move 1 never */
    if(ui->ub_pick == Gen3CartUbPickItem ||
       (ui->ub_pick > Gen3CartUbPickMove1 && ui->ub_pick <= Gen3CartUbPickMove4)) {
        gen3_cart_omega_pick_add(pokemon_fap, "None", GEN3_CART_ITEM_UB_NONE);
        if(selected == 0 && current == 0) selected = GEN3_CART_ITEM_UB_NONE;
    }
    name[0] = '\0';
    if(selected == 0 && current != 0) {
        switch(s) {
        case Gen3UbSortSpecies:
            gen3_ub_species_name(current, name);
            break;
        case Gen3UbSortMoves:
            gen3_ub_move_name(current, name);
            break;
        case Gen3UbSortItems:
            gen3_ub_item_name(current, name);
            break;
        }
    }
    for(size_t k = 0; k < gen3_cart_omega_page_count; k++) {
        gen3_cart_omega_pick_add(
            pokemon_fap, gen3_cart_omega_pages[k].label, GEN3_CART_ITEM_UB_PICK + k);
        if(selected == 0 && name[0] != '\0' && gen3_cart_omega_pages[k].label[0] == name[0]) {
            selected = GEN3_CART_ITEM_UB_PICK + k;
        }
    }
    submenu_set_selected_item(pokemon_fap->submenu, selected);
    return true;
}

/* The names on the page ub_page; false if one can't be read */
static bool gen3_cart_omega_pick_names(PokemonFap* pokemon_fap, Gen3UbSort s) {
    const Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const uint16_t current = gen3_cart_omega_pick_current(ui);
    char name[GEN3_CART_SCREEN_UB_NAME_SIZE];
    uint16_t moves[4] = {0, 0, 0, 0};
    uint16_t id = 0;
    uint32_t selected = 0;

    if(!gen3_cart_omega_pick_pages(s) || ui->ub_page >= gen3_cart_omega_page_count) return false;
    const Gen3CartScreenUbPage* p = &gen3_cart_omega_pages[ui->ub_page];
    snprintf(
        gen3_cart_omega_pick_header,
        sizeof(gen3_cart_omega_pick_header),
        "%s %s",
        gen3_cart_omega_pick_title(ui->ub_pick),
        p->label);
    submenu_set_header(pokemon_fap->submenu, gen3_cart_omega_pick_header);
    if(s == Gen3UbSortMoves) gen3_ub_mon_moves(&ui->ub_edit.mon, moves);
    for(uint16_t i = p->first; i < p->first + p->count; i++) {
        if(!gen3_cart_screen_ub_sorted_name(s, i, &id, name)) return false;
        bool elsewhere = false;
        for(uint8_t k = 0; s == Gen3UbSortMoves && k < 4u; k++) {
            if(k != ui->ub_pick - Gen3CartUbPickMove1 && moves[k] == id) elsewhere = true;
        }
        if(elsewhere) continue;
        gen3_cart_omega_pick_add(pokemon_fap, name, GEN3_CART_ITEM_UB_NAME + i);
        if(id == current) selected = GEN3_CART_ITEM_UB_NAME + i;
    }
    submenu_set_selected_item(pokemon_fap->submenu, selected);
    return true;
}

/* "HARDY", or e.g. "LONELY +Atk -Def" */
static void gen3_cart_omega_pick_nature(uint8_t nature, char out[GEN3_CART_SCREEN_ROW_SIZE]) {
    const char* up = NULL;
    const char* down = NULL;

    for(int s = Gen3StatAtk; s < Gen3StatCount; s++) {
        const int8_t effect = gen3_nature_effect(nature, (Gen3Stat)s);
        if(effect > 0) up = gen3_cart_omega_pick_stats[s];
        if(effect < 0) down = gen3_cart_omega_pick_stats[s];
    }
    if(up && down) {
        snprintf(
            out, GEN3_CART_SCREEN_ROW_SIZE, "%.7s +%.3s -%.3s", gen3_nature_name(nature), up, down);
    } else {
        snprintf(out, GEN3_CART_SCREEN_ROW_SIZE, "%.7s", gen3_nature_name(nature));
    }
}

/* The species' abilities, a slot each that it has; false if the species' can't be read */
static bool gen3_cart_omega_pick_abilities(PokemonFap* pokemon_fap) {
    const Gen3UbMon* m = &gen3_cart_ui_get(pokemon_fap)->ub_edit.mon;
    uint8_t a[3];
    char name[GEN3_CART_SCREEN_UB_NAME_SIZE];
    char label[GEN3_CART_SCREEN_ROW_SIZE];

    if(!gen3_ub_species_abilities(gen3_ub_mon_species(m), a)) return false;
    for(uint8_t slot = 0; slot < 3u; slot++) {
        /* as gen3_ub_edit_set_ability takes them */
        const bool has = slot == 0 || (slot == 1 ? a[1] != 0 && a[1] != a[0] : a[2] != 0);
        if(!has) continue;
        /* a name the data lacks shows as "#<id>", as on the editor's row */
        gen3_ub_ability_name(a[slot], name);
        snprintf(label, sizeof(label), "%.15s%s", name, slot == 2 ? " (H)" : "");
        gen3_cart_omega_pick_add(pokemon_fap, label, GEN3_CART_ITEM_UB_PICK + slot);
    }
    submenu_set_selected_item(
        pokemon_fap->submenu, GEN3_CART_ITEM_UB_PICK + gen3_ub_ability_slot(m));
    return true;
}

/* A list of its own (no letters); false if it can't be read */
static bool gen3_cart_omega_pick_list(PokemonFap* pokemon_fap) {
    const Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const Gen3UbMon* m = &ui->ub_edit.mon;
    char label[GEN3_CART_SCREEN_ROW_SIZE];
    uint8_t v[6];
    uint32_t selected = 0;

    submenu_set_header(pokemon_fap->submenu, gen3_cart_omega_pick_title(ui->ub_pick));
    switch(ui->ub_pick) {
    case Gen3CartUbPickNature:
        for(uint8_t n = 0; n < GEN3_NATURE_COUNT; n++) {
            gen3_cart_omega_pick_nature(n, label);
            gen3_cart_omega_pick_add(pokemon_fap, label, GEN3_CART_ITEM_UB_PICK + n);
        }
        selected = GEN3_CART_ITEM_UB_PICK + gen3_ub_nature(m);
        break;
    case Gen3CartUbPickAbility:
        return gen3_cart_omega_pick_abilities(pokemon_fap);
    case Gen3CartUbPickShiny:
        gen3_cart_omega_pick_add(pokemon_fap, "Yes", GEN3_CART_ITEM_UB_PICK + 1u);
        gen3_cart_omega_pick_add(pokemon_fap, "No", GEN3_CART_ITEM_UB_PICK);
        selected = GEN3_CART_ITEM_UB_PICK + (gen3_ub_shiny(m) ? 1u : 0u);
        break;
    case Gen3CartUbPickIvs:
        gen3_cart_omega_pick_add(pokemon_fap, "All 31", GEN3_CART_ITEM_UB_PICK + Gen3UbIvsAll31);
        gen3_cart_omega_pick_add(pokemon_fap, "All 0", GEN3_CART_ITEM_UB_PICK + Gen3UbIvsAll0);
        gen3_ub_mon_ivs(m, v);
        selected = GEN3_CART_ITEM_UB_PICK + (v[0] == 0 ? Gen3UbIvsAll0 : Gen3UbIvsAll31);
        break;
    case Gen3CartUbPickEvs:
        gen3_ub_mon_evs(m, v);
        for(uint8_t s = 0; s < 6u; s++) {
            gen3_cart_screen_ub_ev_label(s, v[s], label);
            gen3_cart_omega_pick_add(pokemon_fap, label, GEN3_CART_ITEM_UB_PICK + s);
        }
        selected = scene_manager_get_scene_state(
            pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaPick);
        break;
    default:
        break;
    }
    submenu_set_selected_item(pokemon_fap->submenu, selected);
    return true;
}

static void gen3_cart_omega_pick_show(PokemonFap* pokemon_fap) {
    const Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3UbSort s = Gen3UbSortSpecies;
    bool ok;

    gen3_cart_omega_pick_notice = false;
    submenu_reset(pokemon_fap->submenu);
    if(!gen3_cart_omega_pick_sort(ui->ub_pick, &s)) {
        ok = gen3_cart_omega_pick_list(pokemon_fap);
    } else if(ui->ub_page == GEN3_CART_UB_LETTERS) {
        ok = gen3_cart_omega_pick_letters(pokemon_fap, s);
    } else {
        ok = gen3_cart_omega_pick_names(pokemon_fap, s);
    }
    if(!ok) {
        view_dispatcher_send_custom_event(
            pokemon_fap->view_dispatcher, GEN3_CART_UB_EVENT_NO_DATA);
    }
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

void pokemon_scene_gen3_cart_omega_pick_on_enter(void* context) {
    gen3_cart_omega_pick_show(context);
}

/* Over the list: OK (or Back) returns to the editor, the Pokemon as it was */
static void
    gen3_cart_omega_pick_say(PokemonFap* pokemon_fap, const char* header, const char* text) {
    gen3_cart_omega_pick_notice = true;
    gen3_menu_show_notice(pokemon_fap, header, text);
}

static void gen3_cart_omega_pick_refused(PokemonFap* pokemon_fap) {
    gen3_cart_omega_pick_say(pokemon_fap, "Not changed", "No PID has that\nfor this OT ID.");
}

/* New Pokemon's species chosen: the Pokemon, then the editor on it */
static void gen3_cart_omega_pick_new(PokemonFap* pokemon_fap, uint16_t species) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3Omega* o;

    if(!gen3_cart_ui_picture_wait(pokemon_fap)) {
        gen3_cart_ui_busy_notice(pokemon_fap, PokemonSceneGen3CartOmegaPics);
        return;
    }
    o = gen3_cart_ui_omega(pokemon_fap);
    if(!o || !gen3_omega_owner(o, &ui->ub_mon)) {
        gen3_cart_ui_omega_notice(pokemon_fap, "New Pokemon", GEN3_CART_SCREEN_UB_NO_OWNER);
        return;
    }
    /* made in the editor's record, then loaded from a copy, as gen3_ub_edit_load takes it */
    if(!gen3_ub_new_box_mon(&ui->ub_edit.mon, species, GEN3_CART_UB_LEVEL, &ui->ub_mon)) {
        gen3_cart_ui_omega_mon_result(pokemon_fap, true, Gen3CartMonNoData);
        return;
    }
    ui->ub_mon = ui->ub_edit.mon;
    gen3_ub_edit_load(&ui->ub_edit, &ui->ub_mon, o->has_trainer ? &o->trainer : NULL);
    ui->ub_new = true;
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaEdit, 0);
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaEdit);
}

bool gen3_cart_omega_pick_id(PokemonFap* pokemon_fap, uint16_t id) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3UbEditor* e = &ui->ub_edit;
    bool ok = id != 0;

    if(ok && ui->ub_pick == Gen3CartUbPickNew) {
        gen3_cart_omega_pick_new(pokemon_fap, id);
        return true;
    }
    if(ok) {
        switch(ui->ub_pick) {
        case Gen3CartUbPickSpecies:
            ok = gen3_ub_edit_set_species(e, id);
            break;
        case Gen3CartUbPickItem:
            ok = gen3_ub_edit_set_item(e, id);
            break;
        default:
            ok = gen3_ub_edit_set_move(e, (uint8_t)(ui->ub_pick - Gen3CartUbPickMove1), id);
            break;
        }
    }
    gen3_cart_ui_trace(pokemon_fap, "pick %u %s", (unsigned)id, ok ? "ok" : "refused");
    if(ok) {
        scene_manager_search_and_switch_to_previous_scene(
            pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaEdit);
    }
    return ok;
}

/* A name chosen: list index i of s */
static void gen3_cart_omega_pick_name(PokemonFap* pokemon_fap, Gen3UbSort s, uint16_t i) {
    if(!gen3_cart_omega_pick_id(pokemon_fap, gen3_ub_sorted(s, i))) {
        gen3_cart_omega_pick_refused(pokemon_fap);
    }
}

/* A row of the letters or of a list of its own: value k */
static void gen3_cart_omega_pick_value(PokemonFap* pokemon_fap, uint32_t event) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    SceneManager* scene_manager = pokemon_fap->scene_manager;
    Gen3UbEditor* e = &ui->ub_edit;
    const uint32_t k = event - GEN3_CART_ITEM_UB_PICK;
    Gen3UbSort s;
    bool ok = true;

    scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartOmegaPick, event);
    if(gen3_cart_omega_pick_sort(ui->ub_pick, &s)) {
        if(ui->ub_page == GEN3_CART_UB_LETTERS && k < gen3_cart_omega_page_count &&
           (ui->ub_pick == Gen3CartUbPickSpecies || ui->ub_pick == Gen3CartUbPickNew)) {
            /* the species with pictures: the page's first index and count */
            scene_manager_set_scene_state(
                scene_manager,
                PokemonSceneGen3CartOmegaPics,
                ((uint32_t)gen3_cart_omega_pages[k].first << 16) | gen3_cart_omega_pages[k].count);
            scene_manager_next_scene(scene_manager, PokemonSceneGen3CartOmegaPics);
            return;
        }
        /* a page of names */
        ui->ub_page = (uint8_t)k;
        gen3_cart_omega_pick_show(pokemon_fap);
        return;
    }
    switch(ui->ub_pick) {
    case Gen3CartUbPickNature:
        ok = gen3_ub_edit_set_nature(e, (uint8_t)k);
        break;
    case Gen3CartUbPickAbility:
        ok = gen3_ub_edit_set_ability(e, (uint8_t)k);
        break;
    case Gen3CartUbPickShiny:
        ok = gen3_ub_edit_set_shiny(e, k != 0);
        break;
    case Gen3CartUbPickIvs:
        gen3_ub_edit_set_ivs(e, k == Gen3UbIvsAll0 ? Gen3UbIvsAll0 : Gen3UbIvsAll31);
        break;
    case Gen3CartUbPickEvs:
        /* the number scene's state 1 + the stat */
        scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartOmegaNumber, 1u + k);
        scene_manager_next_scene(scene_manager, PokemonSceneGen3CartOmegaNumber);
        return;
    default:
        break;
    }
    if(ok) {
        scene_manager_previous_scene(scene_manager);
    } else {
        gen3_cart_omega_pick_refused(pokemon_fap);
    }
}

/* None: the held item, or Move 2..4, emptied */
static void gen3_cart_omega_pick_none(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(ui->ub_pick == Gen3CartUbPickItem) {
        gen3_ub_edit_set_item(&ui->ub_edit, 0);
    } else if(ui->ub_pick > Gen3CartUbPickMove1 && ui->ub_pick <= Gen3CartUbPickMove4) {
        gen3_ub_edit_set_move(&ui->ub_edit, (uint8_t)(ui->ub_pick - Gen3CartUbPickMove1), 0);
    }
    scene_manager_previous_scene(pokemon_fap->scene_manager);
}

bool pokemon_scene_gen3_cart_omega_pick_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3UbSort s = Gen3UbSortSpecies;
    const bool lettered = gen3_cart_omega_pick_sort(ui->ub_pick, &s);
    const bool names = lettered && ui->ub_page != GEN3_CART_UB_LETTERS;

    if(event.type == SceneManagerEventTypeBack) {
        /* The names: the letters again. The letters, a list, or a notice: the editor (the list,
         * for New Pokemon) */
        if(names && !gen3_cart_omega_pick_notice) {
            ui->ub_page = GEN3_CART_UB_LETTERS;
            gen3_cart_omega_pick_show(pokemon_fap);
            return true;
        }
        return false;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(gen3_cart_omega_pick_notice) {
        /* a row's press still queued counts for nothing under the notice */
        if(event.event == GEN3_EVENT_NOTICE_OK) {
            scene_manager_previous_scene(pokemon_fap->scene_manager);
        }
        return true;
    }
    if(event.event == GEN3_CART_UB_EVENT_NO_DATA) {
        gen3_cart_omega_pick_say(
            pokemon_fap,
            GEN3_CART_SCREEN_CANT_EDIT,
            gen3_cart_screen_mon_text(Gen3CartMonNoData, NULL));
    } else if(event.event == GEN3_CART_ITEM_UB_NONE) {
        if(lettered && !names) gen3_cart_omega_pick_none(pokemon_fap);
    } else if(
        event.event >= GEN3_CART_ITEM_UB_NAME &&
        event.event < GEN3_CART_ITEM_UB_NAME + GEN3_CART_ITEM_UB_NAMES) {
        if(names)
            gen3_cart_omega_pick_name(
                pokemon_fap, s, (uint16_t)(event.event - GEN3_CART_ITEM_UB_NAME));
    } else if(event.event >= GEN3_CART_ITEM_UB_PICK && event.event < GEN3_CART_ITEM_UB_NONE) {
        if(!names) gen3_cart_omega_pick_value(pokemon_fap, event.event);
    } else {
        return false;
    }
    return true;
}

void pokemon_scene_gen3_cart_omega_pick_on_exit(void* context) {
    UNUSED(context);
}
