#ifndef POKEMON_APP_H
#define POKEMON_APP_H

#pragma once

#include <gui/scene_manager.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/icon.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gblink.h>

#include <src/include/pokemon_data.h>
#include <src/gen3/gen3_pk3.h>

#define TAG "Pokemon"

struct pokemon_fap {
    /* Various anonymous pointers for flipper UI/navigation */
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    void* select;
    void* trade;
    Submenu* submenu;
    TextInput* text_input;
    VariableItemList* variable_item_list;
    DialogEx* dialog_ex;

    /* Struct for holding trade data */
    /* NOTE: There may be some runtime memory savings by adding more intelligence
     * to views/trade and slimming down this struct to only contain the single
     * pokemon data rather than the full 6 member party data.
     */
    PokemonData* pdata;

    /* gblink interface */
    void *gblink_handle;

    /* Gen III Link Test: its view, and its session (struct Gen3Session from
     * src/gen3/gen3_session_furi.h). Both live from entering the Link Test until
     * the Gen III menu is shown again, so they survive the "Stop link?" dialog.
     */
    void* gen3_link_test;
    struct Gen3Session* gen3_session;

    /* Gen III editor (struct Gen3Editor from src/gen3/gen3_edit.h). Allocated the first
     * time the Gen III menu is shown, then kept for the app's lifetime.
     */
    struct Gen3Editor* gen3_editor;

    /* Gen III species picker view, allocated next to the editor */
    void* gen3_species_select;

    /* Gen III .pk3 file name being saved, without the extension */
    char gen3_file_name[GEN3_PK3_NAME_MAX + 1];
};

typedef struct pokemon_fap PokemonFap;

typedef enum {
    AppViewSubmenu,
    AppViewTextInput,
    AppViewVariableItem,
    AppViewDialogEx,
    AppViewSelectPokemon,
    AppViewTrade,
    AppViewGen3LinkTest,
    AppViewGen3SpeciesSelect,
} AppView;

#endif /* POKEMON_APP_H */
