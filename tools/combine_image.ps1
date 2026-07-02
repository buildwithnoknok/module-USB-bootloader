<#
  combine_image.ps1 - build a single flash image (bootloader + app + valid
  metadata) for one-pass blank-board bring-up via WCHISPTool.

  Why: the normal path is TWO steps - (1) SWD/BOOT0+WCHISPTool flashes just
  the bootloader, (2) usb_flash.ps1 flashes the app over USB, and it's the
  bootloader's own VERIFY (0x04) step that computes+writes the metadata
  {magic, app_len, app_crc32} marking the app valid. A raw SWD/WCHISPTool
  flash of the app.bin skips that step entirely, which is why a directly
  SWD-flashed app never boots (see the "USB LEDs won't enumerate" writeup).

  This script bakes the SAME metadata (identical CRC32 algorithm, identical
  layout) into a single combined image at build time, so ONE WCHISPTool flash
  at 0x08000000 produces a board that's immediately valid and boots straight
  to the app - no USB/PowerShell step needed for initial bring-up.

  This does NOT replace usb_flash.ps1 for ROUTINE updates - once a board is
  bootstrapped (via this OR the normal 2-step path), all future app updates
  still go through the bootloader's own USB OTA protocol as usual. This tool
  is for the one-time blank-board case only.

  Flash map (from module-USB-bootloader/README.md), all offsets from 0x08000000:
    0x0000-0x1FFF (8 KB)      bootloader
    0x2000-0x7EFF (24320 B)   application
    0x7F00-0x7FFF (256 B)     metadata {magic u32, app_len u32, app_crc32 u32}, LE

  Usage:
    powershell -ExecutionPolicy Bypass -File combine_image.ps1
    powershell -ExecutionPolicy Bypass -File combine_image.ps1 -BootBin path\to\bl.bin -AppBin path\to\app.bin -OutHex out.hex
#>
param(
    [string] $BootBin = "C:\Users\chris\noknok\repos\module-USB-bootloader\firmware\bin\noknok_usb_bootloader.bin",
    [string] $AppBin  = "C:\Users\chris\noknok\repos\module-usb-led\firmware\bin\noknok_leds.bin",
    [string] $OutBin  = "C:\Users\chris\noknok\firmware\noknok_leds_combined.bin",
    [string] $OutHex  = "C:\Users\chris\noknok\firmware\noknok_leds_combined.hex"
)

$BOOT_REGION_LEN = 0x2000       # 8192  - bootloader region
$APP_REGION_LEN  = 0x5F00       # 24320 - application region (META_FLASH_ADDR - APP_FLASH_BASE)
$META_LEN        = 0x100        # 256   - metadata region
$META_MAGIC      = 0x6E6B5542L  # 'nkUB' - must match noknok_usb_bootloader.c META_MAGIC
$BASE_ADDR       = 0x08000000

if (-not (Test-Path $BootBin)) { Write-Error "bootloader bin not found: $BootBin"; exit 1 }
if (-not (Test-Path $AppBin))  { Write-Error "app bin not found: $AppBin"; exit 1 }

$boot = [System.IO.File]::ReadAllBytes($BootBin)
$app  = [System.IO.File]::ReadAllBytes($AppBin)
Write-Host ("Bootloader: {0} ({1} bytes)" -f $BootBin, $boot.Length)
Write-Host ("App:        {0} ({1} bytes)" -f $AppBin,  $app.Length)

if ($boot.Length -gt $BOOT_REGION_LEN) { Write-Error ("bootloader too large: {0} > {1}" -f $boot.Length, $BOOT_REGION_LEN); exit 1 }
if ($app.Length  -gt $APP_REGION_LEN)  { Write-Error ("app too large: {0} > {1}" -f $app.Length, $APP_REGION_LEN); exit 1 }

# zlib CRC32 (poly 0xEDB88320) - identical to the bootloader's crc32_calc() and
# Python binascii.crc32; copied verbatim from usb_flash.ps1 for consistency.
function Get-Crc32([byte[]]$data) {
    $poly = 0xEDB88320L
    $crc  = 0xFFFFFFFFL
    foreach ($b in $data) {
        $crc = $crc -bxor [long]$b
        for ($k=0; $k -lt 8; $k++) {
            if ($crc -band 1L) { $crc = (($crc -shr 1) -bxor $poly) -band 0xFFFFFFFFL }
            else               { $crc =  ($crc -shr 1)               -band 0xFFFFFFFFL }
        }
    }
    return ($crc -bxor 0xFFFFFFFFL) -band 0xFFFFFFFFL
}

