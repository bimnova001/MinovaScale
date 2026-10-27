param([string]$Process = "minova_capture", [string]$OutPng = "D:\MinovaUpscaler\shot_a.png")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class W2 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  public struct R { public int L, T, Rt, B; }
}
'@
$w = Get-Process $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($w -eq $null) { Write-Host "no window for $Process"; exit 1 }
$h = $w.MainWindowHandle
$r = New-Object W2+R
[W2]::GetWindowRect($h, [ref]$r) | Out-Null
$ww = $r.Rt - $r.L; $wh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($ww, $wh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($OutPng)
Write-Host "$Process rect L=$($r.L) T=$($r.T) ${ww}x${wh} -> $OutPng"
