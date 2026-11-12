param([ValidateSet('all','lighting','dt57','sensor')][string]$Board = 'all',
      [switch]$Elevated, [switch]$NoLogo, [switch]$ResetOnly, [switch]$HeaderLink,
      [switch]$StandaloneDemo, [ValidateRange(-1,100)][int]$FixedDimmer=-1,
      [switch]$PB5Display, [string]$TargetConfig='config\boards.local.json')
$ErrorActionPreference = 'Stop'
if($FixedDimmer -ge 0 -and (-not $StandaloneDemo -or $Board -ne 'dt57')){throw '-FixedDimmer requires the standalone dt57 board.'}
if($PB5Display -and (-not $StandaloneDemo -or $Board -ne 'sensor')){throw '-PB5Display requires the standalone sensor board.'}
if ($NoLogo -and $Board -ne 'dt57') { throw '-NoLogo requires -Board dt57.' }
if ($ResetOnly -and $Board -ne 'dt57') { throw '-ResetOnly requires -Board dt57.' }
if ($HeaderLink -and ($Board -notin @('sensor','dt57') -or $NoLogo -or $ResetOnly)) {
    throw '-HeaderLink requires a single sensor or dt57 board with the normal OLED profile.'
}
if ($StandaloneDemo -and ($HeaderLink -or $NoLogo -or $ResetOnly -or $Board -eq 'all')) {
    throw '-StandaloneDemo requires one named board and no other firmware mode.'
}
$root = Split-Path $PSScriptRoot
if(-not [IO.Path]::IsPathRooted($TargetConfig)){$TargetConfig=Join-Path $root $TargetConfig}
if(-not(Test-Path -LiteralPath $TargetConfig)){throw 'Copy config/boards.example.json to config/boards.local.json and configure your board identities before flashing.'}
$configuration=Get-Content -LiteralPath $TargetConfig -Raw|ConvertFrom-Json
$configured=@($configuration.boards)
if($configuration.version -ne 1 -or $configured.Count -ne 3){throw 'Expected board configuration version 1 with exactly three boards.'}
if(@($configured.board|Sort-Object -Unique).Count -ne 3 -or @($configured.parent|Sort-Object -Unique).Count -ne 3 -or @($configured.port|Sort-Object -Unique).Count -ne 3){throw 'Board roles, USB parents and serial ports must be unique.'}
foreach($entry in $configured){
    if($entry.board -notin @('sensor','dt57','lighting') -or $entry.port -notmatch '^COM[0-9]+$' -or
       $entry.parent -notmatch '^USB\\VID_0416&PID_5200\\[^\\]+$' -or $entry.parent -match 'REPLACE' -or
       $entry.debugger -notmatch '^0x[0-9a-fA-F]{8}$'){throw 'Configure each role, COM port, exact Nu-Link USB parent and debugger ID before flashing.'}
    if($entry.board -ne 'sensor' -and $entry.debugger -eq '0x00000000'){throw 'Only the isolated sensor role may use a zero debugger ID.'}
}
$resultFile = Join-Path $root $(if ($StandaloneDemo) { 'build\standalone-flash-result.json' } elseif ($HeaderLink) { 'build\header-link-flash-result.json' } elseif ($ResetOnly) { 'build\dt57-reset-result.json' } else { 'build\oled-flash-result.json' })
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    if ($Elevated) { throw 'Windows administrator rights were not granted.' }
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'),'-Board',$Board,'-Elevated')
    if ($NoLogo) { $arguments += '-NoLogo' }
    if ($ResetOnly) { $arguments += '-ResetOnly' }
    if ($HeaderLink) { $arguments += '-HeaderLink' }
    if ($StandaloneDemo) { $arguments += '-StandaloneDemo' }
    if ($FixedDimmer -ge 0) { $arguments += @('-FixedDimmer',[string]$FixedDimmer) }
    if ($PB5Display) { $arguments += '-PB5Display' }
    $arguments+=@('-TargetConfig',('"'+$TargetConfig+'"'))
    $child = Start-Process powershell.exe -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    if (Test-Path -LiteralPath $resultFile) { Get-Content -LiteralPath $resultFile -Raw }
    if ($child.ExitCode -ne 0) { throw "OLED firmware programming returned exit $($child.ExitCode)." }
    exit
}

