#ifndef GEN3_SPECIES_SELECT_H
#define GEN3_SPECIES_SELECT_H

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <gui/view_dispatcher.h>

/* Gen III species picker: national 1..386, each with a sprite (GEN3_SPRITE_MAX_NATIONAL); a
 * sprite that can't be read from the SD card shows a "?" frame. */

/* Called with the national number when OK is pressed. */
typedef void (*Gen3SpeciesSelectCallback)(void* context, uint16_t national);

/* Allocates its own view and adds it to view_dispatcher as view_id. */
void* gen3_species_select_alloc(ViewDispatcher* view_dispatcher, uint32_t view_id);

void gen3_species_select_free(ViewDispatcher* view_dispatcher, uint32_t view_id, void* select);

/* Sets the species shown and the OK callback. Call before switching to the view. */
void gen3_species_select_start(
    void* select,
    uint16_t national,
    Gen3SpeciesSelectCallback cb,
    void* context);

/* Reads the picture of record `index` of an .fxbm file (a u32 record size, a u32 width, a u32
 * height and 392 bytes of 56x56 XBM per record) into out, which takes GEN3_FXBM_DATA_SIZE bytes:
 * the width, the height, then the pixels. size, if not 0, is the file's exact size. false if the
 * file or the record can't be read. Not from the draw callback. */
#define GEN3_FXBM_DATA_SIZE 400u
bool gen3_fxbm_read(const char* path, uint32_t index, uint32_t size, uint8_t* out);

#endif /* GEN3_SPECIES_SELECT_H */
