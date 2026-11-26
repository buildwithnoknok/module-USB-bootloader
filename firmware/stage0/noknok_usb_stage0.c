/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * noknok USB Stage-0  v1.0.0   CH32V203 (flash layout 4)   |  Stack: cnlohr/ch32fun
 * ============================================================================
 * THIS COMPONENT IS FROZEN. Once it is on a module it can only be replaced with
 * the BOOT0 jumper (WCHISPTool) or SWD. Everything above it - stage-1 (the USB
 * CDC bootloader), the module application - is updatable over USB. Stage-0 is
 * what makes stage-1 updatable, so it must stay small enough to be correct by
 * inspection. DEV-237.
 *
 * Derived line by line from the I2C stage-0 v1.0.0 (module-I2C-bootloader,
 * firmware/stage0/noknok_stage0.c, bench-validated in DEV-31 incl. a power cut
 * at every page). Only three things differ:
 *   - the flash routines are the CH32V20x FAST 256-byte page path (MODEKEYR
 *     unlock, CR_PAGE_ER / CR_PAGE_PG + WRBSY per word + CR_PG_STRT, EOP and
 *     WRPRTERR cleared), copied from the USB bootloader 1.1.0 that fixed DEV-12.
 *     NEVER the V003 BUF_LOAD/BUF_RST bits: on V20x 0x40000/0x80000 are the
 *     32 KB / 64 KB BLOCK ERASE bits.
 *   - the map: control block 0x7E00, flash end 0x8000 (CH32V203G6, 32 KB).
 *   - the attempt cell is 0x200027F8 (0x200027F4 is the USB stage-1's 0xB2
 *     diagnostic cell, 0x200027F0 the handoff cell).
 *
 * -- Flash map, layout 4 (32 KB) ----------------------------------------------
 *   0x0000_0000  STAGE-0       1 KB     this code - FROZEN
 *   0x0000_0400  STAGE-1       7 KB     USB CDC bootloader - updatable
 *   0x0000_2000  APPLICATION   23.5 KB  module firmware; also the STAGING area
 *   0x0000_7E00  CONTROL BLOCK 256 B    stage-1 update marker (below)
 *   0x0000_7F00  APP METADATA  256 B    stage-1 owns it, stage-0 ignores it
 * The application base did NOT move (0x2000 as in the single-stage bootloader
 * 1.x), so the LED apps run unchanged; only the app region shrinks by the
 * control-block page.
 *
 * -- What it does --------------------------------------------------------------
 *   1. If stage-1 has a pending update, apply it (verified copy from staging).
 *   2. Jump to stage-1.
 * No USB, no clock set-up, no interrupts, no LED, no app check. Stage-0 hardcodes
 * exactly two addresses (STAGE1_BASE, CTRL_FLASH); everything else - staging
 * address, length, the app base that bounds the stage-1 region - is DATA from
 * the control block, so a future stage-1 can move the boundary without touching
 * this file.
 *
 * -- Nothing is erased on unverified data ---------------------------------------
 *   1. the descriptor (words 0-4) must match its own CRC32 (word 5)
 *   2. the geometry must be sane (only the stage-1 region can be written)
 *   3. the staged image must match the CRC32 stage-1 recorded for it
 * Any failure -> boot the untouched stage-1.
 *
 * -- Power-loss safety -----------------------------------------------------------
 * Staging is never erased by the copy, so the copy is repeatable; the marker is
 * cleared LAST. A cut anywhere repeats the whole install on the next boot. Every
 * page is verified after erase and program, with retries; a page that will not
 * take costs a counted warm reset, MAX_ATTEMPTS of them, then halt. A cold
 * power-on reads the counter as fresh. The update path waits ~50 ms first so a
 * slow-ramping rail has settled (DEV-12).
 *
 * -- Recovery of last resort ----------------------------------------------------
 * Halt states (persistent flash failure, no stage-1 programmed) need the BOOT0
 * jumper + WCHISPTool with a full layout-4 image, or SWD.
 */

#include "ch32fun.h"
#include <stdint.h>

/* -- The two frozen addresses ------------------------------------------------- */
#define STAGE1_BASE_EXEC   0x00000400U   /* execution alias - where we jump      */
#define STAGE1_BASE_FLASH  0x08000400U   /* controller alias - where we write    */
#define CTRL_FLASH         0x08007E00U   /* control block page                   */