# CRC is over the RAW app bytes only (unpadded) - exactly what the bootloader's
# do_verify() computes over the bytes actually streamed to it.
$crc = Get-Crc32 $app
Write-Host ("App CRC32:  0x{0:X8}" -f $crc)

# --- assemble the combined image, 0xFF-filled (flash-erased state) ---
$total = $BOOT_REGION_LEN + $APP_REGION_LEN + $META_LEN
$img = New-Object byte[] $total
for ($i = 0; $i -lt $total; $i++) { $img[$i] = 0xFF }

[Array]::Copy($boot, 0, $img, 0, $boot.Length)
[Array]::Copy($app,  0, $img, $BOOT_REGION_LEN, $app.Length)

function Write-U32LE([byte[]]$arr, [int]$offset, [uint32]$val) {
    $arr[$offset]     = [byte]( $val         -band 0xFF)
    $arr[$offset + 1] = [byte](($val -shr 8) -band 0xFF)
    $arr[$offset + 2] = [byte](($val -shr 16) -band 0xFF)
    $arr[$offset + 3] = [byte](($val -shr 24) -band 0xFF)
}

$metaOffset = $BOOT_REGION_LEN + $APP_REGION_LEN   # 0x7F00
Write-U32LE $img $metaOffset       ([uint32]$META_MAGIC)
Write-U32LE $img ($metaOffset + 4) ([uint32]$app.Length)
Write-U32LE $img ($metaOffset + 8) ([uint32]$crc)
Write-Host ("Metadata @ 0x{0:X4}: magic=0x{1:X8} app_len={2} crc32=0x{3:X8}" -f `
    $metaOffset, $META_MAGIC, $app.Length, $crc)

# --- self-check: replicate app_is_valid() against the assembled image ---
$checkCrc = Get-Crc32 ($img[$BOOT_REGION_LEN..($BOOT_REGION_LEN + $app.Length - 1)])
if ($checkCrc -ne $crc) { Write-Error "self-check FAILED: recomputed CRC does not match"; exit 1 }
Write-Host "Self-check: OK (metadata matches assembled image)"

# --- write raw combined .bin ---
$outDir = Split-Path $OutBin -Parent
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
[System.IO.File]::WriteAllBytes($OutBin, $img)
Write-Host ("Wrote {0} ({1} bytes)" -f $OutBin, $img.Length)

# --- write Intel HEX (.hex) for WCHISPTool ---
function Get-HexChecksum([byte[]]$bytes) {
    $sum = 0
    foreach ($b in $bytes) { $sum += $b }
    return [byte]((-$sum) -band 0xFF)
}
function Format-HexRecord([byte]$len, [uint16]$addr, [byte]$type, [byte[]]$data) {
    $rec = New-Object byte[] (4 + $data.Length)
    $rec[0] = $len
    $rec[1] = [byte](($addr -shr 8) -band 0xFF)
    $rec[2] = [byte]($addr -band 0xFF)
    $rec[3] = $type
    if ($data.Length -gt 0) { [Array]::Copy($data, 0, $rec, 4, $data.Length) }
    $chk = Get-HexChecksum $rec
    $hexStr = ($rec | ForEach-Object { $_.ToString("X2") }) -join ""
    return (":{0}{1}" -f $hexStr, $chk.ToString("X2"))
}

$lines = New-Object System.Collections.Generic.List[string]
$upper = [uint16](($BASE_ADDR -shr 16) -band 0xFFFF)
$lines.Add((Format-HexRecord 2 0 4 ([byte[]]@([byte](($upper -shr 8) -band 0xFF), [byte]($upper -band 0xFF)))))

$chunk = 16
for ($off = 0; $off -lt $img.Length; $off += $chunk) {
    $n = [Math]::Min($chunk, $img.Length - $off)
    $data = New-Object byte[] $n
    [Array]::Copy($img, $off, $data, 0, $n)
    $addr16 = [uint16]($off -band 0xFFFF)
    $lines.Add((Format-HexRecord ([byte]$n) $addr16 0 $data))
}
$lines.Add(":00000001FF")
[System.IO.File]::WriteAllLines($OutHex, $lines)
Write-Host ("Wrote {0} ({1} records)" -f $OutHex, $lines.Count)

Write-Host ""
Write-Host "Flash noknok_leds_combined.hex (or .bin at base 0x08000000) via WCHISPTool - ONE pass, no BOOT0 jumper needed beyond what WCHISPTool itself requires, no separate usb_flash.ps1 step."
