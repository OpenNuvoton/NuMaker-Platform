$ErrorActionPreference = 'Stop'
$Host.UI.RawUI.WindowTitle = 'NuMaker sensor live readings'
$stateFile = Join-Path (Split-Path $PSScriptRoot) 'build\sensor-live-latest.json'
Clear-Host
while ($true) {
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('NUMAKER AMBIENT LIGHT / PIR - LIVE READINGS')
    try {
        $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
        $stream = [System.IO.File]::Open($stateFile, 'Open', 'Read', $share)
        try {
            $reader = [System.IO.StreamReader]::new($stream)
            try { $state = $reader.ReadToEnd() | ConvertFrom-Json }
            finally { $reader.Dispose() }
        } finally { $stream.Dispose() }
        $updated = [DateTime]::Parse($state.updated_utc).ToUniversalTime()
        $age = ([DateTime]::UtcNow - $updated).TotalSeconds
        $stale = if ($age -gt 1) { '  STALE / WAITING FOR NEW DATA' } else { '' }
        $lines.Add(("{0} | {1} | Data age: {2:N2} s{3}" -f $state.port,$state.status,$age,$stale))
        if ($state.status -eq 'online') {
            if ($state.ambient -and $state.ambient.valid) {
                $saturation = if ($state.ambient.saturated) { ' [SATURATED]' } else { '' }
                $lines.Add(("AMBIENT LIGHT: {0:N2} lux{1}   [TSL25721 at 0x39]" -f $state.ambient.lux,$saturation))
                $lines.Add(("Visible+IR C0: {0}   IR C1: {1}   Integration: 51.87 ms / Gain: 1x" -f $state.ambient.channel0,$state.ambient.channel1))
            } else {
                $lines.Add('Ambient sensor: waiting for valid I2C readings.')
                $lines.Add([string]$state.ambient_error)
            }
            $lines.Add('')
            $pin = if ($state.pir_raw) { 'HIGH' } else { 'LOW' }
            $lines.Add(("PB5 ADC: {0}  |  Digital: {1}  |  Changes: {2}" -f $state.analog.PB5,$pin,$state.transitions))
            $lines.Add('')
            foreach ($channel in @(14,15,2,3,4,5,6,7,8,9,10,11)) {
                $key = "PB$channel"
                $property = $state.analog.PSObject.Properties[$key]
                if ($null -ne $property) {
                    $value = [int]$property.Value
                    $bar = '#' * [int][Math]::Round($value*36/4095)
                    $lines.Add(("{0,-4} {1,4}  {2,-36}" -f $key,$value,$bar))
                } else { $lines.Add(("{0,-4} unavailable" -f $key)) }
            }
            $lines.Add('')
            $lines.Add(("Samples: {0}   Latest gap: {1:N0} ms   Largest gap: {2:N0} ms" -f $state.poll_count,$state.poll_interval_ms,$state.max_poll_gap_ms))
            if ($state.oled_splash) { $lines.Add(("OLED: {0}. Sensing is active." -f $state.oled_page)) }
            else { $lines.Add('OLED updates: ON. Closing this view keeps sensing active.') }
        } else {
            $lines.Add('Waiting for the sensor USB connection; reconnect is automatic.')
            $lines.Add([string]$state.error)
        }
    } catch {
        $lines.Add('Waiting for the next sensor sample...')
    }
    while ($lines.Count -lt 22) { $lines.Add('') }
    try {
        $width = [Math]::Max(20, [Math]::Min(100,[Console]::WindowWidth-1))
        $frame = ($lines | ForEach-Object {
            if ($_.Length -gt $width) { $_.Substring(0,$width) } else { $_.PadRight($width) }
        }) -join "`r`n"
        [Console]::SetCursorPosition(0,0)
        [Console]::Write($frame)
    } catch { }
    Start-Sleep -Milliseconds 100
}
