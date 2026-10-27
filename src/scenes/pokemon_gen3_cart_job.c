#include <src/scenes/include/pokemon_gen3_cart.h>

#include <furi.h>
#include <src/views/gen3_cart_progress.h>

/* Runs the job gen3_cart_ui_run_job set, once per run, on the progress view, then shows its
 * result. Coming back to it (from a scene over it) finds the job running and doesn't start it
 * again. Back stops a job that can stop; a Write, Restore or Discard runs to its end. */

/* The session takes the job once the last one has ended; until then it is tried on every tick */
static void gen3_cart_job_start(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(ui->job_started) return;
    ui->job_started =
        gen3_cart_session_run(ui->session, ui->job, ui->arg[0] != '\0' ? ui->arg : NULL);
    if(ui->job_started) gen3_cart_ui_progress(pokemon_fap, Gen3CartScreenStarting);
}

static void gen3_cart_job_tick(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(!ui->job_started) {
        gen3_cart_job_start(pokemon_fap);
        return;
    }
    const Gen3CartSessionStatus* st = gen3_cart_ui_status(pokemon_fap);
    if(st->busy) {
        gen3_cart_ui_progress(
            pokemon_fap, ui->job_stopping ? Gen3CartScreenStopping : Gen3CartScreenRunning);
        return;
    }
    gen3_cart_ui_set_result(
        pokemon_fap, ui->job, st->step, st->result, false, PokemonSceneGen3Cart);
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartResult);
}

static void gen3_cart_job_back(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(!ui->job_started) return; /* nothing runs yet: its first tick starts it */
    if(ui->job_stopping || gen3_cart_screen_back(ui->job) != Gen3CartBackStop) return;
    gen3_cart_session_cancel(ui->session);
    ui->job_stopping = true;
    gen3_cart_ui_progress(pokemon_fap, Gen3CartScreenStopping);
}

void pokemon_scene_gen3_cart_job_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(ui->job_started) {
        gen3_cart_ui_progress(
            pokemon_fap, ui->job_stopping ? Gen3CartScreenStopping : Gen3CartScreenRunning);
    } else {
        gen3_cart_job_start(pokemon_fap);
        if(!ui->job_started) {
            /* The status is still the last job's: show none of it */
            gen3_cart_progress_set_hint(pokemon_fap->gen3_cart_progress, "");
            gen3_cart_progress_set(pokemon_fap->gen3_cart_progress, "Starting", "", "", -1);
        }
    }
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewGen3CartProgress);
}

bool pokemon_scene_gen3_cart_job_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GEN3_CART_EVENT_TICK) {
            gen3_cart_job_tick(pokemon_fap);
            consumed = true;
        } else if(event.event == GEN3_CART_EVENT_BACK) {
            gen3_cart_job_back(pokemon_fap);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        /* The progress view keeps Back: this never pops the scene with a job running */
        gen3_cart_job_back(pokemon_fap);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_cart_job_on_exit(void* context) {
    UNUSED(context);
}
