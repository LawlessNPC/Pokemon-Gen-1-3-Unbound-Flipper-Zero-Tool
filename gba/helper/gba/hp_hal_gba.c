#include "hp_hal.h"

/* hp_hal.h on the GBA, with the registers of multiboot-and-save.md §4.7-§4.8 (GBATEK "GBA I/O
 * Map", "GBA Timers"). Interrupts stay off for the helper's whole life, so everything polls. */

#define HP_REG16(a) (*(volatile uint16_t*)(a))
#define HP_TM2CNT_L HP_REG16(0x04000108u)
#define HP_TM2CNT_H HP_REG16(0x0400010Au)
#define HP_TM3CNT_L HP_REG16(0x0400010Cu)
#define HP_TM3CNT_H HP_REG16(0x0400010Eu)
#define HP_BACKDROP HP_REG16(0x05000000u) /* palette entry 0, BGR555 */

#define HP_ROM_BASE 0x08000000u
#define HP_FLASH_BASE 0x0E000000u
#define HP_EWRAM_BASE 0x02000000u /* where the BIOS loaded the image, header first */

/* TM2: prescaler 256 from 65469, overflowing every 67 * 256 cycles = 1.0224 ms; TM3 counts those
 * overflows. */
#define HP_TM2_RELOAD 65469u
#define HP_TM2_START 0x0082u /* enable, prescaler 256 */
#define HP_TM3_START 0x0084u /* enable, count-up */

/* From IWRAM, as the ROM scan's inner loop calls them once per word: ARM to ARM, no veneer. */
HP_IWRAM uint16_t hp_hal_rom16(uint32_t off) {
    return *(volatile const uint16_t*)(HP_ROM_BASE + off);
}

HP_IWRAM uint32_t hp_hal_rom32(uint32_t off) {
    return *(volatile const uint32_t*)(HP_ROM_BASE + off);
}

/* Flash reads, status polls included, must run from RAM (GBATEK "GBA Cart Backup Flash ROM"). */
HP_IWRAM uint8_t hp_hal_flash_read(uint16_t off) {
    return *(volatile const uint8_t*)(HP_FLASH_BASE + off);
}

HP_IWRAM void hp_hal_flash_write(uint16_t off, uint8_t v) {
    *(volatile uint8_t*)(HP_FLASH_BASE + off) = v;
}

void hp_hal_timer_start(void) {
    HP_TM2CNT_H = 0;
    HP_TM3CNT_H = 0;
    HP_TM2CNT_L = HP_TM2_RELOAD;
    HP_TM3CNT_L = 0;
    HP_TM3CNT_H = HP_TM3_START; /* both reload as they start */
    HP_TM2CNT_H = HP_TM2_START;
}

uint16_t hp_hal_timer_units(void) {
    return HP_TM3CNT_L;
}

void hp_hal_id_delay(void) {
    for(volatile uint16_t i = 20000; i != 0; i--) {
    }
}

const uint8_t* hp_hal_own_header(void) {
    return (const uint8_t*)HP_EWRAM_BASE;
}

void hp_hal_status(HpStatus s) {
    switch(s) {
    case HpStatusReady:
        HP_BACKDROP = 0x03E0; /* green */
        break;
    case HpStatusBusy:
        HP_BACKDROP = 0x03FF; /* yellow */
        break;
    case HpStatusWriting:
        HP_BACKDROP = 0x001F; /* red */
        break;
    case HpStatusRefused:
        HP_BACKDROP = 0x7C00; /* blue */
        break;
    }
}

/* ---- The EZ-Flash Omega's microSD ----
 * Read as the Omega Definitive Edition's own kernel reads it: ez-flash/omega-de-kernel,
 * source/Ezcard_OP.c (SetSDControl, Wait_SD_Response, Read_SD_sectors), Apache License 2.0,
 * Copyright EZ-FLASH. At the kernel's WAITCNT, 0: it never sets one. Reads only. */

#define HP_WAITCNT_REG HP_REG16(0x04000204u)
#define HP_REG32(a) (*(volatile uint32_t*)(a))
#define HP_DMA3SAD HP_REG32(0x040000D4u)
#define HP_DMA3DAD HP_REG32(0x040000D8u)
#define HP_DMA3CNT HP_REG32(0x040000DCu)
#define HP_OMEGA_DATA 0x09E00000u /* the blocks read, or while reading the card its state */
#define HP_OMEGA_BUSY 0xEEE1u
#define HP_OMEGA_WAIT_POLLS 0x100000u /* the kernel's */
#define HP_OMEGA_TRIES 3u
#define HP_OMEGA_RETRY_DELAY 5000u /* the kernel's delay(5000) before a second try */

static void hp_omega_unlock(void) {
    HP_REG16(0x09FE0000u) = 0xD200u;
    HP_REG16(0x08000000u) = 0x1500u;
    HP_REG16(0x08020000u) = 0xD200u;
    HP_REG16(0x08040000u) = 0x1500u;
}

/* 0 off, 1 on, 3 reading */
static void hp_omega_sd_control(uint16_t control) {
    hp_omega_unlock();
    HP_REG16(0x09400000u) = control;
    HP_REG16(0x09FC0000u) = 0x1500u;
}

static bool hp_omega_wait(void) {
    for(uint32_t n = 0; n < HP_OMEGA_WAIT_POLLS; n++) {
        if(HP_REG16(HP_OMEGA_DATA) != HP_OMEGA_BUSY) return true;
    }
    return false;
}

bool hp_hal_omega_sd_read(uint32_t lba, uint8_t count, uint16_t* out) {
    if(count < 1u || count > 4u) return false;
    const uint16_t waitcnt = HP_WAITCNT_REG;
    HP_WAITCNT_REG = 0;
    bool ok = false;
    hp_omega_sd_control(1);
    for(uint32_t t = 0; t < HP_OMEGA_TRIES && !ok; t++) {
        if(t) {
            for(volatile uint32_t d = 0; d < HP_OMEGA_RETRY_DELAY; d++) {
            }
        }
        hp_omega_unlock();
        HP_REG16(0x09600000u) = (uint16_t)lba;
        HP_REG16(0x09620000u) = (uint16_t)(lba >> 16);
        HP_REG16(0x09640000u) = count;
        HP_REG16(0x09FC0000u) = 0x1500u;
        hp_omega_sd_control(3);
        ok = hp_omega_wait();
        hp_omega_sd_control(1);
    }
    if(ok) {
        /* DMA3, 16-bit, both addresses incrementing, at once: the kernel's dmaCopy */
        HP_DMA3SAD = HP_OMEGA_DATA;
        HP_DMA3DAD = (uint32_t)out;
        HP_DMA3CNT = 0x80000000u | (256u * count);
        __asm__ volatile("" ::: "memory");
    }
    hp_omega_sd_control(0);
    HP_WAITCNT_REG = waitcnt;
    return ok;
}
