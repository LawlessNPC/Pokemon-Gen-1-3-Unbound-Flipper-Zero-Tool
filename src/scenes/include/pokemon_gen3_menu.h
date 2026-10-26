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
