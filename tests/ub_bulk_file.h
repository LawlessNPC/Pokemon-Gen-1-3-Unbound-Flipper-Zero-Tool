#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <src/gen3/gen3_ub_data.h>

/* Host-only Gen3UbRead source over the committed asset. The tests run from tests/. */

#define UB_BULK_FILE_PATH "../files/unbound_bulk.bin"

static FILE* ub_bulk_file_fp = NULL;

/* Set after a good open to make every read fail from then on, as an SD error would */
static bool ub_bulk_file_failing = false;

static inline bool ub_bulk_file_read(void* ctx, uint32_t offset, void* buf, uint32_t len) {
    FILE* f = ctx;
    return !ub_bulk_file_failing && fseek(f, (long)offset, SEEK_SET) == 0 &&
           fread(buf, 1, len, f) == len;
}

/* Opens the file once and installs it, with reads working */
static inline bool ub_bulk_file_open(void) {
    ub_bulk_file_failing = false;
    if(!ub_bulk_file_fp) ub_bulk_file_fp = fopen(UB_BULK_FILE_PATH, "rb");
    if(ub_bulk_file_fp && gen3_ub_data_open(ub_bulk_file_read, ub_bulk_file_fp)) return true;
    printf("cannot open %s as the Unbound data source\n", UB_BULK_FILE_PATH);
    return false;
}
