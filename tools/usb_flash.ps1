<#
  usb_flash.ps1 - OTA-flash a noknok USB module over USB via the noknok USB bootloader.

  Mirrors the I2C module_flasher flow: detect the bootloader (VID 1209 / PID 4E4F),
  ERASE the app region, stream the .bin in WRITE chunks, VERIFY (CRC32), then BOOT.
  If the module is running the application (PID 4E4E) it is told to enter the
  bootloader first (command 0xB0).

  Usage:
    powershell -ExecutionPolicy Bypass -File usb_flash.ps1 -Bin path\to\app.bin
    powershell -ExecutionPolicy Bypass -File usb_flash.ps1 -Bin app.bin -Port COM14

  Bootloader protocol (host waits for each [state,err] reply):
    0x01 ERASE | 0x02 WRITE n <n bytes> | 0x03 READ_STATUS | 0x04 VERIFY crc32(4 LE) | 0x05 BOOT
    state: 0 IDLE 1 BUSY 2 READY 3 ERROR ; err: 0 ok, 5 CRC mismatch, 6 region overflow
#>
param(
    [Parameter(Mandatory=$true)][string] $Bin,
    [string] $Port,
    [int]    $Chunk = 32
)

$VID            = 'VID_1209'
$PID_APP        = 'PID_4E4E'
$PID_BOOTLOADER = 'PID_4E4F'

if (-not (Test-Path $Bin)) { Write-Error "bin not found: $Bin"; exit 1 }
$image = [System.IO.File]::ReadAllBytes($Bin)
Write-Host ("Image: {0} ({1} bytes)" -f $Bin, $image.Length)

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
    var t = new CT(); t.RC = 3000; SetCommTimeouts(h, ref t);   // up to 3 s (ERASE takes ~300 ms)
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

# zlib CRC32 (poly 0xEDB88320) using [long] math to avoid PowerShell uint32 cast issues
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

function Send-Cmd($fs, [byte[]]$bytes) { $fs.Write($bytes, 0, $bytes.Length); $fs.Flush() }
function Read-Status($fs) {
    $buf = New-Object byte[] 2
    $n = $fs.Read($buf, 0, 2)
    if ($n -lt 2) { return $null }
    return @{ state = $buf[0]; err = $buf[1] }
}
function Check($st, [string]$what) {
    if ($null -eq $st) { throw ("{0}: no reply from bootloader" -f $what) }
    if ($st.err -ne 0) { throw ("{0}: bootloader error {1} (state {2})" -f $what, $st.err, $st.state) }
    Write-Host ("  {0}: OK (state {1})" -f $what, $st.state)
}

# --- locate the bootloader; if only the app is present, ask it to enter the bootloader ---
if (-not $Port) {
    $Port = Find-Port $PID_BOOTLOADER
    if (-not $Port) {
        $appPort = Find-Port $PID_APP
        if ($appPort) {
            Write-Host "App found on $appPort - sending 0xB0 ENTER_BOOTLOADER..."
            $a = [NkCom]::Open($appPort)
            try { Send-Cmd $a ([byte[]]@(0xB0)) } catch {} finally { try { $a.Close() } catch {} }
            for ($i=0; $i -lt 30 -and -not $Port; $i++) { Start-Sleep -Milliseconds 250; $Port = Find-Port $PID_BOOTLOADER }
        }
    }
}
if (-not $Port) { Write-Error "noknok USB bootloader (PID 4E4F) not found."; exit 1 }
Write-Host "Bootloader on $Port"

$fs = [NkCom]::Open($Port)
try {
    Start-Sleep -Milliseconds 200

    Write-Host "ERASE..."
    Send-Cmd $fs ([byte[]]@(0x01))
    Check (Read-Status $fs) 'ERASE'

    Write-Host ("WRITE {0} bytes in {1}-byte chunks..." -f $image.Length, $Chunk)
    $off = 0
    while ($off -lt $image.Length) {
        $n = [Math]::Min($Chunk, $image.Length - $off)
        $cmd = New-Object byte[] (2 + $n)
        $cmd[0] = 0x02; $cmd[1] = [byte]$n
        [Array]::Copy($image, $off, $cmd, 2, $n)
        Send-Cmd $fs $cmd
        $st = Read-Status $fs
        if ($null -eq $st -or $st.err -ne 0) { throw ("WRITE @${off}: error {0}" -f ($(if($st){$st.err}else{'no reply'}))) }
        $off += $n
    }
    Write-Host ("  WRITE: OK ({0} bytes)" -f $off)

    $crc = Get-Crc32 $image
    Write-Host ("VERIFY crc32 = 0x{0:X8}..." -f $crc)
    $cb = New-Object byte[] 5
    $cb[0] = 0x04
    $cb[1] = [byte]($crc -band 0xFF); $cb[2] = [byte](($crc -shr 8) -band 0xFF)
    $cb[3] = [byte](($crc -shr 16) -band 0xFF); $cb[4] = [byte](($crc -shr 24) -band 0xFF)
    Send-Cmd $fs $cb
    $st = Read-Status $fs
    if ($null -eq $st) { throw "VERIFY: no reply" }
    if ($st.state -ne 2 -or $st.err -ne 0) { throw ("VERIFY failed: state {0} err {1}" -f $st.state, $st.err) }
    Write-Host "  VERIFY: OK (app accepted)"

    Write-Host "BOOT..."
    Send-Cmd $fs ([byte[]]@(0x05))   # no reply; device resets into the app
    Start-Sleep -Milliseconds 800
    Write-Host "Done. The module should re-enumerate as the application (PID 4E4E)."
} finally { try { $fs.Close() } catch {} }
