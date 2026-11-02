#include <furi.h>
#include <storage/storage.h>

#include <src/gen3/gen3_data.h>

#ifdef POKEMON_CART_APP
static bool gen3_data_tables_furi_read(void* ctx, uint32_t offset, void* buf, uint32_t len) {
    File* file = ctx;
    return storage_file_seek(file, offset, true) && storage_file_read(file, buf, len) == len;
}

/* The tables are copied to the heap, so the file is closed again before this returns */
bool gen3_data_tables_load(void) {
    if(gen3_data_tables_ready()) return true;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(
                  file, APP_ASSETS_PATH("gen3_cart_tables.bin"), FSAM_READ, FSOM_OPEN_EXISTING) &&
              gen3_data_tables_open(gen3_data_tables_furi_read, file);
    storage_file_free(file); /* closes it if open */
    furi_record_close(RECORD_STORAGE);
    return ok;
}
#endif