$images=@{sensor='numaker-sensor';dt57='numaker-dt57';lighting='numaker-control'}
$knownTargets=@($configured|ForEach-Object {
    @{board=$_.board;port=$_.port;debugger=$_.debugger;parent=$_.parent;firmware=('build/'+$images[$_.board]+'-oled.bin')}
})
$targets = $knownTargets | Where-Object { $Board -eq 'all' -or $_.board -eq $Board }
if ($NoLogo) { $targets.firmware = 'build/numaker-dt57.bin' }
if ($HeaderLink) { $targets.firmware = $targets.firmware.Replace('.bin','-link.bin') }
if ($StandaloneDemo) { $targets.firmware = $targets.firmware.Replace('-oled.bin','-link-ring-demo.bin') }
if ($FixedDimmer -ge 0) { $targets.firmware = $targets.firmware.Replace('.bin',"-fixed$FixedDimmer.bin") }
if ($PB5Display) { $targets.firmware = $targets.firmware.Replace('.bin','-pb5.bin') }

function Write-IsolatedBoard($target, $port) {
    $parent = (Get-PnpDeviceProperty -InstanceId $port.PNPDeviceID -KeyName DEVPKEY_Device_Parent).Data
    if ($parent -ne $target.parent) { throw "$($target.port) belongs to an unexpected USB device." }
    $backup = Join-Path $root ("backup\boards\" + $target.debugger + "\manifest.json")
    if (-not (Test-Path -LiteralPath $backup)) { throw "No backup exists for $($target.board)." }
    $interfaces = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'USB\VID_0416&PID_5200&MI_03\*' -and $_.Status -eq 'OK'
    } | ForEach-Object {
        @{instance=$_.InstanceId;parent=(Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Parent).Data}
    })
    $selected = @($interfaces | Where-Object { $_.parent -eq $target.parent })
    if ($selected.Count -ne 1) { throw "No unique programming interface for $($target.board)." }
    if (@($interfaces | Where-Object { $_.parent -notin $knownTargets.parent }).Count) {
        throw 'An unexpected Nu-Link programming interface is connected.'
    }
    $flash = @{Firmware=$target.firmware;ExpectedUsbInstance=$target.parent;ExpectedDebugInterface=$selected[0].instance}
    if ($target.debugger -eq '0x00000000') { $flash.IsolatedZeroId = $true }
    else { $flash.IsolatedUsbTarget = $true }
    if ($HeaderLink -or $StandaloneDemo) {
        $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
        $flash.Snapshot = "build/header-link-before-$($target.board)-$stamp.bin"
    }
    if ($ResetOnly) { $flash.ResetOnly = $true }
    $disabled = @()
    $restoreErrors = @()
    $writeError = $null
    try {
        foreach ($interface in $interfaces) {
            if ($interface.instance -eq $selected[0].instance) { continue }
            $disabled += $interface.instance
            Disable-PnpDevice -InstanceId $interface.instance -Confirm:$false
        }
        & (Join-Path $PSScriptRoot 'flash.ps1') @flash
    } catch {
        $writeError = $_
    } finally {
        foreach ($instance in $disabled) {
            for ($attempt = 1; $attempt -le 3; $attempt++) {
                try {
                    Enable-PnpDevice -InstanceId $instance -Confirm:$false
                    break
                } catch {
                    if ($attempt -eq 3) { $restoreErrors += $_.Exception.Message }
                    else { Start-Sleep -Seconds 1 }
                }
            }
        }
    }
    if ($restoreErrors.Count) {
        $message = "Restoring USB programming interfaces failed. $($restoreErrors -join ' ')"
        if (-not $writeError) { throw $message }
        Write-Warning $message
    }
    if ($writeError) { throw $writeError }
}

$result = @{started_utc=[DateTime]::UtcNow.ToString('o');boards=@()}
$failed = $false
Start-Transcript -Path (Join-Path $root 'build\oled-flash.log') -Append | Out-Null
try {
    foreach ($target in $targets) {
        $entry = @{board=$target.board;port=$target.port;firmware=$target.firmware;verified=$false}
        try {
            $ports = @(Get-CimInstance Win32_SerialPort | Where-Object {
                $_.DeviceID -eq $target.port -and $_.PNPDeviceID -like 'USB\VID_0416&PID_5200*'
            })
            if ($ports.Count -ne 1) { throw "$($target.port) is unavailable or is not a Nu-Link port." }
            Write-IsolatedBoard $target $ports[0]
            if ($ResetOnly) { $entry.reset = $true }
            else { $entry.verified = $true }
        } catch {
            $entry.error = $_.Exception.Message
            Write-Warning "$($target.board): $($entry.error)"
            $failed = $true
        }
        $result.boards += $entry
        $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $resultFile
    }
} finally {
    $result.finished_utc = [DateTime]::UtcNow.ToString('o')
    $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $resultFile
    Stop-Transcript | Out-Null
}
if ($failed) { exit 1 }
