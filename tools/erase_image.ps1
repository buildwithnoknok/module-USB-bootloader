<#
  erase_image.ps1 - generate a fully-blank (0xFF) 32 KB image to WCHISPTool-flash
  onto the CH32V203, guaranteeing a known-clean chip before a from-scratch test.

  Why: most ISP tools (WCHISPTool included, as far as we've observed) only
  erase the flash pages actually covered by the file you give them - NOT the
  whole chip - so a partial flash (just the bootloader, or just an app) can
  leave old data sitting in the untouched regions. Flashing this all-0xFF
  image covers the full 32 KB in one pass, and since flash can only be
  programmed after an erase (bits can only flip 1->0 without one), writing
  0xFF to every byte forces every page in range to actually be erased.

  After flashing this, the chip is genuinely blank: no bootloader, no app, no
  metadata. It will NOT enumerate as either noknok PID (4E4E app / 4E42
  bootloader) - only the BOOT0-jumper WCH factory bootloader (VID 4348 / PID
  55E0) can talk to it at that point. That's the expected, correct signal
  that the erase worked.

  Usage:
    powershell -ExecutionPolicy Bypass -File erase_image.ps1
    powershell -ExecutionPolicy Bypass -File erase_image.ps1 -OutBin blank.bin -OutHex blank.hex
#>
param(
    [string] $OutBin = "C:\Users\chris\noknok\firmware\noknok_leds_blank.bin",
    [string] $OutHex = "C:\Users\chris\noknok\firmware\noknok_leds_blank.hex",
    [int]    $Size   = 0x8000,       # 32 KB - full chip
    [uint32] $BaseAddr = 0x08000000
)

$img = New-Object byte[] $Size
for ($i = 0; $i -lt $Size; $i++) { $img[$i] = 0xFF }

$outDir = Split-Path $OutBin -Parent
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
[System.IO.File]::WriteAllBytes($OutBin, $img)
Write-Host ("Wrote {0} ({1} bytes, all 0xFF)" -f $OutBin, $img.Length)

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
$upper = [uint16](($BaseAddr -shr 16) -band 0xFFFF)
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
Write-Host "Flash this via WCHISPTool (BOOT0 jumper) FIRST to guarantee a blank chip, before flashing the bootloader or app."