#include <src/views/gen3_ub_pic_select.h>

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <gui/elements.h>
#include <gui/view.h>

#include <src/views/gen3_species_select.h>

#define GEN3_UB_PIC_DIM 56

typedef struct {
    View* view;
    Gen3UbPicSelectCallback cb;
    void* ctx;
} Gen3UbPicSelect;

typedef struct {
    char name[12];
    uint16_t n, m;
    bool has_picture;
    uint8_t picture[GEN3_FXBM_DATA_SIZE]; /* the width, the height, then the pixels */
} Gen3UbPicSelectModel;

static void gen3_ub_pic_select_draw(Canvas* canvas, void* model_ptr) {
    const Gen3UbPicSelectModel* model = model_ptr;
    char buf[16];

    canvas_clear(canvas);
    if(model->has_picture) {
        canvas_draw_xbm(
            canvas, 0, 0, GEN3_UB_PIC_DIM, GEN3_UB_PIC_DIM, model->picture + 8);
    } else {
        canvas_draw_rframe(canvas, 4, 4, 48, 48, 4);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 28, 28, AlignCenter, AlignCenter, "?");
    }
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 58, 12, AlignLeft, AlignTop, model->name);
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "< %u/%u >", model->n, model->m);
    canvas_draw_str_aligned(canvas, 58, 28, AlignLeft, AlignTop, buf);
    canvas_set_font(canvas, FontPrimary);
    elements_button_center(canvas, "OK");
}

static bool gen3_ub_pic_select_input(InputEvent* event, void* context) {
    Gen3UbPicSelect* ps = context;
    uint32_t what;

    if(event->type != InputTypePress && event->type != InputTypeRepeat) return false;
    switch(event->key) {
    case InputKeyLeft:
        what = GEN3_UB_PIC_LEFT;
        break;
    case InputKeyRight:
        what = GEN3_UB_PIC_RIGHT;
        break;
    case InputKeyOk:
        /* only a press, so a held OK doesn't fire twice */
        if(event->type != InputTypePress) return true;
        what = GEN3_UB_PIC_OK;
        break;
    default:
        return false;
    }
    if(ps->cb) ps->cb(ps->ctx, what);
    return true;
}

void* gen3_ub_pic_select_alloc(ViewDispatcher* vd, uint32_t view_id) {
    Gen3UbPicSelect* ps = malloc(sizeof(Gen3UbPicSelect));

    memset(ps, 0, sizeof(*ps));
    ps->view = view_alloc();
    view_set_context(ps->view, ps);
    view_allocate_model(ps->view, ViewModelTypeLocking, sizeof(Gen3UbPicSelectModel));
    view_set_draw_callback(ps->view, gen3_ub_pic_select_draw);
    view_set_input_callback(ps->view, gen3_ub_pic_select_input);
    view_dispatcher_add_view(vd, view_id, ps->view);
    return ps;
}

void gen3_ub_pic_select_free(ViewDispatcher* vd, uint32_t view_id, void* p) {
    Gen3UbPicSelect* ps = p;

    view_dispatcher_remove_view(vd, view_id);
    view_free(ps->view);
    free(ps);
}

void gen3_ub_pic_select_set_callback(void* p, Gen3UbPicSelectCallback cb, void* ctx) {
    Gen3UbPicSelect* ps = p;

    ps->cb = cb;
    ps->ctx = ctx;
}

bool gen3_ub_pic_select_set(
    void* p,
    const char* name,
    uint16_t n,
    uint16_t m,
    const char* path,
    uint32_t index,
    uint32_t size) {
    Gen3UbPicSelect* ps = p;
    bool has = false;

    with_view_model(
        ps->view,
        Gen3UbPicSelectModel * model,
        {
            snprintf(model->name, sizeof(model->name), "%s", name);
            model->n = n;
            model->m = m;
            has = path && gen3_fxbm_read(path, index, size, model->picture);
            model->has_picture = has;
        },
        true);
    return has;
}