/* -- Fixed properties of the part (CH32V203G6: 32 KB, 256 B fast pages) -------- */
#define FLASH_END          0x08008000U
#define PAGE               256U
#define PAGE_WORDS         (PAGE / 4U)
#define WRBSY              0x02U         /* STATR: word buffer busy              */

#define CTRL_MAGIC         0x6E6B5530U   /* "nkU0" - a stage-1 update is pending */
#define RETRIES            3U            /* per-page erase/program retries       */
#define MAX_ATTEMPTS       4U            /* whole-install retries (warm resets)  */
#define BSY_TIMEOUT        2000000U      /* a stuck BSY becomes a failed verify  */

/* Control block. Written by stage-1, cleared by stage-0. 0x0800_0000 alias.
 * Field order is a frozen contract, identical to the I2C stage-0. */
typedef struct {
    uint32_t magic;         /* CTRL_MAGIC = update pending; anything else = no  */
    uint32_t staging_addr;  /* where the new stage-1 image is staged            */
    uint32_t stage1_len;    /* its length in bytes                              */
    uint32_t stage1_crc32;  /* CRC32 of the staged image - verified HERE too    */
    uint32_t app_base;      /* application base - bounds the stage-1 region     */
    uint32_t desc_crc32;    /* CRC32 over the five words above                  */
} ctrl_t;

#define CTRL       ((const volatile ctrl_t *)CTRL_FLASH)
#define DESC_BYTES 20U      /* magic .. app_base */

/* Install-attempt counter in no-init RAM (top 16 B, reserved by every linker
 * script). Tag in the high bits; a cold power-on reads as 0. */
#define ATTEMPT_CELL   (*(volatile uint32_t *)0x200027F8U)
#define ATTEMPT_TAG    0x5A000000U
#define ATTEMPT_MASK   0xFF000000U

/* ==============================================================================
 * FLASH - CH32V20x fast 256-byte page path (USB bootloader 1.1.0, DEV-12)
 * Plain CTLR writes, status cleared after every operation.
 * ============================================================================== */

static void fl_wait(void)
{
    for (uint32_t t = BSY_TIMEOUT; t && (FLASH->STATR & FLASH_STATR_BSY); t--);
    FLASH->STATR = FLASH_STATR_EOP | FLASH_STATR_WRPRTERR;   /* write-1-to-clear */
}

static void fl_unlock(void)
{
    RCC->AHBPCENR  |= RCC_FLITFEN;  /* flash interface clock (reset default, kept explicit) */
    FLASH->KEYR     = FLASH_KEY1;   /* program/erase unlock */
    FLASH->KEYR     = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;   /* fast page mode unlock */
    FLASH->MODEKEYR = FLASH_KEY2;
}

static void fl_erase(uint32_t addr)
{
    FLASH->CTLR = CR_PAGE_ER;
    FLASH->ADDR = addr;
    FLASH->CTLR = CR_PAGE_ER | CR_STRT_Set;
    fl_wait();
    FLASH->CTLR = 0;
}

/* src NULL = program 0xFF words. On the CH32V203 a fast page erase leaves the
 * cells reading 0xE339E339, NOT 0xFF (bench, 22 Nov 2026: the first stage-0 build
 * failed its own blank-page verify on exactly that). So a page that must end up
 * blank is erased AND programmed with 0xFF, which reads back as 0xFF. */
static void fl_program(uint32_t addr, const uint32_t *src)
{
    volatile uint32_t *dst = (volatile uint32_t *)addr;
    FLASH->CTLR = CR_PAGE_PG;
    for (uint32_t i = 0; i < PAGE_WORDS; i++) {
        dst[i] = src ? src[i] : 0xFFFFFFFFU;
        for (uint32_t t = BSY_TIMEOUT; t && (FLASH->STATR & WRBSY); t--);
    }
    FLASH->CTLR = CR_PAGE_PG | CR_PG_STRT;
    fl_wait();
    FLASH->CTLR = 0;
}

