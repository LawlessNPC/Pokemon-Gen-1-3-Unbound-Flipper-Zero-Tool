#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>
#include <string.h>

#include <dialogs/dialogs.h>
#include <furi.h>
#include <storage/storage.h>
#include <src/gen3/gen3_pk3_furi.h>

/* Restore backup: the file browser in the backups folder, then the Restore check job on the file
 * picked (gen3_cart_restore_check), whose Ok goes on to Restore backup? over the hub
 * (Gen3CartRestoreAsk). The browser's Back returns to the hub. The hub offers this only while
 * linked and gen3_cart_restorable, which asks for the identification's can_write, not for
 * gen3_cart_writable: a cart whose save is empty or damaged, which isn't writable, can still
 * have a backup put back.
 *
 * On an EZ-Flash Omega (status.omega) it is a list instead: the folder's newest
 * GEN3_CART_RESTORE_UB_MAX backups/UNBOUND_*.sav, newest first, as "1101-120000 12345" (the
 * stamp's MMDD-HHMMSS and the trainer id). A row chosen runs the same check
 * (gen3_omega_restore_check), and the scene state keeps the highlighted row. Back returns to the
 * hub. */

#define GEN3_CART_RESTORE_DIR GEN3_SHARED_DATA_DIR "/" GEN3_CART_BACKUP_DIR
#define GEN3_CART_RESTORE_UB_MAX 20u
/* Row k's item */
#define GEN3_CART_ITEM_RESTORE_UB 0x473305C0u

/* The Omega's list: the names in the folder, newest first */
static char gen3_cart_restore_ub_names[GEN3_CART_RESTORE_UB_MAX][GEN3_CART_NAME_MAX];
static uint8_t gen3_cart_restore_ub_count = 0;

/* A path the job can't take: "Can't restore", OK back to the hub */
static void gen3_cart_restore_refuse(PokemonFap* pokemon_fap, const char* text) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    snprintf(ui->result_header, sizeof(ui->result_header), "Can't restore");
    ui->result_text = text;
    ui->retry_mode = Gen3CartRetryNone;
    ui->result_back = PokemonSceneGen3Cart;
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartResult);
}

/* Own function, so that nothing of it is in the frame the file browser runs in. The jobs name a
 * file relative to the shared folder, and never take a name cut short. */
__attribute__((noinline)) static void
    gen3_cart_restore_picked(PokemonFap* pokemon_fap, const char* path) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const Gen3CartScreenName n =
        gen3_cart_screen_restore_name(path, GEN3_SHARED_DATA_DIR, ui->restore_name);

    if(n != Gen3CartScreenNameOk) {
        gen3_cart_restore_refuse(pokemon_fap, gen3_cart_screen_restore_name_text(n));
        return;
    }
    gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobRestoreCheck, ui->restore_name);
}

/* name into the list where its stamp puts it, newest first; past the last row, nothing */
static void gen3_cart_restore_ub_insert(const char* name) {
    uint8_t k = 0;
    while(k < gen3_cart_restore_ub_count &&
          !gen3_cart_screen_ub_backup_newer(name, gen3_cart_restore_ub_names[k])) {
        k++;
    }
    if(k >= GEN3_CART_RESTORE_UB_MAX) return;
    const uint8_t last = gen3_cart_restore_ub_count < GEN3_CART_RESTORE_UB_MAX ?
                             gen3_cart_restore_ub_count :
                             (uint8_t)(GEN3_CART_RESTORE_UB_MAX - 1u);
    memmove(
        gen3_cart_restore_ub_names[k + 1u],
        gen3_cart_restore_ub_names[k],
        (size_t)(last - k) * GEN3_CART_NAME_MAX);
    snprintf(
        gen3_cart_restore_ub_names[k],
        GEN3_CART_NAME_MAX,
        "%.*s",
        (int)(GEN3_CART_NAME_MAX - 1u),
        name);
    if(gen3_cart_restore_ub_count < GEN3_CART_RESTORE_UB_MAX) gen3_cart_restore_ub_count++;
}

