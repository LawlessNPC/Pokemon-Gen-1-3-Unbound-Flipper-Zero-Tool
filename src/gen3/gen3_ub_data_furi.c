#include <src/gen3/gen3_ub_data_furi.h>

#include <furi.h>
#include <storage/storage.h>

#include <src/gen3/gen3_ub_data.h>

static File* gen3_ub_file = NULL;

static bool gen3_ub_furi_read(void* ctx, uint32_t offset, void* buf, uint32_t len) {
    File* file = ctx;
    return storage_file_seek(file, offset, true) && storage_file_read(file, buf, len) == len;
}

bool gen3_ub_data_furi_open(void) {
    gen3_ub_data_furi_close();
    gen3_ub_file = storage_file_alloc(furi_record_open(RECORD_STORAGE));
    if(storage_file_open(
           gen3_ub_file, APP_ASSETS_PATH("unbound_bulk.bin"), FSAM_READ, FSOM_OPEN_EXISTING) &&
       gen3_ub_data_open(gen3_ub_furi_read, gen3_ub_file)) {
        return true;
    }
    gen3_ub_data_furi_close();
    return false;
}

void gen3_ub_data_furi_close(void) {
    gen3_ub_data_close();
    if(gen3_ub_file) {
        storage_file_free(gen3_ub_file); /* closes it if open */
        gen3_ub_file = NULL;
        furi_record_close(RECORD_STORAGE);
    }
}
