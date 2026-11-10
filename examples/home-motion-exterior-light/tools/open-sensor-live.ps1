param([switch]$Restart, [switch]$Stop)
$ErrorActionPreference = 'Stop'
$monitorScript = Join-Path $PSScriptRoot 'sensor-live.ps1'
$viewScript = Join-Path $PSScriptRoot 'sensor-live-view.ps1'
$parseTokens = $null
$parseErrors = $null
$null = [System.Management.Automation.Language.Parser]::ParseFile($monitorScript, [ref]$parseTokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$null = [System.Management.Automation.Language.Parser]::ParseFile($viewScript, [ref]$parseTokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$null = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'tsl2572.ps1'), [ref]$parseTokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$existing = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object {
    $_.CommandLine -like '*\tools\sensor-live.ps1*'
})
$views = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object {
    $_.CommandLine -like '*\tools\sensor-live-view.ps1*'
})
if ($Restart -or $Stop) {
    foreach ($oldMonitor in $existing) {
        Stop-Process -Id $oldMonitor.ProcessId -Force
    }
    foreach ($oldView in $views) {
        Stop-Process -Id $oldView.ProcessId -Force
    }
    $existing = @()
    $views = @()
}
if ($Stop) {
    [pscustomobject]@{status='stopped'} | ConvertTo-Json -Compress
    exit
}
if ($existing.Count) {
    $monitorPid = $existing[0].ProcessId
} else {
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + $monitorScript + '"'),'-Background')
    $monitorProcess = Start-Process powershell.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $monitorPid = $monitorProcess.Id
}
if ($views.Count) {
    $viewPid = $views[0].ProcessId
} else {
    $viewArguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + $viewScript + '"'))
    $viewProcess = Start-Process powershell.exe -ArgumentList $viewArguments -WindowStyle Normal -PassThru
    $viewPid = $viewProcess.Id
}
$ports = @(Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Name,PNPDeviceID)
[pscustomobject]@{monitor_pid=$monitorPid;view_pid=$viewPid;ports=$ports} | ConvertTo-Json -Depth 4 -Compress
