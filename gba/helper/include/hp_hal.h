#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What the helper's portable core needs from the machine it runs on. Implemented by
 * gba/hp_hal_gba.c on the GBA (registers of multiboot-and-save.md §4.7-§4.8) and by
 * tests/fake_gba.c on the host. */

/* Flash and hash routines run from IWRAM in ARM state on the GBA (32-bit bus, no wait states);
 * crt0.S copies the .iwram section there. On the host it means nothing. */
#ifdef HP_GBA
#define HP_IWRAM __attribute__((section(".iwram"), target("arm"), noinline))
#else
#define HP_IWRAM
#endif

/* The status backdrop (multiboot-and-save.md §4.8 rule 4) */
typedef enum {
    HpStatusReady, /* green */
    HpStatusBusy, /* yellow */
    HpStatusWriting, /* red: a flash write is in progress, do not power off */
    HpStatusRefused, /* blue: the cart was refused */
} HpStatus;

/* Cart ROM at 0x08000000 + off; off even, < 0x02000000 */
uint16_t hp_hal_rom16(uint32_t off);
/* off % 4 == 0 */
uint32_t hp_hal_rom32(uint32_t off);
/* The save chip at 0x0E000000 + off, 8-bit accesses only */
uint8_t hp_hal_flash_read(uint16_t off);
void hp_hal_flash_write(uint16_t off, uint8_t v);
/* Restarts the TM2/TM3 cascade (multiboot-and-save.md §4.7) */
void hp_hal_timer_start(void);
/* 1.0224 ms units since hp_hal_timer_start */
uint16_t hp_hal_timer_units(void);
/* The delay around reading a flash ID: a 20000-iteration volatile loop
 * (pokeruby: src/libs/agb_flash.c: ReadFlashId, its DELAY) */
void hp_hal_id_delay(void);
/* The helper's own image from its start: the 0xC0-byte header, then [0xC4] and [0xC5], where the
 * BIOS writes the boot mode and the slave id (multiboot-and-save.md §2.1) */
const uint8_t* hp_hal_own_header(void);
void hp_hal_status(HpStatus s);
/* Blocks lba .. lba + count - 1 (count 1..4) of an EZ-Flash Omega's microSD, read through the
 * cart's SD window as the Omega's own kernel does (gba/hp_hal_gba.c): count * 256 halfwords into
 * out, little-endian. false if the card never answered. */
bool hp_hal_omega_sd_read(uint32_t lba, uint8_t count, uint16_t* out);
/* Blocks lba .. lba + count - 1 (count 1..4) of the Omega's microSD from data (count * 256
 * halfwords, little-endian), through the same window, as the Omega's own kernel writes them
 * (gba/hp_hal_gba.c). false if the card never answered; it is switched off again either way. No
 * retry. */
bool hp_hal_omega_sd_write(uint32_t lba, uint8_t count, const uint16_t* data);
