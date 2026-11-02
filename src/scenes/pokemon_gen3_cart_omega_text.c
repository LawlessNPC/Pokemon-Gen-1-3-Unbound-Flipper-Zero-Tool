#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <gui/modules/text_input.h>
#include <src/gen3/gen3_text.h>

/* The Unbound editor's nickname: 1 to 10 characters the game can store (gen3_text_encodable),
 * into the editor's Pokemon (gen3_ub_edit_set_nickname). The name as shown, accepted unchanged,
 * changes nothing, so characters the app shows as '?' are kept. Then back to the editor. */

#define GEN3_CART_OMEGA_TEXT_DONE 0x473305B1u

static char gen3_cart_omega_text_buf[GEN3_UB_MON_NICK + 1u];
static char gen3_cart_omega_text_shown[GEN3_UB_MON_NICK + 1u];

static bool gen3_cart_omega_text_validator(const char* text, FuriString* error, void* context) {
    PokemonFap* pokemon_fap = context;

    if(strcmp(text, gen3_cart_omega_text_shown) == 0) return true;
    if(text[0] == '\0') {
        furi_string_set(error, "Name can't\nbe empty!");
        return false;
    }
    if(!gen3_text_encodable(text, GEN3_UB_MON_NICK)) {
        furi_string_set(error, "Unsupported\ncharacter!");
        return false;
    }
    gen3_ub_edit_set_nickname(&gen3_cart_ui_get(pokemon_fap)->ub_edit, text);
    return true;
}

static void gen3_cart_omega_text_result(void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_CART_OMEGA_TEXT_DONE);
}

void pokemon_scene_gen3_cart_omega_text_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    const Gen3UbMon* m = &gen3_cart_ui_get(pokemon_fap)->ub_edit.mon;
    TextInput* text_input = pokemon_fap->text_input;
    uint8_t raw[GEN3_UB_MON_NICK];

    gen3_ub_mon_nickname(m, raw);
    gen3_text_decode(
        raw, sizeof(raw), gen3_cart_omega_text_shown, sizeof(gen3_cart_omega_text_shown));
    memcpy(gen3_cart_omega_text_buf, gen3_cart_omega_text_shown, sizeof(gen3_cart_omega_text_buf));
    text_input_reset(text_input);
    text_input_set_validator(text_input, gen3_cart_omega_text_validator, pokemon_fap);
    text_input_set_result_callback(
        text_input,
        gen3_cart_omega_text_result,
        pokemon_fap,
        gen3_cart_omega_text_buf,
        sizeof(gen3_cart_omega_text_buf),
        true);
    text_input_set_minimum_length(text_input, 1);
    text_input_set_header_text(text_input, "Nickname (1-10)");
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewTextInput);
}

bool pokemon_scene_gen3_cart_omega_text_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    /* Back on the keyboard is the scene manager's: nothing changes */
    if(event.type == SceneManagerEventTypeCustom && event.event == GEN3_CART_OMEGA_TEXT_DONE) {
        scene_manager_previous_scene(pokemon_fap->scene_manager);
        return true;
    }
    return false;
}

void pokemon_scene_gen3_cart_omega_text_on_exit(void* context) {
    UNUSED(context);
}
