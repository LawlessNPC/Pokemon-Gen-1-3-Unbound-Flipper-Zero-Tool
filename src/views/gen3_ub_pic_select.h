#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <gui/view_dispatcher.h>

/* The Unbound species picture browser's view: a 56x56 picture (or a "?" frame), the name, "n/m"
 * and OK. Left, Right and OK call the callback with the GEN3_UB_PIC_* below, in the GUI thread: do
 * no more than post an event. It has no timer: its scene polls on the app's tick. */
#define GEN3_UB_PIC_LEFT 0x47330530u
#define GEN3_UB_PIC_RIGHT 0x47330531u
#define GEN3_UB_PIC_OK 0x47330532u

typedef void (*Gen3UbPicSelectCallback)(void* ctx, uint32_t what);

void* gen3_ub_pic_select_alloc(ViewDispatcher* vd, uint32_t view_id);
void gen3_ub_pic_select_free(ViewDispatcher* vd, uint32_t view_id, void* p);
void gen3_ub_pic_select_set_callback(void* p, Gen3UbPicSelectCallback cb, void* ctx);
/* Shows name, n of m, and the picture of record `index` of the .fxbm file path (gen3_fxbm_read,
 * with its size); a NULL path shows "?". true if a picture was read. GUI thread. */
bool gen3_ub_pic_select_set(
    void* p,
    const char* name,
    uint16_t n,
    uint16_t m,
    const char* path,
    uint32_t index,
    uint32_t size);
