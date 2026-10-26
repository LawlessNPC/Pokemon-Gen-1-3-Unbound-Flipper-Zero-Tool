#include <furi.h>
#include <gui/modules/submenu.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_letters.h>

/* Gen III held item. Gen3ItemLetter: the scene state is the chosen letter. */

#define GEN3_ITEM_NONE 0 /* the "No Item" row; letters are never 0 */

static const Gen3NameList gen3_item_list = {
    GEN3_HOLDABLE_COUNT,
    gen3_holdable_by_name,
    gen3_item_name,
};

static void gen3_item_done(PokemonFap* pokemon_fap) {
    view_dispatcher_send_custom_event(
        pokemon_fap->view_dispatcher, PokemonSceneGen3Edit | PokemonSceneSearch);
}

static bool gen3_item_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event & PokemonSceneSearch)
        scene_manager_search_and_switch_to_previous_scene(
            pokemon_fap->scene_manager, event.event & ~(uint32_t)PokemonSceneSearch);
    else
        scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
    return true;
}

/* Gen3ItemLetter */

static void gen3_item_letter_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    if(index == GEN3_ITEM_NONE) {
        gen3_edit_set_item(pokemon_fap->gen3_editor, 0);
        gen3_item_done(pokemon_fap);
        return;
    }

    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3ItemLetter, index);
    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, PokemonSceneGen3ItemSet);
}

void pokemon_scene_gen3_item_letter_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    char letters[GEN3_LETTERS_MAX];
    char label[2] = {0, 0};
    size_t n = gen3_letters(&gen3_item_list, letters, sizeof(letters));

    submenu_reset(pokemon_fap->submenu);
    submenu_add_item(
        pokemon_fap->submenu, "No Item", GEN3_ITEM_NONE, gen3_item_letter_callback, pokemon_fap);
    for(size_t i = 0; i < n; i++) {
        label[0] = letters[i];
        submenu_add_item(
            pokemon_fap->submenu,
            label,
            (uint32_t)(uint8_t)letters[i],
            gen3_item_letter_callback,
            pokemon_fap);
    }

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_item_letter_on_event(void* context, SceneManagerEvent event) {
    return gen3_item_on_event(context, event);
}

void pokemon_scene_gen3_item_letter_on_exit(void* context) {
    UNUSED(context);
}

/* Gen3ItemSet */

static void gen3_item_set_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    gen3_edit_set_item(pokemon_fap->gen3_editor, (uint16_t)index);
    gen3_item_done(pokemon_fap);
}

void pokemon_scene_gen3_item_set_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    char letter = (char)scene_manager_get_scene_state(
        pokemon_fap->scene_manager, PokemonSceneGen3ItemLetter);
    static uint16_t ids[GEN3_LETTER_IDS_MAX];
    size_t n = gen3_letter_ids(&gen3_item_list, letter, ids, GEN3_LETTER_IDS_MAX);

    submenu_reset(pokemon_fap->submenu);
    for(size_t i = 0; i < n; i++)
        submenu_add_item(
            pokemon_fap->submenu,
            gen3_item_name(ids[i]),
            ids[i],
            gen3_item_set_callback,
            pokemon_fap);
    submenu_set_selected_item(
        pokemon_fap->submenu, gen3_pkm_get(&pokemon_fap->gen3_editor->pkm, Gen3FieldHeldItem));

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_item_set_on_event(void* context, SceneManagerEvent event) {
    return gen3_item_on_event(context, event);
}

void pokemon_scene_gen3_item_set_on_exit(void* context) {
    UNUSED(context);
}