/* Compare one page against src, or against 0xFF if src is NULL. */
static int page_matches(uint32_t addr, const uint32_t *src)
{
    const volatile uint32_t *p = (const volatile uint32_t *)addr;
    for (uint32_t i = 0; i < PAGE_WORDS; i++) {
        uint32_t want = src ? src[i] : 0xFFFFFFFFU;
        if (p[i] != want) return 0;
    }
    return 1;
}

/* Erase, program (src NULL = 0xFF), then VERIFY. Retries. Returns 0 if it never took. */
static int write_page(uint32_t addr, const uint32_t *src)
{
    for (uint32_t t = 0; t < RETRIES; t++) {
        fl_erase(addr);
        fl_program(addr, src);
        if (page_matches(addr, src)) return 1;
    }
    return 0;
}

static void halt(void)
{
    for (;;);   /* recover with BOOT0 + WCHISPTool or SWD */
}

/* CRC32, zlib (poly 0xEDB88320, init/final 0xFFFFFFFF) - the stage-1's CRC. */
static uint32_t crc32(uint32_t addr, uint32_t len)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint32_t b = 0; b < 8U; b++)
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320U : (crc >> 1);
    }
    return ~crc;
}

/* An install attempt failed: count it, warm reset, redo the install (marker
 * still set). After MAX_ATTEMPTS consecutive attempts, halt. */
static void fail_attempt(void)
{
    uint32_t cell = ATTEMPT_CELL;
    uint32_t n    = ((cell & ATTEMPT_MASK) == ATTEMPT_TAG) ? (cell & 0xFFU) : 0U;
    if (n + 1U >= MAX_ATTEMPTS) halt();
    ATTEMPT_CELL = ATTEMPT_TAG | (n + 1U);
    NVIC_SystemReset();
}

/* ==============================================================================
 * MAIN
 * ============================================================================== */

int main(void)
{
    /* No SystemInit: the reset-default HSI clock is fine for flash programming. */

    if (CTRL->magic == CTRL_MAGIC) {

        /* Update path only: let the rail settle (~50 ms at HSI). */
        for (volatile uint32_t i = 0; i < 120000U; i++);

        uint32_t app_base = CTRL->app_base;
        uint32_t staging  = CTRL->staging_addr;
        uint32_t len      = CTRL->stage1_len;

        /* 1. descriptor intact */
        int ok = crc32(CTRL_FLASH, DESC_BYTES) == CTRL->desc_crc32;

        /* 2. geometry sane: only the stage-1 region, staging below the marker */
        ok = ok &&
            app_base >  STAGE1_BASE_FLASH && app_base <= FLASH_END &&
            (app_base & (PAGE - 1U)) == 0U &&
            len      >  0U   && len <= (app_base - STAGE1_BASE_FLASH) &&
            staging  >= app_base &&
            (staging & (PAGE - 1U)) == 0U &&
            staging + len <= CTRL_FLASH;

        /* 3. staged image is what stage-1 verified */
        ok = ok && crc32(staging, len) == CTRL->stage1_crc32;

        if (ok) {
            uint32_t region = app_base - STAGE1_BASE_FLASH;

            fl_unlock();

            /* Rewrite the whole stage-1 region: image, then 0xFF to its end. */
            for (uint32_t off = 0; off < region; off += PAGE) {
                const uint32_t *src = (off < len)
                                    ? (const uint32_t *)(staging + off)
                                    : (const uint32_t *)0;
                if (!write_page(STAGE1_BASE_FLASH + off, src)) fail_attempt();
            }

            /* Clear the marker LAST (erase + 0xFF program + verify, see fl_program). */
            if (!write_page(CTRL_FLASH, 0)) fail_attempt();

            ATTEMPT_CELL = 0;
            NVIC_SystemReset();
        }
        /* A gate failed: boot the untouched stage-1; the marker stays (never
         * unlock flash on a normal boot). */
    }

    /* Normal boot: if a stage-1 was ever programmed, run it. Both blank values
     * count as "nothing there": 0xFF (factory / programmed blank) and 0xE339E339
     * (fast-erased, never programmed). */
    uint32_t first = *(const volatile uint32_t *)STAGE1_BASE_FLASH;
    if (first != 0xFFFFFFFFU && first != 0xE339E339U)
        asm volatile("jr %0" :: "r"(STAGE1_BASE_EXEC));

    halt();
    return 0;
}
