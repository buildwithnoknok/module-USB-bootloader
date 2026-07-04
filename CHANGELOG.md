# Changelog — noknok USB Module Bootloader

All notable changes to the **noknok USB module bootloader** (CH32V203) are recorded
here. This versioning is **independent of the module application firmware** (e.g.
`module-usb-led`) — it tracks the bootloader binary only. The version is set by
`BL_VERSION_{MAJOR,MINOR,PATCH}` in `firmware/src/noknok_usb_bootloader.c`.

> **The bootloader is not updated over the air.** It is flashed **once per board**
> via BOOT0 + WCHISPTool; OTA writes only the application region (`0x08002000`+),
> never the bootloader's own region (`0x08000000`). A bootloader change therefore
> requires a physical re-flash of every affected module.

The format is loosely based on [Keep a Changelog](https://keepachangelog.com/).

## [1.1.0] — 2026-07-04

### Fixed
- **CRITICAL — OTA-flashed applications did not survive a power cycle.** The module
  ran correctly immediately after an OTA (the `BOOT` command jumps straight to the
  app), but on the *next* power-on the boot-time CRC check failed and the module
  dropped back into flashing mode ("stuck in the bootloader"). Root cause:
  `flash_unlock()` unlocked `KEYR` but **not `MODEKEYR`**, so the CH32V20x fast
  page-program mode was gated off and the fallback half-word write **under-committed**
  the flash cells — they read back correctly while fresh (so `VERIFY` passed) but lost
  charge across a power-off. Supply-independent (reproduced on clean PC power). Fixed
  by switching to the WCH-characterized **256-byte fast page program** (the method
  WCHISPTool itself uses), with `EOP`/`WRPRTERR` error checks and per-page read-back
  verify + retry (×3). Commit `7908e59`.

### Added
- **`0xB2 GET_DIAGNOSTIC`** — reports the boot-decision branch (A = `0xB0` warm
  handoff, B = `app_is_valid()` failed) plus `RCC->RSTSCKR` (reset-cause register), so
  a bootloader-mode entry can be diagnosed from the host. Read with
  `tools/usb_get_diagnostic.ps1`. Note: `PORRSTF` is set on **every** power-on and does
  not by itself indicate a brownout.
- **~50 ms power-settle delay** at the top of `main()` before the boot-time CRC scan —
  defence-in-depth against a marginal supply ramp. (This is *not* the fix for the
  retention bug above, which was supply-independent.)

### Tooling
- `tools/combine_image.ps1` — bake bootloader + app + valid metadata into one
  WCHISPTool image for single-pass blank-board bring-up.
- `tools/erase_image.ps1` — all-`0xFF` full-chip image to guarantee a blank chip.
- `tools/usb_get_version.ps1` — read the app's `GET_VERSION` (`0xB1`).
- `tools/usb_get_diagnostic.ps1` — read the `0xB2` diagnostic.

## [1.0.0] — 2026-06

### Added
- Initial noknok USB-CDC OTA bootloader for CH32V203. Flash map: 8 KB bootloader
  @ `0x08000000`, application @ `0x08002000`, 256 B metadata @ `0x08007F00`. CDC
  flashing protocol: `ERASE` / `WRITE` / `READ_STATUS` / `VERIFY(CRC32)` / `BOOT`.
  Warm-reset entry from a running app via the `0xB0` handoff-magic-in-RAM pattern.
  Brick-safe: the metadata validity marker is written only after a verified flash.
  Unique chip-UID serial shared between app and bootloader so a host can match a
  module across the PID change (app `0x4E4E` ↔ bootloader `0x4E42`).

---

**Note:** `BL_VERSION_*` is currently *documentary* — it is not exposed over any
command, so the host cannot yet read the installed bootloader version. A future
enhancement could surface it (e.g. extend `0xB2`). The `1.1.0` source define lands
with this changelog; the committed `firmware/bin/noknok_usb_bootloader.bin` from
`7908e59` is functionally identical (the define is unread) and will carry the bumped
value on its next rebuild.
