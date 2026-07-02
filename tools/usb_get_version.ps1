<#
  usb_get_version.ps1 - read GET_VERSION (0xB1) from a noknok USB module's app.

  Only the APPLICATION (PID 4E4E) implements 0xB1 - the bootloader (PID 4E42)
  does not. If the module is currently sitting in the bootloader (e.g. an app
  flash never completed / metadata invalid), this script says so explicitly
  instead of hanging - that state itself is useful diagnostic information.

  Usage:
    powershell -ExecutionPolicy Bypass -File usb_get_version.ps1
    powershell -ExecutionPolicy Bypass -File usb_get_version.ps1 -Port COM14

  Reply (4 bytes): [PROTOCOL_VERSION, FW_MAJOR, FW_MINOR, FW_PATCH]
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
    $Port = Find-Port $PID_APP
    if (-not $Port) {
        $blPort = Find-Port $PID_BOOTLOADER
        if ($blPort) {
            Write-Host "Module is in the BOOTLOADER (PID 4E42) on $blPort - no app running, GET_VERSION not available."
            Write-Host "Flash a valid app with usb_flash.ps1 first."
            exit 1
        }
        Write-Error "No noknok USB module found (checked app PID 4E4E and bootloader PID 4E42)."
        exit 1
    }
}
Write-Host "App on $Port"

$fs = [NkCom]::Open($Port)
try {
    Start-Sleep -Milliseconds 200
    $fs.Write(([byte[]]@(0xB1)), 0, 1)
    $fs.Flush()
    $buf = New-Object byte[] 4
    $n = $fs.Read($buf, 0, 4)
    if ($n -lt 4) { throw "GET_VERSION: short reply ($n byte(s))" }
    Write-Host ("Protocol: {0}   Firmware: {1}.{2}.{3}" -f $buf[0], $buf[1], $buf[2], $buf[3])
} finally { try { $fs.Close() } catch {} }
