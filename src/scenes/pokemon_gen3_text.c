#include <furi.h>
#include <gui/modules/text_input.h>
#include <gui/view_dispatcher.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_text.h>

/* Nickname and OT name. The scene it is entered as is the scene state of Gen3Nickname. */

static char gen3_nickname_buf[GEN3_NICKNAME_LEN + 1];
static char gen3_ot_name_buf[GEN3_OT_NAME_LEN + 1];

static bool gen3_text_validator(const char* text, FuriString* error, void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    bool nickname =
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Nickname) ==
        PokemonSceneGen3Nickname;

    /* An empty nickname is the species name; an OT needs a name */
    if(!nickname && text[0] == '\0') {
        furi_string_set(error, "Name can't\nbe empty!");
        return false;
    }

    if(!gen3_text_encodable(text, nickname ? GEN3_NICKNAME_LEN : GEN3_OT_NAME_LEN)) {
        furi_string_set(error, "Unsupported\ncharacter!");
        return false;
    }

    if(nickname)
        gen3_edit_set_nickname(pokemon_fap->gen3_editor, text);
    else
        gen3_edit_set_ot_name(pokemon_fap->gen3_editor, text);

    return true;
}

static void gen3_text_result_callback(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, PokemonSceneBack);
}

void pokemon_scene_gen3_text_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    const Gen3Pkm* p = &pokemon_fap->gen3_editor->pkm;
    bool nickname =
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Nickname) ==
        PokemonSceneGen3Nickname;
    char* buf;
    size_t buf_size;

    if(nickname) {
        buf = gen3_nickname_buf;
        buf_size = sizeof(gen3_nickname_buf);
        gen3_pkm_nickname(p, buf, buf_size);
    } else {
        buf = gen3_ot_name_buf;
        buf_size = sizeof(gen3_ot_name_buf);
        gen3_pkm_ot_name(p, buf, buf_size);
    }

    text_input_reset(pokemon_fap->text_input);
    text_input_set_validator(pokemon_fap->text_input, gen3_text_validator, pokemon_fap);
    text_input_set_result_callback(
        pokemon_fap->text_input, gen3_text_result_callback, pokemon_fap, buf, buf_size, true);
    /* Saving an empty nickname must still reach the result callback */
    if(nickname) text_input_set_minimum_length(pokemon_fap->text_input, 0);
    text_input_set_header_text(
        pokemon_fap->text_input, nickname ? "Nickname (none for default)" : "Enter OT Name");

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewTextInput);
}

bool pokemon_scene_gen3_text_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom && event.event == (uint32_t)PokemonSceneBack) {
        scene_manager_previous_scene(pokemon_fap->scene_manager);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_text_on_exit(void* context) {
    UNUSED(context);
}
