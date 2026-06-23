#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

// Boot on HSI no-PLL; main() manually switches to HSE x2 = 48 MHz (crystal).
// USBD (FSDEV) CDC. 48 MHz -> USBPRE=DIV1 -> 48 MHz USB clock.
#define FUNCONF_USE_HSI            1
#define FUNCONF_USE_HSE            0
#define FUNCONF_USE_PLL            0
#define FUNCONF_PLL_MULTIPLIER     2          // unused at boot, valid to compile
#define FUNCONF_SYSTEM_CORE_CLOCK  48000000   // actual clock after manual switch

#define FUNCONF_USE_USBPRINTF      1          // enables usbd.c RX path (poll_input/handle_usbd_input)
#define FUNCONF_USE_DEBUGPRINTF    0
#define FUNCONF_DEBUG_HARDFAULT    0          // avoid pulling in PrintHex
#define FUNCONF_SYSTICK_USE_HCLK   1
#define FUNCONF_ENABLE_HPE         1

#endif
