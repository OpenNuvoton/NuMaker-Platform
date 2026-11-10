param([switch]$Restore, [string]$NuLinkId = "", [string]$Firmware = "", [switch]$IsolatedZeroId, [string]$ExpectedUsbInstance = "", [string]$ExpectedDebugInterface = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$tool = "C:\Program Files (x86)\Nuvoton Tools\NuLink Command Tool\NuLink.exe"
if ($Restore -and $Firmware) { throw "Choose restoration or a firmware file." }
if ($IsolatedZeroId -and $NuLinkId) { throw "Choose isolated zero-ID mode or an explicit debugger ID." }
if ($ExpectedUsbInstance -and -not $IsolatedZeroId) { throw "ExpectedUsbInstance requires isolated zero-ID mode." }
if ($ExpectedDebugInterface -and (-not $IsolatedZeroId -or -not $ExpectedUsbInstance)) { throw "Debugger-interface isolation requires an expected sensor USB identity." }
function Assert-DebugIsolation {
    if (-not $ExpectedDebugInterface) { return }
    $enabled = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'USB\VID_0416&PID_5200&MI_03\*' -and $_.Status -eq 'OK'
    })
    if ($enabled.Count -ne 1 -or $enabled[0].InstanceId -ne $ExpectedDebugInterface) {
        throw 'The sensor is not the sole enabled Nu-Link HID interface; flash cancelled.'
    }
    $parent = (Get-PnpDeviceProperty -InstanceId $enabled[0].InstanceId -KeyName DEVPKEY_Device_Parent).Data
    if ($parent -ne $ExpectedUsbInstance) { throw 'Enabled debugger does not belong to the expected sensor.' }
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
try {
    Assert-DebugIsolation
    $part = & $tool @selection -p | Out-String
    Write-Output $part
    if ($LASTEXITCODE -ne 0 -or $part -notmatch '>>> NDA102SD2\s') {
        throw "Expected an NDA102SD2; flash cancelled."
    }
    # Default single-adapter mode omits the ID banner. If a banner is
    # present, reject a nonzero adapter; otherwise the USB/HID guard pins it.
    if ($IsolatedZeroId -and $part -match 'NuLink 0x[0-9a-fA-F]+' -and $part -notmatch 'NuLink 0x00000000\b') {
        throw 'Expected the zero-ID sensor debugger; flash cancelled before erase.'
    }
    Write-Output "Programming $source"
    Write-Output (Get-FileHash -LiteralPath $source -Algorithm SHA256 | Format-List | Out-String)
    if (-not $NuLinkId -and -not $ExpectedDebugInterface) {
        $current = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_0416&PID_5200\*' })
        if ($current.Count -ne 1 -or $current[0].InstanceId -ne $isolatedInstance) { throw "Connected board changed; flash cancelled." }
    }
    Assert-DebugIsolation
    & $tool @selection -e APROM
    if ($LASTEXITCODE -ne 0) { throw "Application erase failed." }
    Assert-DebugIsolation
    & $tool @selection -w APROM $temporary
    if ($LASTEXITCODE -ne 0) { throw "Programming failed." }
    Assert-DebugIsolation
    & $tool @selection -v APROM $temporary
    if ($LASTEXITCODE -ne 0) { throw "Verification failed." }
    Assert-DebugIsolation
    & $tool @selection -reset
    if ($LASTEXITCODE -ne 0) { throw "Reset failed." }
} finally {
    $canDisconnect = $true
    try { Assert-DebugIsolation } catch { $canDisconnect = $false }
    if ($canDisconnect) { & $tool @selection -disconnect }
    Pop-Location
    Remove-Item -LiteralPath $temporary -ErrorAction SilentlyContinue
}
