# Changelog — noknok USB Module Bootloader

All notable changes to the **noknok USB module bootloader** (CH32V203) are recorded
here. This versioning is **independent of the module application firmware** (e.g.
`module-usb-led`) — it tracks the bootloader binary only. The version is set by
`BL_VERSION_{MAJOR,MINOR,PATCH}` in `firmware/src/noknok_usb_bootloader.c`.

> **Up to 1.1.0 the bootloader is not updated over the air.** It is flashed **once per
> board** via BOOT0 + WCHISPTool; OTA writes only the application region (`0x08002000`+).
> **From 2.0.0 (flash layout 4)** the bootloader is split into a frozen stage-0 and a
> stage-1 that updates itself over USB; getting stage-0 onto a module still takes one
> full-image flash (BOOT0 + WCHISPTool, or SWD).

The format is loosely based on [Keep a Changelog](https://keepachangelog.com/).

## [2.0.0] — 2026-11-26 — release candidate (branch `dev-237-v203-stage0-stage1`, released when merged to `main`)

Release files: `firmware/bin/noknok_usb_stage0.bin` (712 B, crc32 `539c3cf9`),
`firmware/bin/noknok_usb_stage1.bin` (5568 B, crc32 `e5fbbc0a`), stage-1 index
`firmware/index_v203.json` (layout 4). Built with the bench toolchain (gcc 14.2.0), which
rebuilds 1.1.0 byte-identical.

Bench 26 Nov 2026 (Pi4 host, LEDs 16x on a Pi USB port + WCH-LinkE on SWD, `tools/bench/dev237_bench.py`,
18/18 PASS): layout-4 full image over SWD boots the app; `0xB1` = `(1,2,0,0,4)`; app OTA through stage-1 2.0.0;
the OTA-written app survives 10/10 power cuts; stage-1 self-update 2.0.0 -> 2.0.1 -> 2.0.0 with the app pushed
after each; power cut mid-WRITE (old stage-1, flash mode), after VERIFY_STAGE1 before BOOT (stage-0 installs the
new stage-1 at power-on), after BOOT (new stage-1) and with the bootloader idle (unchanged); every such cut read
PORRSTF in `0xB2`. VERIFY_STAGE1 refuses a V003 stage-1, an X035 stage-1 and the 1.1.0 bootloader (error 8) and
an app image (error 4, larger than the stage-1 region); nothing is armed, BOOT then stays in the bootloader.
BOOT without a valid app stays in the bootloader with error 6. `usb_flash.py` now asks `0xB1` up to 3 times (one
empty first read seen right after the bootloader enumerated).

### Changed (breaking: new flash layout 4, one full-image re-flash per module)
- **DEV-237 — the bootloader is now field-updatable.** Split like the I2C bootloader:
  a new frozen **stage-0** (`firmware/stage0/`, 712 B of 1 KB, no USB) at `0x0000`, and
  this CDC bootloader relinked as **stage-1** at `0x0400` (7 KB region, 5568 B). The app
  base stays `0x2000`, so the LED apps run unchanged; the app region shrinks by the new
  control-block page `0x7E00` (23.5 KB, apps today are 6-9 KB).
- New `0x06 VERIFY_STAGE1 crc32` (stage-1 self-update, header + chip guard: error 8 for an
  image that is not a V203 layout-4 stage-1), `0xB1 GET_VERSION` -> 5 bytes with layout 4.
- BOOT no longer jumps into an invalid app (stays, error 6); a pending stage-1 update makes
  BOOT reset so stage-0 installs it.
- Tools: `usb_flash.py --stage1`, new `mk_full_v203.py` (layout-4 full image + `--check`).

Bench 22 Nov 2026 (cellar rig, LEDs 16x on the WCH-LinkE, bench toolchain; main 1.1.0 rebuilds byte-identical;
release sizes stage-0 712 B, stage-1 2.0.0 5568 B): stage-0 SWD tests `firmware/stage0/test/run_s0_tests.sh` 6/6 PASS
(armed update installed, a copy cut half-way redone, bad descriptor / bad image CRC / bad geometry refused with the
stage-1 untouched, control block cleared after the install). **Found and fixed by that test:** on the CH32V203 a
fast page erase leaves the cells reading 0xE339E339, not 0xFF, so the first stage-0 failed its own blank-page verify
and halted; stage-0 now programs blank pages with 0xFF after the erase and treats 0xE339E339 as blank in its boot
check. Boot chain proven via the stage-1 diagnostic cell: layout-4 image with app -> app path; installed stage-1
without app -> flash mode. NOT yet tested: the USB side (0xB1, VERIFY_STAGE1 over CDC, app OTA through stage-1 2.0.0,
power cuts during a USB transfer) - the module's USB-C was not reachable from the Pi or the brain that day.

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
