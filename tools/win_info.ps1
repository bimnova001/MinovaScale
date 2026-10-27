Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class WinInfo {
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);
}
'@
$found = @{}
[WinInfo]::EnumWindows({ param($h, $lp)
  $sb = New-Object System.Text.StringBuilder 256
  [WinInfo]::GetWindowTextW($h, $sb, 256) | Out-Null
  $t = $sb.ToString()
  if ($t -like "*Minova*") {
    $pid2 = 0
    [WinInfo]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
    $vis = [WinInfo]::IsWindowVisible($h)
    $cloak = -1
    [WinInfo]::DwmGetWindowAttribute($h, 14, [ref]$cloak, 4) | Out-Null
    Write-Host "hwnd=0x$('{0:X}' -f $h) pid=$pid2 visible=$vis cloaked=$cloak title=$t"
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
Write-Host "--- minova_capture windows ---"
$capPid = (Get-Process minova_capture -ErrorAction SilentlyContinue | Select-Object -First 1).Id
if ($capPid) {
  [WinInfo]::EnumWindows({ param($h, $lp)
    $pid2 = 0
    [WinInfo]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
    if ($pid2 -eq $script:capPid) {
      $sb2 = New-Object System.Text.StringBuilder 256
      [WinInfo]::GetWindowTextW($h, $sb2, 256) | Out-Null
      $vis2 = [WinInfo]::IsWindowVisible($h)
      Write-Host "hwnd=0x$('{0:X}' -f $h) visible=$vis2 title=$($sb2.ToString())"
    }
    return $true
  }, [IntPtr]::Zero) | Out-Null
} else { Write-Host "capture not running" }
