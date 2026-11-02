#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <furi.h>
#include <gui/modules/text_input.h>

/* The Unbound editor's numbers: the level (1-100) and each EV (0-252). The scene state says which:
 * 0 the level, 1 + a stat (HP, Atk, Def, Spe, SpA, SpD) an EV, set before entering. The keyboard
 * refuses a number gen3_cart_screen_ub_number doesn't take, an EV that would take the six over 510
 * among them, so that no other EV is lowered behind the user's back. A number accepted goes into
 * the editor's Pokemon (gen3_ub_edit_set_level, gen3_ub_edit_set_evs), and the scene returns to
 * the one below: the editor, or the EV list. */

#define GEN3_CART_OMEGA_NUMBER_DONE 0x473305B0u

static char gen3_cart_omega_number_buf[4]; /* "252" */
static char gen3_cart_omega_number_header[24];

static uint32_t gen3_cart_omega_number_which(PokemonFap* pokemon_fap) {
    return scene_manager_get_scene_state(
        pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaNumber);
}

static bool gen3_cart_omega_number_validator(const char* text, FuriString* error, void* context) {
    PokemonFap* pokemon_fap = context;
    Gen3UbEditor* e = &gen3_cart_ui_get(pokemon_fap)->ub_edit;
    const uint32_t which = gen3_cart_omega_number_which(pokemon_fap);
    uint8_t number = 0;
    const char* why = gen3_cart_screen_ub_number(text, which, &e->mon, &number);

    if(why) {
        furi_string_set(error, why);
        return false;
    }
    if(which == 0) {
        gen3_ub_edit_set_level(e, number);
    } else if(which <= 6u) {
        uint8_t evs[6];
        gen3_ub_mon_evs(&e->mon, evs);
        evs[which - 1u] = number;
        gen3_ub_edit_set_evs(e, evs);
    }
    return true;
}

static void gen3_cart_omega_number_result(void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_CART_OMEGA_NUMBER_DONE);
}

void pokemon_scene_gen3_cart_omega_number_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    const Gen3UbMon* m = &gen3_cart_ui_get(pokemon_fap)->ub_edit.mon;
    const uint32_t which = gen3_cart_omega_number_which(pokemon_fap);
    TextInput* text_input = pokemon_fap->text_input;

    if(which == 0) {
        snprintf(
            gen3_cart_omega_number_header,
            sizeof(gen3_cart_omega_number_header),
            "Enter level (1-100)");
        snprintf(
            gen3_cart_omega_number_buf,
            sizeof(gen3_cart_omega_number_buf),
            "%u",
            (unsigned)gen3_ub_mon_level(m));
    } else {
        static const char stats[6][4] = {"HP", "Atk", "Def", "Spe", "SpA", "SpD"};
        uint8_t evs[6];
        const uint8_t s = (uint8_t)(which <= 6u ? which - 1u : 0u);
        gen3_ub_mon_evs(m, evs);
        snprintf(
            gen3_cart_omega_number_header,
            sizeof(gen3_cart_omega_number_header),
            "Enter %.3s EV (0-252)",
            stats[s]);
        snprintf(
            gen3_cart_omega_number_buf, sizeof(gen3_cart_omega_number_buf), "%u", (unsigned)evs[s]);
    }
    text_input_reset(text_input);
    text_input_set_validator(text_input, gen3_cart_omega_number_validator, pokemon_fap);
    text_input_set_result_callback(
        text_input,
        gen3_cart_omega_number_result,
        pokemon_fap,
        gen3_cart_omega_number_buf,
        sizeof(gen3_cart_omega_number_buf),
        true);
    text_input_set_header_text(text_input, gen3_cart_omega_number_header);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewTextInput);
}

bool pokemon_scene_gen3_cart_omega_number_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    /* Back on the keyboard is the scene manager's: nothing changes */
    if(event.type == SceneManagerEventTypeCustom && event.event == GEN3_CART_OMEGA_NUMBER_DONE) {
        scene_manager_previous_scene(pokemon_fap->scene_manager);
        return true;
    }
    return false;
}

void pokemon_scene_gen3_cart_omega_number_on_exit(void* context) {
    UNUSED(context);
}
