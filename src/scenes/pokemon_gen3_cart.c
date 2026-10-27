#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <src/scenes/include/pokemon_gen3_menu.h>
#include <src/views/gen3_cart_progress.h>

/* The Cartridge app's hub, reached once a Connect ran to its end, and the helpers every Cartridge
 * scene shares. The hub is registered as Gen3Menu (pokemon_cart_scene_config.h): the Gen III
 * editor scenes search back to Gen3Menu, and in this app that is this hub. */

/* The hub's items, clear of scene ids and the Gen III events. The scene state keeps the one last
 * chosen. */
#define GEN3_CART_ITEM_INFO 0x47330511u
#define GEN3_CART_ITEM_RECONNECT 0x47330512u
#define GEN3_CART_ITEM_BACKUP 0x47330513u

/* gen3_cart_ui_status's buffer */
static Gen3CartSessionStatus gen3_cart_status;

/* Timer thread: hand the tick to the app thread, which reads the session */
static void gen3_cart_tick_callback(void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_CART_EVENT_TICK);
}

static void gen3_cart_back_callback(void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_CART_EVENT_BACK);
}

Gen3CartUi* gen3_cart_ui_get(PokemonFap* pokemon_fap) {
    if(!pokemon_fap->gen3_cart_ui) {
        Gen3CartUi* ui = malloc(sizeof(Gen3CartUi));
        memset(ui, 0, sizeof(*ui));
        ui->result_back = PokemonSceneGen3Cart;

        pokemon_fap->gen3_cart_progress =
            gen3_cart_progress_alloc(pokemon_fap->view_dispatcher, AppViewGen3CartProgress);
        gen3_cart_progress_set_tick(
            pokemon_fap->gen3_cart_progress, gen3_cart_tick_callback, pokemon_fap);
        gen3_cart_progress_set_back(
            pokemon_fap->gen3_cart_progress, gen3_cart_back_callback, pokemon_fap);

        ui->session = gen3_cart_session_alloc(pokemon_fap->gblink_handle);
        pokemon_fap->gen3_cart_ui = ui;
    }
    return pokemon_fap->gen3_cart_ui;
}

void gen3_cart_ui_end(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = pokemon_fap->gen3_cart_ui;

    if(ui) {
        if(ui->session) gen3_cart_session_free(ui->session);
        free(ui);
        pokemon_fap->gen3_cart_ui = NULL;
    }
    if(pokemon_fap->gen3_cart_progress) {
        gen3_cart_progress_free(
            pokemon_fap->view_dispatcher,
            AppViewGen3CartProgress,
            pokemon_fap->gen3_cart_progress);
        pokemon_fap->gen3_cart_progress = NULL;
    }
}

void gen3_cart_ui_leave(PokemonFap* pokemon_fap) {
    if(pokemon_fap->gen3_cart_ui) gen3_cart_session_cancel(pokemon_fap->gen3_cart_ui->session);
    scene_manager_stop(pokemon_fap->scene_manager);
    view_dispatcher_stop(pokemon_fap->view_dispatcher);
}

const Gen3CartSessionStatus* gen3_cart_ui_status(PokemonFap* pokemon_fap) {
    if(pokemon_fap->gen3_cart_ui) {
        gen3_cart_session_status(pokemon_fap->gen3_cart_ui->session, &gen3_cart_status);
    } else {
        memset(&gen3_cart_status, 0, sizeof(gen3_cart_status));
    }
    return &gen3_cart_status;
}

Gen3Cart* gen3_cart_ui_cart(PokemonFap* pokemon_fap) {
    if(!pokemon_fap->gen3_cart_ui) return NULL;
    return gen3_cart_session_cart(pokemon_fap->gen3_cart_ui->session);
}

uint16_t gen3_cart_ui_changes(PokemonFap* pokemon_fap) {
    const Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    return c ? gen3_cart_changes(c) : 0;
}

static void gen3_cart_dialog_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;
    uint32_t event;

    switch(result) {
    case DialogExResultLeft:
        event = GEN3_CART_EVENT_LEFT;
        break;
    case DialogExResultCenter:
        event = GEN3_CART_EVENT_CENTER;
        break;
    case DialogExResultRight:
        event = GEN3_CART_EVENT_RIGHT;
        break;
    default:
        return;
    }
    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, event);
}

