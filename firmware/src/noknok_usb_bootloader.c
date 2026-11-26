/*
 * noknok USB Module Bootloader - STAGE-1 v2.0.0 - CH32V203, flash layout 4
 * =========================================================================
 * DEV-237 (Nov 2026): the bootloader is split like the I2C one. A frozen 1 KB
 * stage-0 (firmware/stage0/) sits at 0x0000; THIS file is stage-1, relinked to
 * 0x0400, and can now replace itself over USB:
 *
 *   app update:      ERASE -> WRITE xN -> VERIFY(0x04)        -> BOOT
 *   stage-1 update:  ERASE -> WRITE xN -> VERIFY_STAGE1(0x06) -> BOOT
 *
 * VERIFY_STAGE1 CRCs the image staged in the app region, checks its header
 * (NKS1, base 0x400, layout 4, target "V203" - an I2C stage-1 or an app is
 * refused, error 8) and only then writes the control block at 0x7E00. BOOT then
 * resets and stage-0 copies the image into 0x0400 (verified, power-cut safe).
 * The app is destroyed by that (staging = app region) and re-pushed afterwards.
 * New in 2.0.0 as well: 0xB1 GET_VERSION -> [proto, major, minor, patch, layout]
 * like the I2C stage-1, and BOOT refuses an invalid app (stays, error 6).
 *
 * 2.0.0 FLASH MAP (32 KB; also aliased at 0x00000000):
 *   stage-0      0x08000000 .. 0x080003FF   (1 KB, FROZEN, BOOT0/SWD only)
 *   stage-1      0x08000400 .. 0x08001FFF   (7 KB, this image, updatable)
 *   application  0x08002000 .. 0x08007DFF   (23.5 KB, also the staging area)
 *   control blk  0x08007E00 .. 0x08007EFF   (stage-1 update marker for stage-0)
 *   metadata     0x08007F00 .. 0x08007FFF   ({magic, app_len, app_crc32})
 * Existing modules (bootloader 1.x at 0x0000) need ONE full-image re-flash
 * (BOOT0 jumper + WCHISPTool, or SWD) to get stage-0; after that, OTA.
 *
 * ---- history: v1.x text below (single-stage, 8 KB at 0x0000) ----
 * noknok USB Module Bootloader - CH32V203 (v1.0)
 * ================================================
 * USB-CDC OTA bootloader for the noknok USB modules. The USB counterpart of
 * module-I2C-bootloader: re-flashes the application over USB, no programmer and
 * no SWD cable, driven by the host (PC/Pico/app).
 *
 * WHY a custom bootloader (not the WCH factory ROM bootloader):
 *   The CH32V203 factory USB bootloader cannot be reached reliably from running
 *   firmware (STATR boot-mode latch is rejected from firmware; a direct jump to
 *   0x1FFF8000 brings USB up but it never enumerates - it needs a true reset).
 *   So we own the bootloader. Entry = the proven magic+reset pattern (same as the
 *   I2C modules), which we verified works on this chip.
 *
 * FLASH MAP (32 KB; flash is at 0x08000000, also aliased at 0x00000000):
 *   bootloader   0x08000000 .. 0x08001FFF   (8 KB, this image; jumper-flashed once)
 *   application  0x08002000 .. 0x08007EFF   (~23.75 KB, flashed over USB)
 *   metadata     0x08007F00 .. 0x08007FFF   ({magic, app_len, app_crc32})
 *
 * BOOT DECISION (every reset):
 *   handoff magic set -> flashing mode | else app metadata+CRC valid -> run app
 *   | else -> flashing mode (brick-safe).
 *
 * FLASHING PROTOCOL over CDC (host streams data; host waits for each reply):
 *   0x01 ERASE                    -> erase app region; reply [state,err]
 *   0x02 WRITE  n  <n bytes>      -> append n app bytes (programmed in 256 B
 *                                    pages as they fill); reply [state,err]
 *   0x03 READ_STATUS             -> reply [state,err]
 *   0x04 VERIFY  crc32(4, LE)    -> flush, CRC the written app, write metadata
 *                                    if it matches; reply [state,err]
 *   0x05 BOOT                    -> jump to the application (no reply)
 *   0xB2 GET_DIAGNOSTIC          -> reply [branch, rstsckr0..3(LE)] (see below)
 *   state: 0 IDLE 1 BUSY 2 READY 3 ERROR ; err: 0 ok, 5 CRC mismatch, 6 region
 *
 * STATUS LED: PB8 (BOOT0), ACTIVE-HIGH (per noknok decision; pin+polarity are
 *   per-module/per-MCU, "off in the resting state" is the invariant). No LED is
 *   fitted on the current board; PB8 is a plain GPIO after boot (safe).
 *
 * BOOT-DECISION DIAGNOSTIC (0xB2, added Jul 2026 - see the DEV-12 "why did the
 * module land back in the bootloader" investigation): main()'s boot decision has
 * two independent ways to end up in flashing mode -
 *   branch A: HANDOFF_CELL held the app's legitimate 0xB0 magic
 *   branch B: app_is_valid() returned false (bad/missing metadata or CRC)
 * Both are indistinguishable from the OUTSIDE (module just "is in the
 * bootloader"), which made a real question - is a module reverting because of a
 * genuine reset it can't avoid (branch A, stale SRAM read as the magic by pure
 * chance) or because its own CRC scan is glitching on marginal power on some
 * rigs (branch B) - impossible to answer without instrumentation. DIAG_CELL (a
 * SEPARATE word from the handoff cell, so it never interferes with the handoff
 * protocol) latches which branch fired PLUS the raw RCC->RSTSCKR reset-cause
 * bits (PORRSTF/PINRSTF/SFTRSTF/IWDGRSTF/WWDGRSTF/LPWRRSTF), captured as the
 * very first thing main() does (we never clear RMVF, so the register itself is
 * left exactly as the reset left it). Read any time the module is sitting in
 * the bootloader via the new 0xB2 command - no flashing required, purely
 * observational.
 *
 * Reuses the LED module's proven USBD (FSDEV) CDC stack + 48 MHz HSE bring-up.
 */

