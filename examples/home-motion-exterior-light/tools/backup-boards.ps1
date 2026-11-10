param([string[]]$NuLinkIds = @(), [switch]$IsolatedZeroId)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$tool = "C:\Program Files (x86)\Nuvoton Tools\NuLink Command Tool\NuLink.exe"
if ($IsolatedZeroId) {
    $boards = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\VID_0416&PID_5200\*' })
    if ($boards.Count -ne 1) { throw "Zero-ID backup requires exactly one connected board." }
    $NuLinkIds = @("0x00000000")
}
if (-not $NuLinkIds.Count) { throw 'Specify the board debugger ID with -NuLinkIds.' }
$staging = Join-Path ([System.IO.Path]::GetTempPath()) ("numkr-backup-"+[guid]::NewGuid())
New-Item -ItemType Directory -Path $staging | Out-Null
Push-Location (Split-Path $tool)
try {
    foreach ($id in $NuLinkIds) {
        if ($id -notmatch '^0x[0-9A-Fa-f]{8}$' -or ($id -eq '0x00000000' -and -not $IsolatedZeroId)) { throw "Explicit nonzero debugger ID required." }
        $selection = @()
        if (-not $IsolatedZeroId) { $selection = @($id) }
        $destination = Join-Path $root ("backup\boards\"+$id.ToLower())
        if (Test-Path (Join-Path $destination "manifest.json")) {
            Write-Output "Existing backup preserved: $id"
            continue
        }
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        try {
            $part = & $tool @selection -p | Out-String
            if ($LASTEXITCODE -ne 0) { throw "Chip identification failed for $id" }
            if ($part -notmatch '>>> NDA102SD2\s') { throw "Expected an NDA102SD2." }
            Write-Output $part
            Set-Content -LiteralPath (Join-Path $destination "part.txt") -Value $part
            foreach ($cfg in @("CFG0","CFG1")) {
                $output = & $tool @selection -r $cfg | Out-String
                if ($LASTEXITCODE -ne 0) { throw "Configuration read failed for $id" }
                Set-Content -LiteralPath (Join-Path $destination ($cfg.ToLower()+".txt")) -Value $output
            }
            $hashes = @{}
            foreach ($section in @("APROM","LDROM")) {
                $file = Join-Path $staging ($id+"-"+$section+".bin")
                & $tool @selection -r $section $file
                if ($LASTEXITCODE -ne 0 -or -not (Test-Path $file)) { throw "Firmware backup failed for $id" }
                $target = Join-Path $destination ($section.ToLower()+".bin")
                Copy-Item -LiteralPath $file -Destination $target
                $hashes[$section.ToLower()] = @{bytes=(Get-Item $target).Length; sha256=(Get-FileHash $target -Algorithm SHA256).Hash.ToLower()}
            }
            @{debugger_id=$id; captured_utc=[DateTime]::UtcNow.ToString("o"); sections=$hashes} |
                ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $destination "manifest.json")
            Write-Output "Backed up $id to $destination"
        } finally { & $tool @selection -disconnect }
    }
} finally {
    Pop-Location
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}