void gen3_cart_ui_dialog(PokemonFap* pokemon_fap) {
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    dialog_ex_reset(dialog_ex);
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, gen3_cart_dialog_callback);
}

void gen3_cart_ui_progress(PokemonFap* pokemon_fap, Gen3CartScreenState state) {
    Gen3CartScreenProgress p;

    gen3_cart_screen_progress(gen3_cart_ui_status(pokemon_fap), state, &p);
    gen3_cart_progress_set_hint(pokemon_fap->gen3_cart_progress, p.hint);
    gen3_cart_progress_set(pokemon_fap->gen3_cart_progress, p.title, p.line1, p.line2, p.percent);
}

void gen3_cart_ui_run_job(PokemonFap* pokemon_fap, Gen3CartJob job, const char* arg) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    ui->job = job;
    snprintf(ui->arg, sizeof(ui->arg), "%s", arg ? arg : "");
    ui->job_started = false;
    ui->job_stopping = false;
    ui->result_back = PokemonSceneGen3Cart;
    if(!scene_manager_search_and_switch_to_previous_scene(
           pokemon_fap->scene_manager, PokemonSceneGen3CartJobRun)) {
        scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartJobRun);
    }
}

void gen3_cart_ui_set_result(
    PokemonFap* pokemon_fap,
    Gen3CartJob job,
    Gen3CartStep step,
    Gen3CartResult r,
    bool dropped,
    uint32_t back) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3CartScreenResult res;

    gen3_cart_screen_result(job, step, r, dropped, &res);
    ui->result = r;
    snprintf(ui->result_header, sizeof(ui->result_header), "%s", res.header);
    ui->result_text = res.text;
    ui->retry_mode = res.retry;
    ui->result_back = back;
}

/* ---- The hub ---- */

static void gen3_cart_item_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

void pokemon_scene_gen3_cart_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Submenu* submenu = pokemon_fap->submenu;
    Gen3Cart* c = gen3_cart_session_cart(gen3_cart_ui_get(pokemon_fap)->session);
    const Gen3CartId* id = gen3_cart_id(c);
    Gen3CartScreenHub items;

    gen3_cart_screen_hub(
        gen3_cart_ui_status(pokemon_fap), gen3_cart_backup_current(c), id->can_read_save, &items);

    submenu_reset(submenu);
    submenu_set_header(submenu, id->title[0] ? id->title : "Cartridge");
    submenu_add_item(
        submenu, "Cart info", GEN3_CART_ITEM_INFO, gen3_cart_item_callback, pokemon_fap);
    if(items.reconnect) {
        submenu_add_item(
            submenu, "Reconnect", GEN3_CART_ITEM_RECONNECT, gen3_cart_item_callback, pokemon_fap);
    }
    if(items.backup) {
        submenu_add_item(
            submenu, "Back up again", GEN3_CART_ITEM_BACKUP, gen3_cart_item_callback, pokemon_fap);
    }
    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Cart));

    gen3_menu_show_menu(pokemon_fap);
}

bool pokemon_scene_gen3_cart_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    SceneManager* scene_manager = pokemon_fap->scene_manager;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case GEN3_CART_ITEM_INFO:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            scene_manager_next_scene(scene_manager, PokemonSceneGen3CartInfo);
            consumed = true;
            break;
        case GEN3_CART_ITEM_RECONNECT:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobReconnect, NULL);
            consumed = true;
            break;
        case GEN3_CART_ITEM_BACKUP:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobBackup, NULL);
            consumed = true;
            break;
        case GEN3_MENU_EVENT_DATA_OK:
            if(gen3_menu_dialog_up()) {
                gen3_menu_show_menu(pokemon_fap);
                consumed = true;
            }
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(gen3_menu_dialog_up()) {
            /* Back on the "Data missing" dialog returns to the hub */
            gen3_menu_show_menu(pokemon_fap);
        } else if(gen3_cart_ui_changes(pokemon_fap) > 0) {
            scene_manager_next_scene(scene_manager, PokemonSceneGen3CartLeave);
        } else {
            gen3_cart_ui_leave(pokemon_fap);
        }
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_cart_on_exit(void* context) {
    UNUSED(context);
}