#include "ch32fun.h"
#include "usbd.h"
#include <string.h>

/* -D overrides are for test images (e.g. a 2.0.1 to prove a self-update). */
#define BL_PROTOCOL_VERSION 0x01
#ifndef BL_VERSION_MAJOR
#define BL_VERSION_MAJOR  2     /* 2.0.0: stage-1 of layout 4 (DEV-237). 1.1.0: DEV-12 retention fix */
#endif
#ifndef BL_VERSION_MINOR
#define BL_VERSION_MINOR  0
#endif
#ifndef BL_VERSION_PATCH
#define BL_VERSION_PATCH  0
#endif
#define BL_LAYOUT         4u    /* V203 layout ids are unique across chip families (I2C: 2 V003, 3 X035) */

/* ---- flash map (REAL addresses for erase/program/CRC) ---- */
#define STAGE1_FLASH_BASE 0x08000400u
#define APP_FLASH_BASE   0x08002000u
#define CTRL_FLASH_ADDR  0x08007E00u            /* stage-1 update control block (layout 4) */
#define META_FLASH_ADDR  0x08007F00u
#define APP_REGION_LEN   (CTRL_FLASH_ADDR - APP_FLASH_BASE)   /* 0x5E00 = 24064 B */
#define STAGE1_REGION_LEN (APP_FLASH_BASE - STAGE1_FLASH_BASE) /* 0x1C00 = 7 KB */

/* ---- stage-1 image header: 20 B at base + 0x100 (bootloader.ld places it) ----
 * Same idea and field order as the I2C stage-1 (DEV-31 hardening E, DEV-124
 * target word): VERIFY_STAGE1 refuses a staged image unless it finds this. */
#define HDR_OFFSET       0x100u
#define HDR_MAGIC        0x31534B4Eu            /* "NKS1" */
#define HDR_TARGET       0x33303256u            /* "V203" */
typedef struct { uint32_t magic, base, layout, version, target; } stage1_hdr_t;
__attribute__((section(".stage1_hdr"), used))
const stage1_hdr_t stage1_hdr = {
    HDR_MAGIC, 0x00000400u, BL_LAYOUT,
    ((uint32_t)BL_PROTOCOL_VERSION << 24) | ((uint32_t)BL_VERSION_MAJOR << 16) |
    ((uint32_t)BL_VERSION_MINOR << 8) | (uint32_t)BL_VERSION_PATCH,
    HDR_TARGET,
};

/* ---- control block (read by the frozen stage-0; field order is a contract) ---- */
#define CTRL_MAGIC       0x6E6B5530u            /* "nkU0" - update pending */
struct ctrl { uint32_t magic, staging_addr, stage1_len, stage1_crc32, app_base, desc_crc32; };
#define APP_JUMP_ADDR    0x00002000u            /* alias; app is linked here */
#define META_MAGIC       0x6E6B5542u            /* 'nkUB' - written only after a verified flash */

struct app_meta { uint32_t magic; uint32_t app_len; uint32_t app_crc32; };

