#include <src/gen3/gen3_pk3_furi.h>

bool gen3_pk3_dir(Storage* storage, FuriString* out) {
    if(storage_sd_status(storage) != FSE_OK) return false;

    furi_string_set(out, APP_DATA_PATH("gen3"));
    storage_common_resolve_path_and_ensure_app_directory(storage, out);
    /* Only the app's own directory is created above, not this subfolder */
    return storage_simply_mkdir(storage, furi_string_get_cstr(out));
}

Gen3Check gen3_pk3_load_file(
    Storage* storage,
    const char* path,
    Gen3Pkm* out,
    Gen3ImportInfo* info) {
    uint8_t buf[GEN3_PARTY_SIZE];
    Gen3Check ret = Gen3CheckFileError;
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t size = storage_file_size(file);
        if(size != GEN3_BOX_SIZE && size != GEN3_PARTY_SIZE) {
            ret = Gen3CheckBadSize;
        } else if(storage_file_read(file, buf, size) == size) {
            ret = gen3_pk3_import(buf, (size_t)size, out, info);
        }
    }

    storage_file_free(file); /* closes it if open */
    return ret;
}

bool gen3_pk3_save_file(Storage* storage, const char* path, const Gen3Pkm* p) {
    uint8_t buf[GEN3_BOX_SIZE];
    bool ret = false;
    File* file = storage_file_alloc(storage);

    gen3_pk3_export(p, buf);
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ret = storage_file_write(file, buf, sizeof(buf)) == sizeof(buf);
    }

    storage_file_free(file);
    return ret;
}
