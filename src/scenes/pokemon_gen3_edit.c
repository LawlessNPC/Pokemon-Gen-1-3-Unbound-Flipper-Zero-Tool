#include <gui/modules/submenu.h>

#include <src/include/pokemon_app.h>
#include <src/include/named_list.h>
#include <src/include/stat_nl.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>

/* Gen III editor menu. Each row's item index is the scene it opens. The spaces after each label
 * line the values up at the same pixel column in the submenu font, as in pokemon_gen.c. */

/* name_buf holds the species name too */
_Static_assert(GEN3_NICKNAME_LEN >= GEN3_SPECIES_NAME_LEN, "name_buf too small");

static void gen3_edit_item_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    /* Highlight the same row when we come back to this menu */
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Edit, index);

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

static void gen3_edit_add(PokemonFap* pokemon_fap, const char* label, PokemonScene scene) {
    submenu_add_item(pokemon_fap->submenu, label, scene, gen3_edit_item_callback, pokemon_fap);
}

static const char* gen3_edit_gender_text(Gen3Gender gender) {
    switch(gender) {
    case Gen3GenderMale:
        return "Male";
    case Gen3GenderFemale:
        return "Female";
    default:
        return "Unknown";
    }
}

void pokemon_scene_gen3_edit_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    const Gen3Editor* ed = pokemon_fap->gen3_editor;
    const Gen3Pkm* p = &ed->pkm;
    uint16_t national = gen3_edit_national(ed);
    const Gen3SpeciesInfo* info = gen3_species(national);
    uint32_t ot_id = gen3_pkm_get(p, Gen3FieldOtId);
    uint16_t item;
    Gen3Traits traits;
    char buf[32];
    char name_buf[GEN3_NICKNAME_LEN + 1];

    gen3_traits_of(p, &traits);

    submenu_reset(pokemon_fap->submenu);

    gen3_species_name_get(national, name_buf);
    snprintf(buf, sizeof(buf), "Pokemon:     %s", name_buf);
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Species);

    gen3_pkm_nickname(p, name_buf, sizeof(name_buf));
    snprintf(buf, sizeof(buf), "Nickname:  %s", name_buf);
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Nickname);

    snprintf(
        buf,
        sizeof(buf),
        "Level:           %u",
        (unsigned)(uint8_t)gen3_pkm_get(p, Gen3FieldLevel));
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Level);

    /* A loaded Pokemon keeps its own IVs and EVs until a preset is chosen */
    gen3_edit_add(
        pokemon_fap,
        ed->preset_applied ? namedlist_name_get_index(stat_list, ed->preset) :
                             "IVs/EVs: as loaded",
        PokemonSceneGen3Preset);

    snprintf(buf, sizeof(buf), "Nature:        %s", gen3_nature_name(traits.nature));
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Nature);

    snprintf(
        buf,
        sizeof(buf),
        "Ability:          %s",
        gen3_ability_name(info ? info->ability[traits.ability_slot & 1] : 0));
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Ability);

    snprintf(buf, sizeof(buf), "Shiny:           %s", traits.shiny ? "Yes" : "No");
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Shiny);

    snprintf(buf, sizeof(buf), "Gender:        %s", gen3_edit_gender_text(traits.gender));
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Gender);

    if(national == GEN3_NATIONAL_UNOWN) {
        snprintf(buf, sizeof(buf), "Unown Form: %c", gen3_unown_char(traits.unown_letter));
        gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Unown);
    }

    snprintf(
        buf,
        sizeof(buf),
        "Ball:               %s",
        gen3_item_name((uint16_t)gen3_pkm_get(p, Gen3FieldBall)));
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Ball);

    item = (uint16_t)gen3_pkm_get(p, Gen3FieldHeldItem);
    snprintf(buf, sizeof(buf), "Held Item:   %s", item ? gen3_item_name(item) : "No Item");
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3ItemLetter);

    gen3_edit_add(pokemon_fap, "Select Moves", PokemonSceneGen3Moves);

    snprintf(
        buf,
        sizeof(buf),
        "Pokerus:       %s",
        gen3_pokerus_status((uint8_t)gen3_pkm_get(p, Gen3FieldPokerus)));
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Pokerus);

    gen3_pkm_ot_name(p, name_buf, sizeof(name_buf));
    snprintf(buf, sizeof(buf), "OT Name:    %s", name_buf);
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3OtName);

    snprintf(buf, sizeof(buf), "OT TID:         %05u", (unsigned)(uint16_t)ot_id);
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Tid);

    snprintf(buf, sizeof(buf), "OT SID:         %05u", (unsigned)(uint16_t)(ot_id >> 16));
    gen3_edit_add(pokemon_fap, buf, PokemonSceneGen3Sid);

    submenu_set_selected_item(
        pokemon_fap->submenu,
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Edit));

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_edit_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    uint16_t national = gen3_edit_national(pokemon_fap->gen3_editor);
    uint32_t alias;

    /* Back is left to the scene manager, which returns to the Gen III menu */
    if(event.type != SceneManagerEventTypeCustom) return false;

    /* One handler serves several scenes; the scene it is entered as is kept in the scene
     * state of the first one */
    switch(event.event) {
    case PokemonSceneGen3Nickname:
    case PokemonSceneGen3OtName:
        alias = PokemonSceneGen3Nickname;
        break;
    case PokemonSceneGen3Level:
    case PokemonSceneGen3Tid:
    case PokemonSceneGen3Sid:
        alias = PokemonSceneGen3Level;
        break;
    case PokemonSceneGen3Ability:
        /* Nothing to choose for a one-ability species */
        if(!gen3_species_two_abilities(national)) return true;
        alias = PokemonSceneGen3Preset;
        break;
    case PokemonSceneGen3Gender:
        /* Nor for a male-only, female-only or genderless one */
        if(gen3_species_gender_fixed(national)) return true;
        alias = PokemonSceneGen3Preset;
        break;
    case PokemonSceneGen3Species:
    case PokemonSceneGen3Moves:
    case PokemonSceneGen3ItemLetter:
    case PokemonSceneGen3Pokerus:
        /* Their own scenes, so there is no alias state to set */
        scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
        return true;
    case PokemonSceneGen3Preset:
    case PokemonSceneGen3Nature:
    case PokemonSceneGen3Shiny:
    case PokemonSceneGen3Ball:
    case PokemonSceneGen3Unown:
        alias = PokemonSceneGen3Preset;
        break;
    default:
        /* Not one of this menu's rows */
        return false;
    }

    scene_manager_set_scene_state(pokemon_fap->scene_manager, alias, event.event);
    scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
    return true;
}

void pokemon_scene_gen3_edit_on_exit(void* context) {
    UNUSED(context);
}
