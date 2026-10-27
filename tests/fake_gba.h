#pragma once

#include <stdint.h>
#include <hp_hal.h>

/* Test-only model of the GBA the helper's portable core runs on, behind hp_hal.h: its own header,
 * the TM3 timer and the status backdrop. The hp_hal_* functions act on the FakeGba that
 * fake_gba_init initialised last. It shares nothing with the Flipper's code. Never a stack local
 * in a test: static or heap. */

#define FAKE_GBA_OWN_LEN 0x100u

typedef struct FakeGba FakeGba;

struct FakeGba {
    /* The helper's image from 0x02000000: the header at 0x00..0xBF, then the multiboot entries,
     * where the BIOS writes the boot mode at 0xC4 and the slave id at 0xC5 */
    uint8_t own[FAKE_GBA_OWN_LEN];
    uint16_t timer_units; /* what hp_hal_timer_units returns; hp_hal_timer_start zeroes it */
    HpStatus status; /* the last hp_hal_status */
};

/* The own header gets a test logo (a fixed 156-byte pattern, not the real one) at 0x04..0x9F,
 * 3 (MultiPlay) at 0xC4 and 1 (slave id) at 0xC5; the timer reads 0; the status is Ready. */
void fake_gba_init(FakeGba* g);
FakeGba* fake_gba_current(void);
