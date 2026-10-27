Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, "MinovaUpscaler")
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if (-not $win) { Write-Host "GUI window not found"; exit 1 }
$all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
Write-Host "controls: $($all.Count)"
foreach ($e in $all) {
  Write-Host ("  {0} | name='{1}' id={2}" -f $e.Current.ControlType.ProgrammaticName, $e.Current.Name, $e.Current.AutomationId)
}
