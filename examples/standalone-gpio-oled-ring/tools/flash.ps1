param([switch]$Restore, [string]$NuLinkId = "", [string]$Firmware = "", [switch]$IsolatedZeroId,
      [switch]$IsolatedUsbTarget, [string]$ExpectedUsbInstance = "", [string]$ExpectedDebugInterface = "",
      [switch]$ResetOnly, [string]$Snapshot = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$tool = "C:\Program Files (x86)\Nuvoton Tools\NuLink Command Tool\NuLink.exe"
if ($Restore -and $Firmware) { throw "Choose restoration or a firmware file." }
if ($ResetOnly -and -not $IsolatedUsbTarget) { throw 'Reset-only mode requires an isolated USB target.' }
if ($IsolatedZeroId -and $NuLinkId) { throw "Choose isolated zero-ID mode or an explicit debugger ID." }
if ($IsolatedUsbTarget -and ($IsolatedZeroId -or $NuLinkId -or $Restore -or -not $Firmware -or -not $ExpectedUsbInstance -or -not $ExpectedDebugInterface)) {
    throw 'Isolated USB programming requires an explicit image and one expected programming interface.'
}
if ($ExpectedUsbInstance -and -not ($IsolatedZeroId -or $IsolatedUsbTarget)) { throw "ExpectedUsbInstance requires an isolated target." }
if ($ExpectedDebugInterface -and (-not ($IsolatedZeroId -or $IsolatedUsbTarget) -or -not $ExpectedUsbInstance)) { throw "Debugger-interface isolation requires an expected USB identity." }
function Assert-DebugIsolation {
    if (-not $ExpectedDebugInterface) { return }
    $enabled = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'USB\VID_0416&PID_5200&MI_03\*' -and $_.Status -eq 'OK'
    })
    if ($enabled.Count -ne 1 -or $enabled[0].InstanceId -ne $ExpectedDebugInterface) {
        throw 'The target is not the sole enabled Nu-Link HID interface; flash cancelled.'
    }
    $parent = (Get-PnpDeviceProperty -InstanceId $enabled[0].InstanceId -KeyName DEVPKEY_Device_Parent).Data
    if ($parent -ne $ExpectedUsbInstance) { throw 'Enabled debugger does not belong to the expected USB target.' }
}
if ($NuLinkId -and $NuLinkId -notmatch '^0x[0-9A-Fa-f]{8}$') { throw "An explicit nonzero Nu-Link ID is required." }
if ($NuLinkId -eq '0x00000000') { throw "Cannot program an unidentified debugger." }
if (-not $NuLinkId) {
    if ($ExpectedDebugInterface) {
        Assert-DebugIsolation
        $isolatedInstance = $ExpectedUsbInstance
    } else {
    $boards = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_0416&PID_5200\*' })
    if ($boards.Count -ne 1) { throw "Specify -NuLinkId when exactly one board is not connected." }
    $isolatedInstance = $boards[0].InstanceId
    if ($ExpectedUsbInstance -and $isolatedInstance -ne $ExpectedUsbInstance) {
        throw "The connected board is not the expected sensor USB device; flash cancelled."
    }
    }
} else {
    $backup = Join-Path $root ("backup\boards\"+$NuLinkId.ToLower()+"\manifest.json")
    if (-not (Test-Path $backup)) { throw "Back up this board before programming: $NuLinkId" }
}
if ($IsolatedZeroId) {
    $backup = Join-Path $root "backup\boards\0x00000000\manifest.json"
    if (-not (Test-Path $backup)) { throw "Back up the isolated zero-ID board before programming." }
}
$selection = @()
if ($NuLinkId) { $selection = @($NuLinkId) }
$source = if ($Restore -and $IsolatedZeroId) {
    Join-Path $root "backup\boards\0x00000000\aprom.bin"
} elseif ($Restore -and $NuLinkId) {
    Join-Path $root ("backup\boards\"+$NuLinkId.ToLower()+"\aprom.bin")
} elseif ($Restore) {
    Join-Path $root "backup\original-aprom.bin"
} elseif ($Firmware) {
    if ([System.IO.Path]::IsPathRooted($Firmware)) { $Firmware } else { Join-Path $root $Firmware }
} else {
    Join-Path $root "build\numaker-control.bin"
}
if (-not (Test-Path $source)) { throw "Missing firmware: $source" }
$temporary = Join-Path ([System.IO.Path]::GetTempPath()) ("numkr-" + [guid]::NewGuid() + ".bin")
Copy-Item -LiteralPath $source -Destination $temporary
Push-Location (Split-Path $tool)
$identified = $false
function Invoke-NuLink([string]$operation, [string[]]$arguments, [int]$timeoutMs) {
    $outputFile = "$temporary.output.txt"
    $errorFile = "$temporary.errors.txt"
    try {
        $process = Start-Process -FilePath $tool -ArgumentList ($selection + $arguments) -WorkingDirectory (Split-Path $tool) -NoNewWindow -PassThru -RedirectStandardOutput $outputFile -RedirectStandardError $errorFile
        $null = $process.Handle
        if (-not $process.WaitForExit($timeoutMs)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Nu-Link $operation timed out."
        }
        $output = Get-Content -LiteralPath $outputFile -Raw
        $errors = Get-Content -LiteralPath $errorFile -Raw
        if ($process.ExitCode -ne 0) { throw "Nu-Link $operation failed (exit $($process.ExitCode)). $output $errors" }
        return $output
    } finally {
        Remove-Item -LiteralPath $outputFile,$errorFile -ErrorAction SilentlyContinue
    }
}
try {
    Assert-DebugIsolation
    $part = Invoke-NuLink 'identification before erase' @('-p') 20000
    Write-Output $part
    if ($part -notmatch '>>> NDA102SD2\s') {
        throw 'Expected an NDA102SD2; flash cancelled before erase.'
    }
    # Default single-adapter mode omits the ID banner. If a banner is
    # present, reject a nonzero adapter; otherwise the USB/HID guard pins it.
    if ($IsolatedZeroId -and $part -match 'NuLink 0x[0-9a-fA-F]+' -and $part -notmatch 'NuLink 0x00000000\b') {
        throw 'Expected the zero-ID sensor debugger; flash cancelled before erase.'
    }
    $identified = $true
    if ($ResetOnly) {
        Assert-DebugIsolation
        Invoke-NuLink 'target reset' @('-reset') 15000
        return
    }
    if ($Snapshot) {
        $snapshotPath = if ([System.IO.Path]::IsPathRooted($Snapshot)) { $Snapshot } else { Join-Path $root $Snapshot }
        if (Test-Path -LiteralPath $snapshotPath) { throw 'Snapshot already exists; refusing to overwrite it.' }
        $snapshotTemporary = "$temporary.previous.bin"
        try {
            Invoke-NuLink 'current APROM backup' @('-r','APROM',('"'+$snapshotTemporary+'"')) 45000
            if (-not (Test-Path -LiteralPath $snapshotTemporary)) { throw 'Current APROM backup was not created.' }
            Copy-Item -LiteralPath $snapshotTemporary -Destination $snapshotPath
            Write-Output "Saved current APROM: $snapshotPath"
        } finally { Remove-Item -LiteralPath $snapshotTemporary -ErrorAction SilentlyContinue }
    }
    Write-Output "Programming $source"
    Write-Output (Get-FileHash -LiteralPath $source -Algorithm SHA256 | Format-List | Out-String)
    if (-not $NuLinkId -and -not $ExpectedDebugInterface) {
        $current = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_0416&PID_5200\*' })
        if ($current.Count -ne 1 -or $current[0].InstanceId -ne $isolatedInstance) { throw "Connected board changed; flash cancelled." }
    }
    Assert-DebugIsolation
    Write-Output 'Erasing APROM'
    Invoke-NuLink 'APROM erase' @('-e','APROM') 45000
    Assert-DebugIsolation
    Write-Output 'Writing APROM'
    Invoke-NuLink 'APROM write' @('-w','APROM',('"'+$temporary+'"')) 45000
    Assert-DebugIsolation
    Write-Output 'Verifying APROM'
    Invoke-NuLink 'APROM verification' @('-v','APROM',('"'+$temporary+'"')) 45000
    Assert-DebugIsolation
    Invoke-NuLink 'target reset' @('-reset') 15000
} finally {
    $canDisconnect = $true
    try { Assert-DebugIsolation } catch { $canDisconnect = $false }
    if ($canDisconnect -and $identified) {
        try { Invoke-NuLink 'target disconnect' @('-disconnect') 10000 }
        catch { Write-Warning $_.Exception.Message }
    }
    Pop-Location
    Remove-Item -LiteralPath $temporary -ErrorAction SilentlyContinue
}
