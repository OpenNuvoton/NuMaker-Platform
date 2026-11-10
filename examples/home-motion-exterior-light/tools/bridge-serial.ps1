param([string]$Port = "COM3")
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
try {
    $inputLine = [Console]::In.ReadLine()
    if (-not $inputLine -or $inputLine.Length -gt 8192) { throw "Invalid bridge request." }
    $request = $inputLine | ConvertFrom-Json
    if (-not $request.commands -or $request.commands.Count -gt 64) {
        throw "Expected one to sixty-four board commands."
    }
    $outputLines = @(& (Join-Path $PSScriptRoot "control.ps1") -Port $Port -CommandText $request.commands)
    $replies = @($outputLines | Where-Object { $_ -is [string] -and $_.StartsWith("OK ") })
    @{ok=$true; responses=$replies} | ConvertTo-Json -Compress
} catch {
    @{ok=$false; error=$_.Exception.Message} | ConvertTo-Json -Compress
    exit 1
}