/* ---- handoff cell: top 16 B of RAM, protected by bootloader.ld + app.ld ---- */
#define HANDOFF_CELL    (*(volatile uint32_t *)0x200027F0u)
#define ENTER_BL_MAGIC  0x6E6B4F54u             /* 'nkOT' - app writes this then resets */

/* ---- boot-decision diagnostic: a SEPARATE word in the same reserved 16 B,
 * @0x200027F4 (HANDOFF_CELL is @0x200027F0, 4 B) - never touched by the
 * handoff protocol, so it can't corrupt or be corrupted by it. No-init RAM,
 * so it (like the handoff cell) survives a WARM reset but not a cold power-up
 * - that is itself diagnostic: an unexpected/garbage value on 0xB2 after a
 * power cycle is consistent with "SRAM wasn't retained", which is exactly the
 * kind of fact branch-A-vs-B needs.
 *
 * Layout: bits [31:24] and [1:0] are the REAL RCC->RSTSCKR reset-cause bits
 * (RMVF/PINRSTF/PORRSTF/SFTRSTF/IWDGRSTF/WWDGRSTF/LPWRRSTF live in [31:24];
 * LSION/LSIRDY live in [1:0]) captured verbatim, unmodified, as the very first
 * thing main() does (we deliberately do NOT clear RMVF, so we never disturb
 * the register - see the main() comment). Bits [9:8] are ours - unused by the
 * real register - and hold which branch fired last (0 = none captured yet/
 * unreadable, 1 = branch A magic, 2 = branch B invalid app). This packing
 * means DIAG_CELL is "RSTSCKR with the branch stamped into spare bits", so
 * nothing is lost and nothing real is overwritten. */
#define DIAG_CELL            (*(volatile uint32_t *)0x200027F4u)
#define DIAG_BRANCH_SHIFT    8u
#define DIAG_BRANCH_MASK     (0x3u << DIAG_BRANCH_SHIFT)
#define DIAG_BRANCH_MAGIC    (1u << DIAG_BRANCH_SHIFT)
#define DIAG_BRANCH_INVALID  (2u << DIAG_BRANCH_SHIFT)
#define CMD_GET_DIAGNOSTIC   0xB2u

/* ---- status LED on PB8 (active-high) ---- */
#define STATUS_LED_PIN  8

/* ---- command bytes ---- */
#define CMD_ERASE        0x01
#define CMD_WRITE        0x02
#define CMD_READ_STATUS  0x03
#define CMD_VERIFY       0x04
#define CMD_BOOT         0x05
#define CMD_VERIFY_STAGE1 0x06   /* + crc32(4, LE): arm a stage-1 self-update (2.0.0) */
#define CMD_GET_VERSION  0xB1    /* -> [proto, major, minor, patch, layout] (2.0.0)   */

/* ---- state ---- */
enum { BL_IDLE = 0, BL_BUSY = 1, BL_READY = 2, BL_ERROR = 3 };
/* 4/6/8 as in the I2C stage-1: 4 length invalid, 6 region / flash / BOOT with
 * no valid app, 8 not a V203 layout-4 stage-1 image. */
enum { ERR_NONE = 0, ERR_LEN = 4, ERR_CRC = 5, ERR_REGION = 6, ERR_NOT_STAGE1 = 8 };
static uint8_t bl_state = BL_IDLE;
static uint8_t bl_error = ERR_NONE;

/* ---- flashing working set ----
 * The app streams in over USB in <=32-byte chunks, but the V20x fast programmer
 * burns a whole 256-byte page at once. So bytes accumulate into page_buf and a
 * full page is flushed to flash when it fills (the final partial page is
 * 0xFF-padded and flushed at VERIFY). page_buf is u32-aligned for the word-wise
 * page program. */
static uint32_t write_addr;                  /* flash addr of the page currently filling */
static uint32_t total_written;               /* total app bytes received */
static uint32_t page_fill;                   /* bytes currently in page_buf (0..256) */
static uint8_t  page_buf[256] __attribute__((aligned(4)));

/* ============================================================
 * Unique USB serial from the chip UID (same as the app, so a module keeps the
 * same serial across app and bootloader - host can match them).
 * ============================================================ */
uint8_t noknok_serial[2 + 24 * 2];
static void build_serial(void) {
    const volatile uint8_t *uid = (const volatile uint8_t *)0x1FFFF7E8;
    static const char H[] = "0123456789ABCDEF";
    noknok_serial[0] = sizeof(noknok_serial);
    noknok_serial[1] = 0x03;
    for (int i = 0; i < 12; i++) {
        uint8_t b = uid[i];
        uint8_t hi = H[b >> 4], lo = H[b & 0x0F];
        noknok_serial[2 + (i * 2) * 2]     = hi;  noknok_serial[2 + (i * 2) * 2 + 1]     = 0x00;
        noknok_serial[2 + (i * 2 + 1) * 2] = lo;  noknok_serial[2 + (i * 2 + 1) * 2 + 1] = 0x00;
    }
}

