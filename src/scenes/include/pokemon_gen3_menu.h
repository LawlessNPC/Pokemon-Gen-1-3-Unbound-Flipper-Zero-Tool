#pragma once

#include <src/include/pokemon_app.h>

/* Custom event a Gen III editor scene may send itself (from a view callback) to reach
 * gen3_menu_show_data_missing from its on_event. Clear of scene ids and the other Gen III
 * events. */
#define GEN3_EVENT_DATA_MISSING 0x47330102u

/* The bulk data file (gen3_bulk.bin) is missing, corrupt or unreadable: returns to the Gen III menu
 * from any Gen III scene above it, and shows the "Data missing" dialog there. OK or Back on the
 * dialog shows the menu. Call from a scene's on_event, not from a view callback. */
void gen3_menu_show_data_missing(PokemonFap* pokemon_fap);

/* Custom events: a Gen III editor scene sends GEN3_EVENT_NO_SHINY itself (from a view callback)
 * to reach gen3_menu_show_notice from its on_event; the notice sends GEN3_EVENT_NOTICE_OK on OK.
 * Clear of scene ids and the other Gen III events. */
#define GEN3_EVENT_NO_SHINY 0x47330103u
#define GEN3_EVENT_NOTICE_OK 0x47330104u

/* Shows a dialog with header, text and OK over the current Gen III editor scene. OK sends that
 * scene GEN3_EVENT_NOTICE_OK, and Back reaches it as a Back event; it then leaves with
 * scene_manager_previous_scene. The strings must outlive the dialog (string literals). Call from
 * a scene's on_event. */
void gen3_menu_show_notice(PokemonFap* pokemon_fap, const char* header, const char* text);

/* Header (up to 24 characters) and text (up to 96) of the Gen3Result scene, copied; set them
 * before scene_manager_next_scene(..., PokemonSceneGen3Result). */
void pokemon_scene_gen3_result_set(const char* header, const char* text);
