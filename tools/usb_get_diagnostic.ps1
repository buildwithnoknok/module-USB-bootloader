<#
  usb_get_diagnostic.ps1 - read GET_DIAGNOSTIC (0xB2) from a noknok USB module's
  bootloader, to answer: "why is this module sitting in the bootloader right now?"

  Only the BOOTLOADER (PID 4E42) implements 0xB2 - if the module is currently
  running the application (PID 4E4E), there is nothing to diagnose (it isn't in
  flashing mode), so this script says so explicitly instead of hanging.

  Background (DEV-12 "module reverts to bootloader on some rigs" investigation):
  the bootloader's boot decision in main() has TWO independent ways to end up in
  flashing mode, indistinguishable from the outside without this diagnostic:
    branch A: the app legitimately wrote the 0xB0 handoff magic before resetting
    branch B: app_is_valid() found bad/missing metadata or a CRC mismatch
  0xB2 reports which branch fired on the LAST flashing-mode entry, plus the raw
  RCC->RSTSCKR reset-cause register captured at the very top of main() (before
  anything can disturb it) on that same entry - so a genuine brownout/POR shows
  up here even if the module has since been re-flashed back to a healthy state
  (DIAG_CELL is a separate no-init RAM word from the flashing protocol itself,
  only ever read, never touched by ERASE/WRITE/VERIFY/BOOT).

  IMPORTANT CAVEAT: DIAG_CELL is no-init SRAM, like the handoff cell - it
  survives a WARM reset (software reset, watchdog, the 0xB0 handoff itself) but
  is NOT guaranteed to survive a genuine COLD power-up (power fully removed and
  re-applied). If the module has been fully unplugged since the flashing-mode
  entry you're trying to diagnose, branch=0 / garbage RSTSCKR bits are possible
  and are THEMSELVES diagnostic (they mean "SRAM wasn't retained across that
  power event" - consistent with, though not proof of, real power marginality).

  Usage:
    powershell -ExecutionPolicy Bypass -File usb_get_diagnostic.ps1
    powershell -ExecutionPolicy Bypass -File usb_get_diagnostic.ps1 -Port COM14

  Reply (5 bytes): [branch, rstsckr0, rstsckr1, rstsckr2, rstsckr3 (LE)]
#>
param(
    [string] $Port
)

$VID            = 'VID_1209'
$PID_APP        = 'PID_4E4E'
$PID_BOOTLOADER = 'PID_4E42'

# --- raw COM handle (avoids .NET SerialPort SetCommState issues on the minimal CDC) ---
if (-not ([System.Management.Automation.PSTypeName]'NkCom').Type) {
Add-Type @'
using System; using System.IO; using System.Runtime.InteropServices; using Microsoft.Win32.SafeHandles;
public static class NkCom {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Auto)]
  static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
  [StructLayout(LayoutKind.Sequential)] struct CT { public uint RI, RM, RC, WM, WC; }
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetCommTimeouts(SafeFileHandle h, ref CT t);
  public static FileStream Open(string p){
    var h = CreateFile(@"\\.\"+p, 0xC0000000, 0, IntPtr.Zero, 3, 0x80, IntPtr.Zero);
    if (h.IsInvalid) throw new IOException("open "+p+" err "+Marshal.GetLastWin32Error());
    var t = new CT(); t.RC = 1000; SetCommTimeouts(h, ref t);
    return new FileStream(h, FileAccess.ReadWrite);
  }
}
'@
}

function Find-Port([string]$pidPart) {
    Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.DeviceID -like "*$VID&$pidPart*" -and $_.Name -match '\((COM\d+)\)' } |
        ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } } | Select-Object -First 1
}

if (-not $Port) {
    $Port = Find-Port $PID_BOOTLOADER
    if (-not $Port) {
        $appPort = Find-Port $PID_APP
        if ($appPort) {
            Write-Host "Module is running the APPLICATION (PID 4E4E) on $appPort - nothing to diagnose (0xB2 is bootloader-only, and the module isn't in flashing mode right now)."
            exit 1
        }
        Write-Error "No noknok USB module found (checked app PID 4E4E and bootloader PID 4E42)."
        exit 1
    }
}
Write-Host "Bootloader on $Port"

$fs = [NkCom]::Open($Port)
try {
    Start-Sleep -Milliseconds 200
    $fs.Write(([byte[]]@(0xB2)), 0, 1)
    $fs.Flush()
    $buf = New-Object byte[] 5
    $n = $fs.Read($buf, 0, 5)
    if ($n -lt 5) { throw "GET_DIAGNOSTIC: short reply ($n byte(s)) - is this bootloader built with the 0xB2 diagnostic (Jul 2026+)?" }

    $branch  = $buf[0]
    $rstsckr = [uint32]$buf[1] -bor ([uint32]$buf[2] -shl 8) -bor ([uint32]$buf[3] -shl 16) -bor ([uint32]$buf[4] -shl 24)

    $branchText = switch ($branch) {
        0 { "NONE CAPTURED (no flashing-mode entry recorded since SRAM was last cold-powered - or this is the first entry since a fresh bootloader flash)" }
        1 { "A - HANDOFF MAGIC (app legitimately sent 0xB0 and reset into the bootloader)" }
        2 { "B - APP_IS_VALID() FAILED (bad/missing metadata, or a CRC mismatch on the boot-time scan)" }
        default { "UNKNOWN ($branch)" }
    }

    Write-Host ""
    Write-Host ("Branch: {0}" -f $branchText)
    Write-Host ("RCC->RSTSCKR raw: 0x{0:X8}" -f $rstsckr)

    $flags = New-Object System.Collections.Generic.List[string]
    if ($rstsckr -band 0x08000000) { $flags.Add("PORRSTF (power-on / power-down reset)") }
    if ($rstsckr -band 0x04000000) { $flags.Add("PINRSTF (NRST pin reset)") }
    if ($rstsckr -band 0x10000000) { $flags.Add("SFTRSTF (software reset - NVIC_SystemReset, e.g. a legitimate 0xB0)") }
    if ($rstsckr -band 0x20000000) { $flags.Add("IWDGRSTF (independent watchdog reset)") }
    if ($rstsckr -band 0x40000000) { $flags.Add("WWDGRSTF (window watchdog reset)") }
    if ($rstsckr -band 0x80000000) { $flags.Add("LPWRRSTF (low-power/standby reset)") }
    if ($flags.Count -eq 0) {
        Write-Host "Reset-cause flags: none set (either a genuinely clean state, or SRAM was not retained across a cold power-up since this entry - see script header caveat)"
    } else {
        Write-Host "Reset-cause flags set:"
        foreach ($f in $flags) { Write-Host ("  - {0}" -f $f) }
    }
    Write-Host ""
    Write-Host "Interpretation: PORRSTF set + branch B  -> genuine brownout/POR on this power path, consistent with the boot-time CRC scan glitching (hypothesis B)."
    Write-Host "                SFTRSTF set + branch A  -> a real, deliberate 0xB0 handoff (expected/healthy OTA entry)."
    Write-Host "                PORRSTF/PINRSTF set + branch A -> the physically-plausible 'stale SRAM coincidentally matched the magic on a cold power-up' theory (hypothesis A) - magic present despite a real power-cycle, not a software handoff."
} finally { try { $fs.Close() } catch {} }
