#include "fake_gba.h"
#include <string.h>

static FakeGba* fake_gba_now = NULL;

void fake_gba_init(FakeGba* g) {
    memset(g, 0, sizeof(*g));
    for(uint32_t i = 0; i < 156; i++) g->own[4 + i] = (uint8_t)(i * 37u + 11u);
    g->own[0xC4] = 3;
    g->own[0xC5] = 1;
    g->status = HpStatusReady;
    fake_gba_now = g;
}

FakeGba* fake_gba_current(void) {
    return fake_gba_now;
}

const uint8_t* hp_hal_own_header(void) {
    return fake_gba_now->own;
}

void hp_hal_timer_start(void) {
    fake_gba_now->timer_units = 0;
}

uint16_t hp_hal_timer_units(void) {
    return fake_gba_now->timer_units;
}

void hp_hal_status(HpStatus s) {
    fake_gba_now->status = s;
}
