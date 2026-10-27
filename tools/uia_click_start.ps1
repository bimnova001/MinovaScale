param([string]$PngOut = "")
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Click {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  public const uint LEFTDOWN = 0x02, LEFTUP = 0x04;
  public static void Do(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(60);
    mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(30);
    mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
  }
}
'@

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, "MinovaUpscaler")
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if (-not $win) { Write-Host "GUI window not found"; exit 1 }

function Find-ById($win, $id) {
  $cond2 = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
  return $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond2)
}

# 1) select first row of the window list
$list = Find-ById $win "101"
if (-not $list) { Write-Host "no list"; exit 1 }
$r = $list.Current.BoundingRectangle
Write-Host "list rect: $r"
[Click]::Do([int]($r.X + 60), [int]($r.Y + 10))
Start-Sleep 1

# 2) press Start (id 111)
$btn = Find-ById $win "111"
$br = $btn.Current.BoundingRectangle
Write-Host "start btn: $br"
[Click]::Do([int]($br.X + $br.Width/2), [int]($br.Y + $br.Height/2))
Start-Sleep 10

if ($PngOut) {
  pwsh -File "D:\MinovaUpscaler\tools\shot_proc.ps1" -Process minova_capture -OutPng $PngOut
}