/* The Unbound backups in the folder; none without an SD card or the folder */
static void gen3_cart_restore_ub_scan(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    FileInfo info;
    char name[64];

    gen3_cart_restore_ub_count = 0;
    if(storage_dir_open(dir, GEN3_CART_RESTORE_DIR)) {
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(!file_info_is_dir(&info) && gen3_cart_screen_ub_backup_ok(name)) {
                gen3_cart_restore_ub_insert(name);
            }
        }
    }
    storage_dir_close(dir); /* required even after a failed open */
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
}

static void gen3_cart_restore_ub_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

/* Own function, so that its buffers aren't in the vanilla browser's frame */
__attribute__((noinline)) static void gen3_cart_restore_ub_enter(PokemonFap* pokemon_fap) {
    Submenu* submenu = pokemon_fap->submenu;
    char label[GEN3_CART_SCREEN_ROW_SIZE];

    gen3_cart_restore_ub_scan();
    if(gen3_cart_restore_ub_count == 0) {
        gen3_cart_restore_refuse(pokemon_fap, GEN3_CART_SCREEN_UB_NO_BACKUPS);
        return;
    }
    submenu_reset(submenu);
    submenu_set_header(submenu, "Restore backup");
    for(uint8_t k = 0; k < gen3_cart_restore_ub_count; k++) {
        gen3_cart_screen_ub_backup_label(gen3_cart_restore_ub_names[k], label);
        submenu_add_item(
            submenu,
            label,
            GEN3_CART_ITEM_RESTORE_UB + k,
            gen3_cart_restore_ub_callback,
            pokemon_fap);
    }
    submenu_set_selected_item(
        submenu,
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartRestore));
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

/* Row k of the Omega's list: the check job on it, as the jobs name it */
static void gen3_cart_restore_ub_picked(PokemonFap* pokemon_fap, uint8_t k) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    snprintf(
        ui->restore_name,
        sizeof(ui->restore_name),
        "%s/%.39s",
        GEN3_CART_BACKUP_DIR,
        gen3_cart_restore_ub_names[k]);
    gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobRestoreCheck, ui->restore_name);
}

void pokemon_scene_gen3_cart_restore_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    if(gen3_cart_ui_status(pokemon_fap)->omega) {
        gen3_cart_restore_ub_enter(pokemon_fap);
        return;
    }
    FuriString* path = furi_string_alloc_set(GEN3_CART_RESTORE_DIR);
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogsFileBrowserOptions options;

    dialog_file_browser_set_basic_options(&options, ".sav", NULL);
    options.base_path = GEN3_CART_RESTORE_DIR;
    const bool picked = dialog_file_browser_show(dialogs, path, path, &options);
    furi_record_close(RECORD_DIALOGS);

    if(picked) {
        gen3_cart_restore_picked(pokemon_fap, furi_string_get_cstr(path));
    } else {
        scene_manager_previous_scene(pokemon_fap->scene_manager);
    }
    furi_string_free(path);
}

bool pokemon_scene_gen3_cart_restore_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    /* The Omega's list; the browser sends nothing. Back: the hub. */
    if(event.type != SceneManagerEventTypeCustom || event.event < GEN3_CART_ITEM_RESTORE_UB ||
       event.event >= GEN3_CART_ITEM_RESTORE_UB + gen3_cart_restore_ub_count) {
        return false;
    }
    scene_manager_set_scene_state(
        pokemon_fap->scene_manager, PokemonSceneGen3CartRestore, event.event);
    gen3_cart_restore_ub_picked(pokemon_fap, (uint8_t)(event.event - GEN3_CART_ITEM_RESTORE_UB));
    return true;
}

void pokemon_scene_gen3_cart_restore_on_exit(void* context) {
    UNUSED(context);
}