/* ============================================================
 * Clock: HSI boot -> HSE 24 MHz x2 = 48 MHz (verbatim from the LED firmware).
 * ============================================================ */
static int clock_to_hse_48(void) {
    RCC->CTLR |= RCC_HSEON;
    volatile uint32_t t = 1500000;
    while (t--) { if (RCC->CTLR & RCC_HSERDY) break; }
    if (!(RCC->CTLR & RCC_HSERDY)) return 0;
    FLASH->ACTLR = (FLASH->ACTLR & ~0x07u) | 0x01u;
    RCC->CFGR0 = RCC_HPRE_DIV1 | RCC_PPRE1_DIV1 | RCC_PPRE2_DIV1 | RCC_PLLSRC | RCC_PLLMULL2;
    RCC->CTLR |= RCC_PLLON;
    t = 1500000;
    while (t--) { if (RCC->CTLR & RCC_PLLRDY) break; }
    if (!(RCC->CTLR & RCC_PLLRDY)) return 0;
    RCC->CFGR0 = (RCC->CFGR0 & ~(uint32_t)0x03) | RCC_SW_PLL;
    t = 1500000;
    while (t--) { if ((RCC->CFGR0 & RCC_SWS) == 0x08) break; }
    return ((RCC->CFGR0 & RCC_SWS) == 0x08);
}

/* ============================================================
 * CRC32 (zlib poly) - matches the I2C bootloader and Python binascii.crc32.
 * ============================================================ */
