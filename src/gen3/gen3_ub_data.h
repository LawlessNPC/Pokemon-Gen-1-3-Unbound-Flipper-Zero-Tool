#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Pokemon Unbound's game data by its own species id, from files/unbound_bulk.bin on the SD card,
 * which tools/unbound_tables.py makes from PUSE's data (github.com/Zannael/PUSE, MIT). The
 * accessors read through the installed source: call them from the app's threads, never an ISR. */

#define GEN3_UB_SPECIES_MAX 1267u /* species ids 1..1267 have names */
#define GEN3_UB_NAME_LEN 10u

typedef bool (*Gen3UbRead)(void* ctx, uint32_t offset, void* buf, uint32_t len);

/* Installs read as the source if the file's header, table directory and CRC-32 check out; else
 * leaves no source. */
bool gen3_ub_data_open(Gen3UbRead read, void* ctx);
void gen3_ub_data_close(void);
bool gen3_ub_data_ok(void);
/* The species' name, uppercase; false (out "#<id>") for an id with none, or a read failure */
bool gen3_ub_species_name(uint16_t species, char out[GEN3_UB_NAME_LEN + 1u]);
/* The growth rate 0..5, -1 if unknown or unreadable. A Gen3UbGrowthFn (ctx unused). */
int gen3_ub_growth(uint16_t species, void* ctx);
/* The National Dex number, 0 if unmapped or unreadable */
uint16_t gen3_ub_national(uint16_t species);
