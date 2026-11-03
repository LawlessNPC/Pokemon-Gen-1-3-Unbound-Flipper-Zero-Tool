#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <furi.h>
#include <src/gen3/gen3_pk3_furi.h>
#include <src/gen3/gen3_ub_data.h>
#include <src/scenes/include/pokemon_gen3_menu.h>
#include <src/views/gen3_species_select.h>
#include <src/views/gen3_ub_pic_select.h>

/* The Unbound species picker for the Pokemon row and New Pokemon (Gen3CartOmegaPick's letters
 * open it, the scene state being the page: its first list index in the high half, its count in
 * the low): one species of the page at a time with its picture or "?", the name and n/m; Left and
 * Right step (round the page), OK picks (gen3_cart_omega_pick_id), Back returns to the letters.
 *
 * A picture is, in turn: the shipped one (all_sprites.fxbm, gen3_ub_shipped_picture), the cached
 * one (unbound_pics/<species>.fxb, exactly one 404-byte record), else "?" and a background fetch
 * from the Omega's ROM (gen3_cart_ui_picture_want) while it can be had. On the app's 100 ms tick
 * (a SceneManagerEventTypeTick, on this thread) it takes the finished job
 * (gen3_cart_ui_picture_ended), reloads the picture if it was the shown species', and starts the
 * waiting one. A species whose fetch failed is not asked for again until it has been stepped away
 * from. The request queue is only touched here, and dropped on leaving. */

#define GEN3_CART_PIC_FILE_SIZE 404u

static void* gen3_cart_pics_view;
static uint16_t gen3_cart_pics_first, gen3_cart_pics_count, gen3_cart_pics_at;
static uint16_t gen3_cart_pics_failed; /* the species whose fetch just failed, 0 none */
static bool gen3_cart_pics_notice; /* gen3_menu_show_notice is up */

static void gen3_cart_pics_callback(void* context, uint32_t what) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, what);
}

static uint16_t gen3_cart_pics_species(void) {
    return gen3_ub_sorted(Gen3UbSortSpecies, (uint16_t)(gen3_cart_pics_first + gen3_cart_pics_at));
}

/* Shows the species at `at`; with ask, a missing picture of a fetched species is asked for */
static void gen3_cart_pics_show(PokemonFap* pokemon_fap, bool ask) {
    char name[GEN3_UB_NAME_LEN + 1u];
    char path[64];
    const uint16_t id = gen3_cart_pics_species();
    const uint16_t shipped = gen3_ub_shipped_picture(id);
    const char* file = NULL;
    uint32_t index = 0;
    uint32_t size = 0;

    gen3_ub_species_name(id, name);
    if(shipped) {
        file = APP_ASSETS_PATH("all_sprites.fxbm");
        index = shipped - 1u;
    } else if(id) {
        snprintf(path, sizeof(path), "%s/%s/%u.fxb", GEN3_SHARED_DATA_DIR, GEN3_OMEGA_PICS, id);
        file = path;
        size = GEN3_CART_PIC_FILE_SIZE;
    }
    const bool has = gen3_ub_pic_select_set(
        gen3_cart_pics_view,
        name,
        (uint16_t)(gen3_cart_pics_at + 1u),
        gen3_cart_pics_count,
        file,
        index,
        size);
    if(ask && !has && !shipped && id && id != gen3_cart_pics_failed) {
        gen3_cart_ui_picture_want(pokemon_fap, id);
    }
}

void pokemon_scene_gen3_cart_omega_pics_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    const Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const uint32_t page = scene_manager_get_scene_state(
        pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaPics);

    gen3_cart_pics_first = (uint16_t)(page >> 16);
    gen3_cart_pics_count = (uint16_t)page;
    gen3_cart_pics_failed = 0;
    gen3_cart_pics_notice = false;
    gen3_cart_pics_at = gen3_cart_screen_pic_page(
        gen3_cart_pics_first,
        gen3_cart_pics_count,
        ui->ub_pick == Gen3CartUbPickSpecies ? gen3_ub_mon_species(&ui->ub_edit.mon) : 0);
    gen3_cart_pics_view =
        gen3_ub_pic_select_alloc(pokemon_fap->view_dispatcher, AppViewGen3UbPic);
    gen3_ub_pic_select_set_callback(gen3_cart_pics_view, gen3_cart_pics_callback, pokemon_fap);
    gen3_cart_pics_show(pokemon_fap, true);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewGen3UbPic);
}

/* OK: the species at `at`. A request not yet started is dropped; New Pokemon waits for one
 * running, as it needs the Omega. */
static void gen3_cart_pics_pick(PokemonFap* pokemon_fap) {
    gen3_cart_pic_drop(&gen3_cart_ui_get(pokemon_fap)->pics);
    if(!gen3_cart_omega_pick_id(pokemon_fap, gen3_cart_pics_species())) {
        gen3_cart_pics_notice = true;
        gen3_menu_show_notice(pokemon_fap, "Not changed", "No PID has that\nfor this OT ID.");
    }
}

/* The tick: a finished job's picture, if it is the shown species', then the waiting request */
static void gen3_cart_pics_tick(PokemonFap* pokemon_fap) {
    uint16_t species;
    Gen3CartResult r;

    if(!gen3_cart_pics_view || gen3_cart_pics_notice) return;
    if(gen3_cart_ui_picture_ended(pokemon_fap, &species, &r) &&
       species == gen3_cart_pics_species()) {
        gen3_cart_pics_failed = species;
        gen3_cart_pics_show(pokemon_fap, false);
    }
    gen3_cart_ui_picture_pump(pokemon_fap);
}

bool pokemon_scene_gen3_cart_omega_pics_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    if(event.type == SceneManagerEventTypeTick) {
        gen3_cart_pics_tick(pokemon_fap);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(gen3_cart_pics_notice) {
        if(event.event == GEN3_EVENT_NOTICE_OK) {
            scene_manager_search_and_switch_to_previous_scene(
                pokemon_fap->scene_manager, PokemonSceneGen3CartOmegaEdit);
        }
        return true;
    }
    switch(event.event) {
    case GEN3_UB_PIC_LEFT:
    case GEN3_UB_PIC_RIGHT:
        if(!gen3_cart_pics_count) return true;
        gen3_cart_pics_at = event.event == GEN3_UB_PIC_LEFT ?
                                (gen3_cart_pics_at ? gen3_cart_pics_at : gen3_cart_pics_count) - 1u :
                                (gen3_cart_pics_at + 1u) % gen3_cart_pics_count;
        gen3_cart_pics_failed = 0;
        gen3_cart_pics_show(pokemon_fap, true);
        break;
    case GEN3_UB_PIC_OK:
        gen3_cart_pics_pick(pokemon_fap);
        break;
    default:
        return false;
    }
    return true;
}

/* The picture view lives for the visit only (memory is tight), so it goes here. But on_exit runs
 * before the next scene's on_enter, while it is still the shown view, and removing the shown view
 * stops the view dispatcher: the app would close to the Flipper's menu on every OK and Back. So
 * the submenu, which the app keeps for its whole life, is shown first; every scene this one
 * leaves to shows its own view in on_enter right after. */
void pokemon_scene_gen3_cart_omega_pics_on_exit(void* context) {
    PokemonFap* pokemon_fap = context;

    gen3_cart_pic_drop(&gen3_cart_ui_get(pokemon_fap)->pics);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
    gen3_ub_pic_select_free(pokemon_fap->view_dispatcher, AppViewGen3UbPic, gen3_cart_pics_view);
    gen3_cart_pics_view = NULL;
}
