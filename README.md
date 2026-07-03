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
| `0xB2` GET_DIAGNOSTIC | `B2` | reply `[branch, rstsckr(4, LE)]` — see [Diagnosing a bootloader entry](#diagnosing-a-bootloader-entry) below |

`state`: 0 IDLE, 1 BUSY, 2 READY, 3 ERROR. `err`: 0 ok, 5 CRC mismatch, 6 region overflow.

## Brick safety

1. **Interrupted flash** → metadata is the *last* thing written, so CRC fails →
   the bootloader stays in flashing mode → the host just retries (no cable).
2. **BOOT0 jumper + WCH factory ROM bootloader** = the unbrickable backstop to
   re-flash *this* bootloader via WCHISPTool.

## Power-settle delay (brownout mitigation)

`main()` waits ~50 ms (a raw HSI busy-loop, before any flash access) so the 5V
rail can finish charging through the module's input polyfuse + 1N4007 + bulk
caps before the boot-time `app_is_valid()` CRC scan runs. **Why:** the DEV-12
investigation confirmed (via the `0xB2` diagnostic below: *branch B + PORRSTF*)
that on a marginal 5V ramp — a weak/slow-ramping hub supply, compounded by the
uninitialised WS2812b inrush that loads the rail before any firmware runs — the
CH32V203 browns out *during* that CRC scan, corrupting the read even though the
flash contents are fine, so the bootloader wrongly falls to flashing mode. The
settle delay removes the sagging-rail condition rather than trying to make the
scan tolerate it. It is **cheap insurance, not the primary fix** — the real fix
is a clean supply / more input bulk capacitance / defining the WS2812b power-up
state (see `module-usb-led` hardware notes). Sized to stay imperceptible: it
gates every power-on, including a healthy app launch, so it is ~50 ms not
seconds. **Not attempted:** reordering `clock_to_hse_48()` before the CRC scan —
running the PLL at 48 MHz on a still-sagging rail is *more* voltage-sensitive
than the low-power HSI scan, so it could worsen the brownout.

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

## Diagnosing a bootloader entry

If a module unexpectedly shows up in the bootloader (PID `4E42`) instead of the
app — especially after being power-cycled or moved to a different host/rig —
`0xB2 GET_DIAGNOSTIC` answers **why**, without needing to flash anything first:

```
powershell -ExecutionPolicy Bypass -File tools/usb_get_diagnostic.ps1
```

`main()`'s boot decision has two independent paths into flashing mode, which
look identical from the outside (the module "just is in the bootloader"):

- **Branch A** — the app itself wrote the `0xB0` handoff magic and reset
  (a legitimate, expected OTA entry).
- **Branch B** — `app_is_valid()` found bad/missing metadata or a CRC mismatch
  on the boot-time scan (the brick-safe fallback — could mean a genuinely bad
  app image, **or** a marginal-power CRC-read glitch on a specific rig even
  though the flash contents are actually fine).

`0xB2` reports which branch fired on the *last* flashing-mode entry, plus the
raw `RCC->RSTSCKR` reset-cause register (`PORRSTF`/`PINRSTF`/`SFTRSTF`/
`IWDGRSTF`/`WWDGRSTF`/`LPWRRSTF`) captured at the very first line of `main()`,
before anything else can disturb it. Branch A with `SFTRSTF` = a normal,
expected OTA handoff. Branch B = the app failed its CRC scan.

⚠️ **Interpreting `PORRSTF` (learned the hard way in DEV-12):** `PORRSTF` is set
on **every** power-on — it only means "the last reset was a power cycle", which
is always true after unplugging/replugging a module. So *branch B + PORRSTF does
NOT prove a brownout* — it only says "a power cycle happened and the CRC scan
then failed." The DEV-12 failure read exactly that, and the real cause turned out
to be **read-marginal flash from the old halfword programmer** (fixed — see
*CH32V203 flash programming* below), exposed by (not caused by) a marginal supply.
`0xB2` is still useful for telling branch A from branch B and spotting a genuine
`SFTRSTF`/`IWDGRSTF`/`WWDGRSTF`, but do not read `PORRSTF` alone as "brownout".

**Caveat:** the diagnostic is stored in no-init RAM (a separate word from the
handoff cell, at `0x200027F4`, never touched by the flashing protocol) — it
survives a *warm* reset but is not guaranteed to survive a full power removal.
If the module has been completely unplugged since the entry you're
investigating, `branch=0` / a zeroed `RSTSCKR` is itself informative: it means
SRAM wasn't retained across that power event, which is circumstantial evidence
for real power marginality on that rig, not a tool failure.

**Requires a bootloader built after this diagnostic was added (2026-07)** —
older bootloaders (including any already SWD/BOOT0-flashed onto a board before
this change) do not implement `0xB2` and will not reply; re-flash
`firmware/bin/noknok_usb_bootloader.bin` via BOOT0 + WCHISPTool to add it.

## CH32V203 flash programming (for maintainers)

This bootloader uses the **256-byte FAST PAGE program/erase** path — the WCH-
characterised method for V20x, the same one the vendor flash library
(`ch32fun/extralibs/ch20x_30x_flash.h`) and WCHISPTool use:

- **Unlock** both `FLASH->KEYR` **and** `FLASH->MODEKEYR` (KEY1, KEY2 each) —
  `MODEKEYR` unlocks the fast/page mode; without it the page path is locked.
- **Erase**: `CR_PAGE_ER` + `CR_STRT` per 256-byte page.
- **Program**: `CR_PAGE_PG`, load a full 256-byte page into the word buffer
  (poll `WRBSY` / `STATR bit 1` between words), then `CR_PG_STRT` to burn it.
- **Finish**: poll `BSY`, check/clear `EOP` + `WRPRTERR`.

⚠️ **Why NOT the legacy halfword program (`FLASH_CTLR_PG` + 16-bit writes):** an
earlier version of this bootloader used it, and it caused the DEV-12 failure — a
flash **retention** defect. OTA-flashed apps VERIFY correct at write time and run
immediately after BOOT, but on the **next power cycle** the boot-time CRC scan
fails and the module drops to the bootloader. Reproduced entirely on **clean PC
5V** (no supply/brownout involved): byte-identical content flashed by WCHISPTool
survives power cycles; the same bytes flashed by the old halfword path did not.
The single-halfword program (BSY-wait only, no EOP/error check) **under-committed
the cells** — they read back correct while fresh but lose state after power-off.
The fast-page path (this version) burns a full page in one managed, fully-
committed pulse, plus a per-page read-back verify + retry, matching WCHISPTool's
retention. Every OTA update would otherwise be effectively bricked-to-bootloader
on the next unplug. **Do not revert to halfword programming.**

⚠️ Also do **not** copy the CH32V003 fast-program buffer bits: on the V20x,
`0x40000`/`0x80000` (`CR_BER32`/`CR_BER64`) are **32K/64K block-erase** opcodes,
not buffer reset/load — using them wipes large regions.

## License

Firmware: MIT. See the central
[License, Safety & Liability](https://buildwithnoknok.github.io/safety-and-license/) page.
