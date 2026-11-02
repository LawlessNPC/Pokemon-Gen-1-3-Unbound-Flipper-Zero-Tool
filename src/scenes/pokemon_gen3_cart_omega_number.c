#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <furi.h>
#include <gui/modules/text_input.h>

/* The Unbound editor's numbers: the level (1-100) and each EV (0-252). The scene state says which:
 * 0 the level, 1 + a stat (HP, Atk, Def, Spe, SpA, SpD) an EV, set before entering. A number
 * accepted goes into the editor's Pokemon (gen3_ub_edit_set_level, gen3_ub_edit_set_evs, which
 * keeps the total at most 510), and the scene returns to the one below: the editor, or the EV
 * list. */

#define GEN3_CART_OMEGA_NUMBER_DONE 0x473305B0u
#define GEN3_CART_OMEGA_LEVEL_MAX 100u
#define GEN3_CART_OMEGA_EV_MAX 252u

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
    const bool level = which == 0;
    const uint32_t min = level ? 1u : 0u;
    const uint32_t max = level ? GEN3_CART_OMEGA_LEVEL_MAX : GEN3_CART_OMEGA_EV_MAX;
    uint32_t number = 0;
    bool ok = text[0] != '\0';

    /* Digits only; the buffer holds 3, so this can't overflow */
    for(size_t i = 0; ok && text[i] != '\0'; i++) {
        if(text[i] < '0' || text[i] > '9') {
            ok = false;
        } else {
            number = number * 10u + (uint32_t)(text[i] - '0');
        }
    }
    if(!ok || number < min || number > max) {
        furi_string_set(error, level ? "Level must\nbe 1-100!" : "EV must\nbe 0-252!");
        return false;
    }
    if(level) {
        gen3_ub_edit_set_level(e, (uint8_t)number);
    } else if(which <= 6u) {
        uint8_t evs[6];
        gen3_ub_mon_evs(&e->mon, evs);
        evs[which - 1u] = (uint8_t)number;
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