static uint32_t crc32_calc(const uint8_t *p, uint32_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

/* ============================================================
 * Status LED (PB8, active-high)
 * ============================================================ */
static void led_init(void) {
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOB;
    GPIOB->CFGHR &= ~(0xFu << ((STATUS_LED_PIN - 8) * 4));
    GPIOB->CFGHR |=  (0x1u << ((STATUS_LED_PIN - 8) * 4));   /* output PP, 10 MHz */
}
static inline void led_set(int on) {
    GPIOB->BSHR = on ? (1u << STATUS_LED_PIN) : (1u << (STATUS_LED_PIN + 16));
}

/* ============================================================
 * Flash programming (CH32V20x FAST 256-byte page program/erase).
 *
 * WHY THE 256-BYTE PAGE PATH (not the legacy halfword FLASH_CTLR_PG):
 *   DEV-12 (Jul 2026): OTA-flashed apps VERIFY correct at write time (do_verify's
 *   crc32 passes) and the app runs immediately after BOOT - but on the NEXT power
 *   cycle the boot-time app_is_valid() CRC scan FAILS and the module drops to the
 *   bootloader. Reproduced entirely on clean PC 5V (no supply/brownout involved).
 *   Decisive test: byte-identical app content flashed by WCHISPTool survives power
 *   cycles; the SAME bytes flashed by this bootloader's OLD halfword path did not.
 *   => a flash RETENTION defect: the old single-halfword program (FLASH_CTLR_PG +
 *   16-bit write + BSY-wait only, no EOP/error check) UNDER-COMMITTED the cells -
 *   they read back correct while fresh but lose state across a power-off.
 *   FIX: the WCH-CHARACTERISED V20x path - and what the vendor flash library
 *   (ch32fun extralibs/ch20x_30x_flash.h) and WCHISPTool use - is the 256-byte
 *   FAST PAGE program: MODEKEYR-unlock the fast mode, load a full 256 B page into
 *   the program buffer (WRBSY poll per word), then PG_STRT to burn it in one
 *   managed, fully-committed pulse, then check EOP/WRPRTERR. This rewrite matches
 *   that reference faithfully AND adds a per-page read-back verify + retry
 *   (flash_program_page) so a page that programs wrong is caught, not advanced
 *   over. Goal: OTA-programmed cells with WCHISPTool-equivalent retention.
 *
 * The bootloader only ever touches the app + metadata regions, never its own.
 * All addresses here are 256-byte aligned (APP_FLASH_BASE=0x2000, META=0x7F00).
 * ============================================================ */
#define FLASH_PAGE_LEN 256u
#define WRBSY          0x02u    /* FLASH->STATR word-buffer busy (per WCH SDK/ch32fun) */

static int flash_ok = 1;        /* cleared on any WRPRTERR; do_verify surfaces it */

static void flash_wait(void) {           /* bounded BSY wait -> a stall becomes a recoverable error */
    volatile uint32_t t = 2000000;
    while ((FLASH->STATR & FLASH_STATR_BSY) && t) t--;
}

/* Busy-poll to completion, capture + clear the status flags (mirrors the WCH
 * reference _ch20x_30x_flash_busy_poll). Sets flash_ok=0 on a protection error. */
static void flash_finish(void) {
    flash_wait();
    if (FLASH->STATR & FLASH_STATR_WRPRTERR) flash_ok = 0;
    FLASH->STATR = FLASH_STATR_EOP | FLASH_STATR_WRPRTERR;   /* clear (write-1-to-clear) */
}

static void flash_unlock(void) {
    RCC->AHBPCENR |= RCC_AHBPeriph_SRAM | RCC_FLITFEN;   /* restore FLITF (USBDSetup cleared it) */
    FLASH->KEYR    = FLASH_KEY1; FLASH->KEYR    = FLASH_KEY2;  /* unlock erase/program */
    FLASH->MODEKEYR = FLASH_KEY1; FLASH->MODEKEYR = FLASH_KEY2; /* unlock FAST (page) mode */
}

/* Fast 256-byte page erase (CR_PAGE_ER + PG_STRT), per the WCH reference. */
static void flash_erase_page(uint32_t addr) {
    FLASH->CTLR = CR_PAGE_ER;
    FLASH->ADDR = addr;
    FLASH->CTLR = CR_STRT_Set | CR_PAGE_ER;
    flash_finish();
    FLASH->CTLR = 0;
}

/* One fast 256-byte page-program pulse: load the whole page into the buffer as
 * 32-bit words (WRBSY poll between words), then PG_STRT to burn it. `data` = 64
 * u32 words. Faithful to the WCH reference _ch20x_30x_flash_page_write. */
static void flash_program_page_once(uint32_t addr, const uint32_t *data) {
    volatile uint32_t *dst = (volatile uint32_t *)addr;
    FLASH->CTLR = CR_PAGE_PG;                       /* enter fast page-program */
    for (uint32_t i = 0; i < FLASH_PAGE_LEN / 4; i++) {
        dst[i] = data[i];
        while (FLASH->STATR & WRBSY);               /* wait word into buffer */
    }
    FLASH->CTLR = CR_PAGE_PG | CR_PG_STRT;          /* buffer -> flash */
    flash_finish();
    FLASH->CTLR = 0;                                /* clear FTPG */
}

/* Program a page WITH read-back verify + retry. This is the belt-and-suspenders
 * for the DEV-12 flash-RETENTION bug: the failure mode was cells that read back
 * correct right after programming but did NOT retain across a power cycle
 * (under-committed). A single post-program read-back verify here doesn't by
 * itself prove retention (the cell is still fresh), BUT the fast-page program
 * pulse above is the WCH-characterised full-commit method, and a per-page
 * verify+retry catches any page the controller reports/leaves wrong so we never
 * advance over a bad page. On a persistent mismatch we set flash_ok=0 -> the
 * whole flash is rejected (ERR_REGION), so a defective program can never be
 * marked valid. `data` must point at a 256-byte, 4-aligned source buffer. */
#define FLASH_PROG_RETRIES 3
static void flash_program_page(uint32_t addr, const uint32_t *data) {
    const uint32_t *src = data;
    for (int attempt = 0; attempt <= FLASH_PROG_RETRIES; attempt++) {
        flash_program_page_once(addr, src);
        /* read-back verify the whole page */
        const volatile uint32_t *v = (const volatile uint32_t *)addr;
        int ok = 1;
        for (uint32_t i = 0; i < FLASH_PAGE_LEN / 4; i++) {
            if (v[i] != src[i]) { ok = 0; break; }
        }
        if (ok) return;                              /* programmed + reads back correct */
        if (attempt < FLASH_PROG_RETRIES) {
            flash_erase_page(addr);                  /* clear + retry the pulse */
        }
    }
    flash_ok = 0;                                    /* gave up -> reject the flash */
}

/* ============================================================
 * Command handlers
 * ============================================================ */
static void do_erase(void) {
    bl_state = BL_BUSY; bl_error = ERR_NONE;
    flash_ok = 1;
    flash_unlock();
    /* erase the whole app + metadata region (0x2000 .. 0x8000) in 256-byte pages */
    for (uint32_t a = APP_FLASH_BASE; a < 0x08008000u; a += FLASH_PAGE_LEN)
        flash_erase_page(a);
    write_addr    = APP_FLASH_BASE;
    total_written = 0;
    page_fill     = 0;
    if (!flash_ok) { bl_state = BL_ERROR; bl_error = ERR_REGION; return; }
    bl_state = BL_IDLE;
}

/* Program the current 256-byte page buffer to flash and advance. Any unused tail
 * of the buffer must already be 0xFF-filled by the caller (matches an erased
 * cell, so it programs to no-change). */
static void flush_page(void) {
    flash_program_page(write_addr, (const uint32_t *)page_buf);
    write_addr += FLASH_PAGE_LEN;
    page_fill = 0;
}

static void write_byte(uint8_t b) {
    if (total_written >= APP_REGION_LEN) { bl_state = BL_ERROR; bl_error = ERR_REGION; return; }
    page_buf[page_fill++] = b;
    total_written++;
    if (page_fill == FLASH_PAGE_LEN) flush_page();     /* full page -> burn it */
}

/* Shared by VERIFY and VERIFY_STAGE1: flush the last partial page, then CRC
 * what was written to the app/staging region. 1 = ok. */
static int staged_ok(uint32_t expected_crc, uint32_t max_len) {
    /* flush the final partial page (0xFF-pad the unused tail so it programs to
     * erased/no-change) */
    if (page_fill) {
        for (uint32_t i = page_fill; i < FLASH_PAGE_LEN; i++) page_buf[i] = 0xFF;
        flush_page();
    }
    if (!flash_ok) { bl_state = BL_ERROR; bl_error = ERR_REGION; return 0; }
    if (total_written == 0 || total_written > max_len) { bl_state = BL_ERROR; bl_error = ERR_LEN; return 0; }

    uint32_t crc = crc32_calc((const uint8_t *)APP_FLASH_BASE, total_written);
    if (crc != expected_crc) { bl_state = BL_ERROR; bl_error = ERR_CRC; return 0; }
    return 1;
}

/* Arm a stage-1 self-update (2.0.0, DEV-237). The new stage-1 has been written
 * to the app region exactly like an app. Only if the CRC matches AND the image
 * carries a V203 layout-4 stage-1 header is the control block written; until
 * then nothing is pending and an interrupted transfer just leaves no app. */
static void do_verify_stage1(uint32_t expected_crc) {
    if (!staged_ok(expected_crc, STAGE1_REGION_LEN)) return;

    const volatile stage1_hdr_t *h = (const volatile stage1_hdr_t *)(APP_FLASH_BASE + HDR_OFFSET);
    if (total_written < HDR_OFFSET + sizeof(stage1_hdr_t) ||
        h->magic != HDR_MAGIC || h->base != 0x00000400u ||
        h->layout != BL_LAYOUT || h->target != HDR_TARGET) {
        bl_state = BL_ERROR; bl_error = ERR_NOT_STAGE1; return;
    }

    static uint32_t ctrl_page[FLASH_PAGE_LEN / 4];
    for (uint32_t i = 0; i < FLASH_PAGE_LEN / 4; i++) ctrl_page[i] = 0xFFFFFFFFu;
    ctrl_page[0] = CTRL_MAGIC;
    ctrl_page[1] = APP_FLASH_BASE;      /* staging_addr */
    ctrl_page[2] = total_written;       /* stage1_len   */
    ctrl_page[3] = expected_crc;
    ctrl_page[4] = APP_FLASH_BASE;      /* app_base - bounds the stage-1 region for stage-0 */
    ctrl_page[5] = crc32_calc((const uint8_t *)ctrl_page, 20);   /* descriptor CRC */
    flash_unlock();
    flash_erase_page(CTRL_FLASH_ADDR);  /* ERASE wiped it already; a repeated VERIFY_STAGE1 must not program over old data */
    flash_program_page(CTRL_FLASH_ADDR, ctrl_page);
    if (!flash_ok) { bl_state = BL_ERROR; bl_error = ERR_REGION; return; }
    bl_state = BL_READY; bl_error = ERR_NONE;
}

static int stage1_update_pending(void) {
    return *(const volatile uint32_t *)CTRL_FLASH_ADDR == CTRL_MAGIC;
}

static void do_verify(uint32_t expected_crc) {
    if (!staged_ok(expected_crc, APP_REGION_LEN)) return;
    uint32_t crc = expected_crc;

    /* write metadata (validity marker) into its own 256-byte page. META_FLASH_ADDR
     * (0x7F00) is page-aligned + already erased by do_erase; program a full page
     * with the struct at the front and 0xFF elsewhere. Only now is the app good. */
    static uint8_t meta_page[256] __attribute__((aligned(4)));
    for (uint32_t i = 0; i < FLASH_PAGE_LEN; i++) meta_page[i] = 0xFF;
    struct app_meta m = { META_MAGIC, total_written, crc };
    __builtin_memcpy(meta_page, &m, sizeof(m));
    flash_program_page(META_FLASH_ADDR, (const uint32_t *)meta_page);
    if (!flash_ok) { bl_state = BL_ERROR; bl_error = ERR_REGION; return; }

    bl_state = BL_READY; bl_error = ERR_NONE;
}

static int app_is_valid(void) {
    const struct app_meta *m = (const struct app_meta *)META_FLASH_ADDR;
    if (m->magic != META_MAGIC) return 0;
    if (m->app_len == 0 || m->app_len > APP_REGION_LEN) return 0;
    return crc32_calc((const uint8_t *)APP_FLASH_BASE, m->app_len) == m->app_crc32;
}

static void jump_to_app(void) {
    EXTEN->EXTEN_CTR &= ~EXTEN_USBD_PU_EN;   /* drop USB pull-up so host sees the bootloader detach */
    Delay_Ms(30);
    __asm volatile(
        "li t0, 0x00002000 \n"
        "jr t0 \n"
    );
    while (1) { }
}

/* ============================================================
 * CDC command parser (commands may span multiple poll_input dispatches)
 * ============================================================ */
static void send_status(void) {
    uint8_t r[2] = { bl_state, bl_error };
    USBD_SendEndpoint(3, r, 2);
}

/* 0xB2 GET_DIAGNOSTIC reply: 5 bytes, [branch, rstsckr0..3(LE)].
 * branch: 0 = no flashing-mode entry captured yet this power-up (DIAG_CELL is
 *   whatever no-init SRAM happened to hold, or the value we might reasonably
 *   never touch on a normal app boot - see the "only stamp on entry" note in
 *   main()), 1 = branch A (handoff magic), 2 = branch B (app_is_valid failed).
 * rstsckr0..3: the RAW RCC->RSTSCKR value captured at the top of main() on
 *   THAT flashing-mode entry, with our own branch bits masked back OUT (the
 *   host gets the real hardware register value, unmodified) - decode with
 *   RCC_PORRSTF/PINRSTF/SFTRSTF/IWDGRSTF/WWDGRSTF/LPWRRSTF (bits 24-31). */
static void send_diagnostic(void) {
    uint32_t d = DIAG_CELL;
    uint8_t branch = (uint8_t)((d & DIAG_BRANCH_MASK) >> DIAG_BRANCH_SHIFT);
    uint32_t rstsckr = d & ~DIAG_BRANCH_MASK;
    uint8_t r[5] = {
        branch,
        (uint8_t)(rstsckr & 0xFF), (uint8_t)((rstsckr >> 8) & 0xFF),
        (uint8_t)((rstsckr >> 16) & 0xFF), (uint8_t)((rstsckr >> 24) & 0xFF)
    };
    USBD_SendEndpoint(3, r, 5);
}

/* 0xB1 GET_VERSION reply: [proto, major, minor, patch, layout] (2.0.0), the same
 * 5 bytes as the I2C stage-1 - the brain reads the layout from byte 5. */
static void send_version(void) {
    uint8_t r[5] = { BL_PROTOCOL_VERSION, BL_VERSION_MAJOR, BL_VERSION_MINOR,
                     BL_VERSION_PATCH, (uint8_t)BL_LAYOUT };
    USBD_SendEndpoint(3, r, 5);
}

/* BOOT (2.0.0): a pending stage-1 update outranks the app (the app region holds
 * the staged image) -> reset, stage-0 installs it. Otherwise only a VALID app
 * is started; with none the module stays here with error 6 (1.x jumped blind). */
static void do_boot(void) {
    if (stage1_update_pending()) {
        EXTEN->EXTEN_CTR &= ~EXTEN_USBD_PU_EN;   /* host sees the detach */
        Delay_Ms(30);
        NVIC_SystemReset();
    }
    if (app_is_valid()) jump_to_app();
    bl_state = BL_ERROR; bl_error = ERR_REGION;
}

static enum { P_CMD, P_WRITE_N, P_WRITE_DATA, P_VERIFY, P_VERIFY_S1 } pstate = P_CMD;
static uint8_t wr_remaining = 0;
static uint8_t vbuf[4];
static uint8_t vidx = 0;

static void process_byte(uint8_t b) {
    switch (pstate) {
        case P_CMD:
            switch (b) {
                case CMD_ERASE:          do_erase();   send_status(); break;
                case CMD_READ_STATUS:    send_status();               break;
                case CMD_WRITE:          pstate = P_WRITE_N;          break;
                case CMD_VERIFY:         vidx = 0; pstate = P_VERIFY;  break;
                case CMD_BOOT:           do_boot();                   break;  /* no return unless no valid app */
                case CMD_VERIFY_STAGE1:  vidx = 0; pstate = P_VERIFY_S1; break;
                case CMD_GET_VERSION:    send_version();              break;
                case CMD_GET_DIAGNOSTIC: send_diagnostic();           break;
                default: break;
            }
            break;
        case P_WRITE_N:
            wr_remaining = b;
            if (wr_remaining == 0) { send_status(); pstate = P_CMD; }
            else                   { pstate = P_WRITE_DATA; }
            break;
        case P_WRITE_DATA:
            write_byte(b);
            if (--wr_remaining == 0) { send_status(); pstate = P_CMD; }
            break;
        case P_VERIFY:
        case P_VERIFY_S1:
            vbuf[vidx++] = b;
            if (vidx == 4) {
                uint32_t crc = (uint32_t)vbuf[0] | ((uint32_t)vbuf[1] << 8)
                             | ((uint32_t)vbuf[2] << 16) | ((uint32_t)vbuf[3] << 24);
                if (pstate == P_VERIFY_S1) do_verify_stage1(crc);
                else                       do_verify(crc);
                send_status();
                pstate = P_CMD;
            }
            break;
    }
}

/* RX (host->device, EP2) is delivered here by poll_input(). */
void handle_usbd_input(int numbytes, uint8_t *data) {
    for (int i = 0; i < numbytes; i++) process_byte(data[i]);
}

/* ch32fun's USB-printf backend calls this symbol; route it to USBD. */
int USBFS_SendEndpointNEW(int endp, uint8_t *data, int len, int copy) {
    (void)copy;
    return USBD_SendEndpoint(endp, data, len);
}

/* ============================================================
 * Main
 * ============================================================ */
int main(void) {
    /* Capture the reset cause FIRST, before anything (RMVF clear, RCC changes,
     * even the boot-decision reads themselves) can disturb RCC->RSTSCKR. Do
     * NOT clear RMVF here - leave the register exactly as the reset left it,
     * in case something else ever wants to read it too; DIAG_CELL is our own
     * durable copy. See the DIAG_CELL comment above for the bit layout. */
    uint32_t rstsckr_raw = RCC->RSTSCKR;

    /* POWER-SETTLE DELAY (~50 ms) before the boot-time CRC scan. NOTE on the
     * rationale (DEV-12, corrected Jul 2026): the ACTUAL root cause of the DEV-12
     * failure was NOT a brownout - it was read-marginal flash left by the OLD
     * halfword programmer (now fixed by the fast-page program path below), which
     * failed the CRC scan on a marginal supply that a WCHISPTool-programmed image
     * survived. So this delay is NOT the primary fix. It is kept only as cheap,
     * harmless defence-in-depth: it lets the rail finish charging through the
     * module's input polyfuse + 1N4007 + bulk caps before the ~6 KB flash CRC
     * read, which can only help any residual marginal-supply-at-boot effect and
     * costs ~50 ms on a cold boot (imperceptible; it gates every power-on incl. a
     * healthy app launch, hence ~50 ms not seconds). Raw HSI busy-loop: needs no
     * peripheral/clock/SysTick, valid however the chip reset. ~100k iters. */
    for (volatile uint32_t s = 0; s < 100000u; s++) { __asm volatile("" ::: "memory"); }

    /* Boot decision at the reset-default HSI clock (no USB needed yet). */
    int enter_flash = 0;
    uint32_t branch = 0;
    if (HANDOFF_CELL == ENTER_BL_MAGIC) {
        HANDOFF_CELL = 0; enter_flash = 1; branch = DIAG_BRANCH_MAGIC;
    } else if (!app_is_valid()) {
        enter_flash = 1; branch = DIAG_BRANCH_INVALID;
    }

    if (enter_flash) {
        /* Only stamp the diagnostic when we're actually entering flashing
         * mode - a normal app boot leaves DIAG_CELL untouched from whatever
         * the LAST flashing-mode entry recorded, which is fine: it's only
         * ever read via 0xB2, which only works while sitting in the
         * bootloader in the first place. */
        DIAG_CELL = (rstsckr_raw & ~DIAG_BRANCH_MASK) | branch;
    }

    if (!enter_flash) jump_to_app();

    /* ---- flashing mode ---- */
    clock_to_hse_48();
    build_serial();
    USBDSetup();
    led_init();
    bl_state = BL_IDLE;

    uint32_t tick = 0;
    while (1) {
        poll_input();
        /* status LED: solid on error, slow pulse otherwise (cosmetic; no LED on board yet) */
        tick++;
        led_set(bl_error ? 1 : (int)((tick >> 16) & 1));
    }
}
