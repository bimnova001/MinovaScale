param([string]$OutPng = "D:\MinovaUpscaler\shot_presenter11.png")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  public struct R { public int L, T, Rt, B; }
}
'@
$w = Get-Process minova_capture -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle } | Select-Object -First 1
if ($w -eq $null) { Write-Host "no window"; exit 1 }
$h = $w.MainWindowHandle
$r = New-Object W+R
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$ww = $r.Rt - $r.L; $wh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($ww, $wh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($OutPng)
Write-Host "shot ${ww}x${wh} -> $OutPng"
