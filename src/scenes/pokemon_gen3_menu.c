#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_session_furi.h>
#include <src/views/gen3_link_test.h>

#include <furi_hal_random.h>
#include <stdlib.h>
#include <src/gen3/gen3_bulk_furi.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/views/gen3_species_select.h>

/* The "Data missing" dialog is up instead of the menu */
static bool gen3_menu_dialog_shown = false;

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

/* Everything but the Link Test needs the species names and learnsets from gen3_bulk.bin. The
 * dialog has no buttons; Back returns to this menu. */
static void gen3_menu_show_data_missing(PokemonFap* pokemon_fap) {
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    dialog_ex_reset(dialog_ex);
    dialog_ex_set_header(dialog_ex, "Data missing", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog_ex,
        "Gen III data file is missing.\nReinstall the app.",
        64,
        36,
        AlignCenter,
        AlignCenter);

    gen3_menu_dialog_shown = true;
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

static void gen3_menu_show_menu(PokemonFap* pokemon_fap) {
    gen3_menu_dialog_shown = false;
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

void pokemon_scene_gen3_menu_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    /* The bulk data file is opened once, then kept open until the app exits (free_app closes
     * it). The Pokemon being edited lives until the app exits too. Its nickname and moves come
     * from the bulk file, so without the file there is no editor. */
    bool have_data = gen3_bulk_ready() || gen3_bulk_furi_open();
    if(!pokemon_fap->gen3_editor && have_data) {
        pokemon_fap->gen3_editor = malloc(sizeof(Gen3Editor));
        gen3_edit_init(pokemon_fap->gen3_editor, furi_hal_random_get());
    }
    if(!pokemon_fap->gen3_species_select) {
        pokemon_fap->gen3_species_select =
            gen3_species_select_alloc(pokemon_fap->view_dispatcher, AppViewGen3SpeciesSelect);
    }

    submenu_reset(pokemon_fap->submenu);
    submenu_set_header(pokemon_fap->submenu, "Gen III");

    submenu_add_item(
        pokemon_fap->submenu,
        "Edit Pokemon",
        PokemonSceneGen3Edit,
        gen3_menu_item_callback,
        pokemon_fap);

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
    gen3_menu_show_menu(pokemon_fap);
    gen3_menu_end_link_test(pokemon_fap);
}

bool pokemon_scene_gen3_menu_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    /* Only menu items. A Link Test refresh tick still queued when the Link Test closed may
     * arrive here too; ignore it. */
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == PokemonSceneGen3LinkTest) {
            scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
            consumed = true;
        } else if(event.event == PokemonSceneGen3Edit) {
            /* There is an editor only once the bulk file is open, and it stays open */
            if(pokemon_fap->gen3_editor) {
                scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
            } else {
                gen3_menu_show_data_missing(pokemon_fap);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack && gen3_menu_dialog_shown) {
        /* Back on the dialog returns to this menu, not to the main menu */
        gen3_menu_show_menu(pokemon_fap);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_menu_on_exit(void* context) {
    UNUSED(context);
}
