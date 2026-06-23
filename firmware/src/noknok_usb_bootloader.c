/*
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
 *   state: 0 IDLE 1 BUSY 2 READY 3 ERROR ; err: 0 ok, 5 CRC mismatch, 6 region
 *
 * STATUS LED: PB8 (BOOT0), ACTIVE-HIGH (per noknok decision; pin+polarity are
 *   per-module/per-MCU, "off in the resting state" is the invariant). No LED is
 *   fitted on the current board; PB8 is a plain GPIO after boot (safe).
 *
 * Reuses the LED module's proven USBD (FSDEV) CDC stack + 48 MHz HSE bring-up.
 */

#include "ch32fun.h"
#include "usbd.h"
#include <string.h>

#define BL_VERSION_MAJOR  1
#define BL_VERSION_MINOR  0
#define BL_VERSION_PATCH  0

/* ---- flash map (REAL addresses for erase/program/CRC) ---- */
#define APP_FLASH_BASE   0x08002000u
#define META_FLASH_ADDR  0x08007F00u
#define APP_REGION_LEN   (META_FLASH_ADDR - APP_FLASH_BASE)   /* 0x5F00 = 24320 B */
#define APP_JUMP_ADDR    0x00002000u            /* alias; app is linked here */
#define META_MAGIC       0x6E6B5542u            /* 'nkUB' - written only after a verified flash */
#define FLASH_ERASE_PAGE 1024u                   /* CH32V20x standard page erase = 1 KB */

struct app_meta { uint32_t magic; uint32_t app_len; uint32_t app_crc32; };

/* ---- handoff cell: top 16 B of RAM, protected by bootloader.ld + app.ld ---- */
#define HANDOFF_CELL    (*(volatile uint32_t *)0x200027F0u)
#define ENTER_BL_MAGIC  0x6E6B4F54u             /* 'nkOT' - app writes this then resets */

/* ---- status LED on PB8 (active-high) ---- */
#define STATUS_LED_PIN  8

/* ---- command bytes ---- */
#define CMD_ERASE        0x01
#define CMD_WRITE        0x02
#define CMD_READ_STATUS  0x03
#define CMD_VERIFY       0x04
#define CMD_BOOT         0x05

/* ---- state ---- */
enum { BL_IDLE = 0, BL_BUSY = 1, BL_READY = 2, BL_ERROR = 3 };
enum { ERR_NONE = 0, ERR_CRC = 5, ERR_REGION = 6 };
static uint8_t bl_state = BL_IDLE;
static uint8_t bl_error = ERR_NONE;

/* ---- flashing working set ---- */
static uint32_t write_addr;     /* next real flash addr to program */
static uint32_t total_written;  /* total app bytes received */
static uint8_t  hw_lo;          /* pending low byte of the current 16-bit halfword */
static int      hw_have;        /* 1 if hw_lo holds a byte awaiting its pair */

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
 * Flash programming (CH32V20x fast pages; per ch32fun examples/flashtest)
 * The bootloader only ever touches the app + metadata regions, never its own.
 * ============================================================ */
static void flash_wait(void) {           /* bounded BSY wait -> a stall becomes a recoverable error */
    volatile uint32_t t = 2000000;
    while ((FLASH->STATR & FLASH_STATR_BSY) && t) t--;
}
static void flash_unlock(void) {
    RCC->AHBPCENR |= RCC_AHBPeriph_SRAM | RCC_FLITFEN;   /* restore FLITF (USBDSetup cleared it) */
    FLASH->KEYR = FLASH_KEY1; FLASH->KEYR = FLASH_KEY2;  /* unlock flash for erase/program */
}
/* Standard CH32V20x flash (per WCH SDK): 1 KB page erase + 16-bit halfword program.
 * The V20x fast-program buffer bits differ from the V003 family (0x40000/0x80000
 * are block-erase on V20x), so we use the simple, robust standard sequence. */
static void flash_erase_1k(uint32_t addr) {
    FLASH->CTLR |= FLASH_CTLR_PER;
    FLASH->ADDR  = addr;
    FLASH->CTLR |= CR_STRT_Set;
    flash_wait();
    FLASH->CTLR &= ~FLASH_CTLR_PER;
}
static void flash_program_hword(uint32_t addr, uint16_t val) {
    FLASH->CTLR |= FLASH_CTLR_PG;
    *(volatile uint16_t *)addr = val;
    flash_wait();
    FLASH->CTLR &= ~FLASH_CTLR_PG;
}

/* ============================================================
 * Command handlers
 * ============================================================ */
static void do_erase(void) {
    bl_state = BL_BUSY; bl_error = ERR_NONE;
    flash_unlock();
    /* erase the whole app + metadata region (0x2000 .. 0x8000) in 1 KB pages */
    for (uint32_t a = APP_FLASH_BASE; a < 0x08008000u; a += FLASH_ERASE_PAGE)
        flash_erase_1k(a);
    write_addr = APP_FLASH_BASE;
    total_written = 0;
    hw_have = 0;
    bl_state = BL_IDLE;
}

static void write_byte(uint8_t b) {
    if (total_written >= APP_REGION_LEN) { bl_state = BL_ERROR; bl_error = ERR_REGION; return; }
    if (!hw_have) { hw_lo = b; hw_have = 1; }
    else {
        flash_program_hword(write_addr, (uint16_t)hw_lo | ((uint16_t)b << 8));
        write_addr += 2;
        hw_have = 0;
    }
    total_written++;
}

static void do_verify(uint32_t expected_crc) {
    /* flush a dangling odd byte (pad the high byte with 0xFF) */
    if (hw_have) {
        flash_program_hword(write_addr, (uint16_t)hw_lo | 0xFF00u);
        write_addr += 2;
        hw_have = 0;
    }
    uint32_t crc = crc32_calc((const uint8_t *)APP_FLASH_BASE, total_written);
    if (crc != expected_crc) { bl_state = BL_ERROR; bl_error = ERR_CRC; return; }

    /* write metadata (validity marker) - meta page already erased by do_erase.
     * Only now is the app considered good. */
    struct app_meta m = { META_MAGIC, total_written, crc };
    const uint16_t *mp = (const uint16_t *)&m;
    for (uint32_t i = 0; i < sizeof(m) / 2; i++)
        flash_program_hword(META_FLASH_ADDR + i * 2, mp[i]);
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

static enum { P_CMD, P_WRITE_N, P_WRITE_DATA, P_VERIFY } pstate = P_CMD;
static uint8_t wr_remaining = 0;
static uint8_t vbuf[4];
static uint8_t vidx = 0;

static void process_byte(uint8_t b) {
    switch (pstate) {
        case P_CMD:
            switch (b) {
                case CMD_ERASE:       do_erase();   send_status(); break;
                case CMD_READ_STATUS: send_status();               break;
                case CMD_WRITE:       pstate = P_WRITE_N;          break;
                case CMD_VERIFY:      vidx = 0; pstate = P_VERIFY;  break;
                case CMD_BOOT:        jump_to_app();               break;  /* no return */
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
            vbuf[vidx++] = b;
            if (vidx == 4) {
                uint32_t crc = (uint32_t)vbuf[0] | ((uint32_t)vbuf[1] << 8)
                             | ((uint32_t)vbuf[2] << 16) | ((uint32_t)vbuf[3] << 24);
                do_verify(crc);
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
    /* Boot decision at the reset-default HSI clock (no USB needed yet). */
    int enter_flash = 0;
    if (HANDOFF_CELL == ENTER_BL_MAGIC) { HANDOFF_CELL = 0; enter_flash = 1; }
    else if (!app_is_valid())           { enter_flash = 1; }

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
