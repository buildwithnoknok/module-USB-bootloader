# noknok USB Module Bootloader (CH32V203)

USB-CDC **OTA bootloader** for the noknok USB modules — the USB counterpart of
[`module-I2C-bootloader`](https://github.com/buildwithnoknok/module-I2C-bootloader).
It re-flashes a module's application **over USB**, with no programmer and no SWD
cable, driven by the host (PC now; Pico / noknok app later). The application
update flow mirrors the I2C modules: `ERASE → WRITE → VERIFY(CRC32) → BOOT`.

## Why a custom bootloader (and not the WCH factory ROM bootloader)

The CH32V203 has a factory USB bootloader in ROM (`0x1FFF8000`, the one
WCHISPTool uses with the BOOT0 jumper). We tried hard to reach it **from running
firmware** so OTA could ride it for free — it is **not reachable**:

- **Boot-mode latch (`FLASH->STATR` bit 14 + `BOOT_MODEKEYR`)** — the method
  `minichlink -B` uses — is **silently rejected from firmware** (the register
  reads back `0` after the write; the latch is only honoured for a debug/POR
  reset, which firmware can't issue on itself).
- **Direct jump to `0x1FFF8000`** — the ROM bootloader runs and re-asserts USB,
  but **never enumerates** (failed device descriptor) regardless of clock/peripheral
  deinit. It needs a true power/pin reset. Confirmed via three jump variants.

So we own the bootloader. Entry uses the **magic-in-RAM + warm reset** pattern
(same as the I2C modules), which is proven to work on this chip.

## Flash map (32 KB; flash at `0x08000000`, also aliased at `0x00000000`)

| Region | Address | Notes |
|--------|---------|-------|
| Bootloader | `0x08000000` (8 KB) | this image; jumper-flashed **once** |
| Application | `0x08002000` (~23.75 KB) | flashed over USB |
| Metadata | `0x08007F00` (256 B) | `{magic, app_len, app_crc32}` — validity marker |

The app is linked at the `0x00002000` alias (`app.ld`); the bootloader programs
it to the real `0x08002000` and jumps to `0x00002000`. The top 16 B of RAM
(`0x200027F0`) are the no-init handoff cell, kept out of both linkers' RAM.

## Boot decision (every reset)

1. Handoff RAM magic set (app asked) → **flashing mode**.
2. Else app metadata valid **and** `CRC32(app) == meta.crc` → **jump to app**.
3. Else (no/invalid app) → **flashing mode** (brick-safe).

## Identity

The bootloader enumerates as a CDC device **VID `0x1209` / PID `0x4E42`** ("NB")
(the app is `0x4E4E` "NN"), product string `noknok USB bootloader`, with the same
unique chip-UID serial as the app — so the host can match a module across both.

## Flashing protocol (CDC; host waits for each `[state, err]` reply)

| Cmd | Bytes | Action |
|-----|-------|--------|
| `0x01` ERASE | `01` | erase the app + metadata region (1 KB pages) |
| `0x02` WRITE | `02 n <n bytes>` | append `n` app bytes (programmed as 16-bit halfwords) |
| `0x03` READ_STATUS | `03` | reply `[state, err]` |
| `0x04` VERIFY | `04 crc32(4, LE)` | CRC-check the written app; write metadata **only on match** |
| `0x05` BOOT | `05` | jump to the application (no reply) |

`state`: 0 IDLE, 1 BUSY, 2 READY, 3 ERROR. `err`: 0 ok, 5 CRC mismatch, 6 region overflow.

## Brick safety

1. **Interrupted flash** → metadata is the *last* thing written, so CRC fails →
   the bootloader stays in flashing mode → the host just retries (no cable).
2. **BOOT0 jumper + WCH factory ROM bootloader** = the unbrickable backstop to
   re-flash *this* bootloader via WCHISPTool.

## Status LED

PB8 (BOOT0), **active-high** (per the noknok decision: status-LED pin + polarity
are per-module/per-MCU; "off in the resting state" is the invariant). No LED is
fitted on the current LED-module board; PB8 is driven as a plain GPIO after boot
(safe). Active-high is required here because BOOT0 must read low at reset — an
active-low LED would bias it high and could select the factory bootloader.

## Building

On the RPi4 (ch32fun toolchain):

```
cd firmware/src
make noknok_usb_bootloader.bin     # uses bootloader.ld (8 KB region @ flash base)
```

`firmware/bin/noknok_usb_bootloader.bin` is the distributable image.

## Provisioning a blank board (once)

Flash the bootloader via the **BOOT0 jumper + WCHISPTool** to `0x08000000`.
After that, all application updates are OTA over USB — no SWD, no jumper.

## OTA-flashing an application (host)

```
powershell -ExecutionPolicy Bypass -File tools/usb_flash.ps1 -Bin path\to\app.bin
```

The flasher auto-detects the bootloader (PID `4E42`). If the module is running
the application (PID `4E4E`) it first sends `0xB0 ENTER_BOOTLOADER`, which makes
the app write the handoff magic and reset into the bootloader, then flashes.

## Building an application to run under this bootloader

Every USB-module app must (like the I2C relink rule):
1. Link at the `0x2000` offset (`app.ld`).
2. Reserve the top 16 B of RAM for the handoff cell (`app.ld`).
3. Implement `0xB0 ENTER_BOOTLOADER` = write `0x6E6B4F54` to `0x200027F0` then
   `NVIC_SystemReset()`.

See `module-usb-led` for the reference (`app.ld` + the `0xB0` handler).

## Recovery

- App won't boot / corrupt: power-cycle — the bootloader CRC-checks the app and
  drops to flashing mode if it's invalid; re-run `usb_flash.ps1`.
- Bootloader itself damaged: BOOT0 jumper + WCHISPTool → re-flash
  `noknok_usb_bootloader.bin`.

## CH32V203 flash gotcha (for maintainers)

V20x standard flash programming = **1 KB page erase (`FLASH_CTLR_PER`) + 16-bit
halfword program (`FLASH_CTLR_PG`)** — what this bootloader uses. Do **not** copy
the CH32V003 fast-program buffer bits: on the V20x, `0x40000`/`0x80000` are
**32K/64K block-erase** opcodes, not buffer reset/load — using them issues a
block erase that stalls and wipes large regions.

## License

Firmware: MIT. See the central
[License, Safety & Liability](https://buildwithnoknok.github.io/safety-and-license/) page.
