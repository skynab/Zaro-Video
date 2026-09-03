# Capture the live zaro-preview window to a PNG.
#
# The program monitor is a QRhiWidget on a D3D swapchain and will come out
# black -- that is expected, see SKILL.md. Everything else in the window
# captures correctly.
#
#   powershell -ExecutionPolicy Bypass -File shotwin.ps1 -Out C:\path\live.png

param(
    [string]$Out = "$env:TEMP\cutreel.png",
    [string]$ProcessName = "zaro-preview"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinShot {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int L, T, R, B; }
}
"@

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } |
        Select-Object -First 1
if ($null -eq $proc) {
    Write-Error "no $ProcessName window found; is it running and past its splash?"
    exit 1
}

# Bring it up first: CopyFromScreen reads the desktop, so an obscured window
# captures whatever is sitting on top of it.
[void][WinShot]::SetForegroundWindow($proc.MainWindowHandle)
Start-Sleep -Milliseconds 700

$r = New-Object WinShot+RECT
[void][WinShot]::GetWindowRect($proc.MainWindowHandle, [ref]$r)
$w = $r.R - $r.L
$h = $r.B - $r.T
if ($w -le 0 -or $h -le 0) {
    Write-Error "window rect is empty ($w x $h); the window may be minimised"
    exit 1
}

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
try {
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output "saved $Out ($w x $h)"
} finally {
    $g.Dispose()
    $bmp.Dispose()
}
