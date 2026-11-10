param([string]$PreferredPort = 'COM9', [switch]$Background)
$ErrorActionPreference = 'Stop'
if (-not $Background) { $Host.UI.RawUI.WindowTitle = 'NuMaker sensor live monitor' }
$root = Split-Path $PSScriptRoot
$stateFile = Join-Path $root 'build\sensor-live-latest.json'
$stateTemporary = "$stateFile.$PID.tmp"
$splashFile = Join-Path $root 'build\oled-splash.bin'
$descriptionFile = Join-Path $root 'build\oled-sensor-description.bin'
$script:serial = $null
$script:oledRows = @{}
$script:splashStamp = $null
$script:oledPage = $null
$script:lastPin = $null
$script:edges = 0
$script:extraSupported = $true
$script:tailSupported = $true
. (Join-Path $PSScriptRoot 'tsl2572.ps1')
$selectionPausesDisabled = $false
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SensorMonitorConsole {
    [DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int n);
    [DllImport("kernel32.dll")] public static extern bool GetConsoleMode(IntPtr h, out uint mode);
    [DllImport("kernel32.dll")] public static extern bool SetConsoleMode(IntPtr h, uint mode);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool MoveFileEx(string source, string destination, uint flags);
}
'@
try {
    $consoleInput = [SensorMonitorConsole]::GetStdHandle(-10)
    [uint32]$consoleMode = 0
    if ([SensorMonitorConsole]::GetConsoleMode($consoleInput, [ref]$consoleMode)) {
        $selectionPausesDisabled = [SensorMonitorConsole]::SetConsoleMode($consoleInput, (($consoleMode -bor 0x80) -band (-bnot 0x40)))
    }
} catch { }

function Save-State($value) {
    $value['updated_utc'] = [DateTime]::UtcNow.ToString('o')
    $value['windows_pid'] = $PID
    $json = $value | ConvertTo-Json -Depth 5
    [System.IO.File]::WriteAllText($stateTemporary, $json)
    # Replace in place so the WSL controller only sees complete samples.
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        if ([SensorMonitorConsole]::MoveFileEx($stateTemporary, $stateFile, 1)) { return }
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($errorCode -notin @(5, 32, 33)) { throw [ComponentModel.Win32Exception]::new($errorCode) }
        Start-Sleep -Milliseconds 5
    }
    # The WSL share can deny replacement while another process reads the file.
    [System.IO.File]::WriteAllText($stateFile, $json)
}

function Send-Board([string]$command) {
    $script:serial.WriteLine($command)
    for ($attempt = 0; $attempt -lt 12; $attempt++) {
        $line = $script:serial.ReadLine().Trim()
        if ($line.StartsWith('ERR ')) { throw $line }
        if ($line.StartsWith('OK ')) { return $line }
    }
    throw 'No board acknowledgement'
}

function Set-OledRow([int]$row, [string]$text) {
    if ($null -ne $script:splashStamp) { return }
    if ($text.Length -gt 21) { $text = $text.Substring(0, 21) }
    if ($script:oledRows[$row] -ne $text) {
        $null = Send-Board "TEXT $row $text"
        $script:oledRows[$row] = $text
    }
}

function Update-OledSplash($ambient) {
    $file = Get-Item -LiteralPath $splashFile -ErrorAction SilentlyContinue
    if ($null -eq $file) {
        if ($null -eq $script:splashStamp) { return }
        $null = Send-Board 'CLEAR'
        $script:splashStamp = $null
        $script:oledPage = $null
        $script:oledRows = @{}
        if ($ambient) { Set-OledRow 0 'AMBIENT TSL25721' }
        else { Set-OledRow 0 'CONTINUOUS INPUTS' }
        Set-OledRow 7 'PB5 map unverified'
        if ($null -ne $script:lastPin) {
            $pin = if ($script:lastPin) { 'HIGH' } else { 'LOW' }
            $row = if ($ambient) { 3 } else { 1 }
            Set-OledRow $row ("PB5 {0} Edges {1}" -f $pin,$script:edges)
        }
        return
    }
    $page = 'logo'
    if ([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() % 10 -ge 4) {
        $description = Get-Item -LiteralPath $descriptionFile -ErrorAction SilentlyContinue
        if ($null -ne $description -and $description.Length -eq 1024) {
            $file = $description
            $page = 'description'
        }
    }
    if ($file.Length -ne 1024 -or ($file.LastWriteTimeUtc -eq $script:splashStamp -and $page -eq $script:oledPage)) { return }
    $frame = [System.IO.File]::ReadAllBytes($file.FullName)
    if ($frame.Length -ne 1024) { return }
    $null = Send-Board 'OLED CONTRAST 160'
    $null = Send-Board 'OLED INVERT 0'
    for ($offset = 0; $offset -lt $frame.Length; $offset += 48) {
        $count = [Math]::Min(48, $frame.Length - $offset)
        $hex = [BitConverter]::ToString($frame, $offset, $count).Replace('-', '')
        $null = Send-Board "BUFFER $offset $hex"
    }
    $null = Send-Board 'PRESENT'
    if ((Send-Board 'OLED ON') -notmatch ' OLED ON ADDR ') { throw 'Sensor splash OLED is unavailable' }
    $script:splashStamp = $file.LastWriteTimeUtc
    $script:oledPage = $page
}

function Read-Analog([string]$command) {
    $line = Send-Board $command
    if (-not $line.StartsWith('OK ANALOG ')) { throw 'Unexpected ADC response' }
    $result = @{}
    foreach ($match in [regex]::Matches($line, 'PB(\d+) (\d+)')) {
        $value = [int]$match.Groups[2].Value
        if ($value -gt 4095) { throw 'ADC result out of range' }
        $result['PB' + $match.Groups[1].Value] = $value
    }
    if ($result.Count -eq 0) { throw 'Empty ADC response' }
    return $result
}

try {
    while ($true) {
        $ports = @(Get-CimInstance Win32_SerialPort | Where-Object {
            $_.PNPDeviceID -like '*VID_0416&PID_5200*' -and
            $_.DeviceID -notin @('COM3', 'COM5')
        } | Select-Object -ExpandProperty DeviceID)
        $ports = @($ports | Sort-Object { if ($_ -eq $PreferredPort) { 0 } else { 1 } })
        if (-not $ports.Count) {
            if (-not $Background) {
            Clear-Host
            Write-Host 'NUMAKER SENSOR LIVE MONITOR' -ForegroundColor Cyan
            Write-Host ''
            Write-Host 'Waiting for the sensor USB connection...' -ForegroundColor Yellow
            Write-Host 'Reconnect the sensor board USB cable. This window reconnects automatically.'
            Write-Host 'The lighting and relay boards (COM3/COM5) are excluded.'
            Write-Host 'Live readings will also appear on the sensor OLED.'
            }
            Save-State @{status='waiting'; message='No sensor serial port detected'; ports=$ports}
            Start-Sleep -Seconds 2
            continue
        }
        foreach ($sensorPort in $ports) {
            try {
                $script:serial = [System.IO.Ports.SerialPort]::new($sensorPort,115200,[System.IO.Ports.Parity]::None,8,[System.IO.Ports.StopBits]::One)
                $script:serial.NewLine = "`n"
                $script:serial.ReadTimeout = 2500
                $script:serial.WriteTimeout = 2500
                $script:serial.DtrEnable = $false
                $script:serial.RtsEnable = $false
                $script:serial.Open()
                $script:serial.DiscardInBuffer()
                $board = Send-Board 'STATUS'
                if ($board -notmatch '^OK RGB ' -or $board -match ' LAMP ') {
                    throw 'This port is not the sensor diagnostic profile'
                }
                $probe = Send-Board 'OLED PROBE'
                if ($probe -notmatch 'PB0/PB1') { throw 'Expected sensor OLED on PB0/PB1' }
                $null = Send-Board 'OLED ON'
                $null = Send-Board 'CLEAR'
                $script:oledRows = @{}
                $script:splashStamp = $null
                $script:oledPage = $null
                $script:lastPin = $null
                $script:edges = 0
                $script:extraSupported = $true
                $script:tailSupported = $true
                $ambient = $null
                $ambientError = $null
                try { $ambient = Initialize-AmbientSensor } catch { $ambientError = $_.Exception.Message }
                Set-OledRow 0 'CONTINUOUS INPUTS'
                Set-OledRow 7 'PB5 map unverified'
                if ($ambient) { Set-OledRow 0 'AMBIENT TSL25721' }
                $clock = [System.Diagnostics.Stopwatch]::StartNew()
                $lastPoll = 0.0
                $lastUi = -2.0
                $lastSave = -2.0
                $lastOled = -2.0
                $lastSplash = -2.0
                $splashError = $null
                $lastAmbient = -2.0
                $oledRow = if ($ambient) { 4 } else { 2 }
                $adc = @{}
                $pollCount = 0
                $maxGapMs = 0.0
                $lastEdgeUtc = $null
                while ($true) {
                    $digital = Send-Board 'INPUTS'
                    if ($digital -notmatch '^OK INPUTS PA (\d+) PB (\d+) PC (\d+) PD (\d+) PF (\d+)$') {
                        throw 'Unexpected digital input response'
                    }
                    $portB = [int]$Matches[2]
                    $pirPin = ($portB -shr 5) -band 1
                    $now = $clock.Elapsed.TotalSeconds
                    $gapMs = ($now - $lastPoll) * 1000
                    $lastPoll = $now
                    $pollCount++
                    if ($pollCount -gt 1) { $maxGapMs = [Math]::Max($maxGapMs, $gapMs) }
                    $changed = $null -eq $script:lastPin -or $pirPin -ne $script:lastPin
                    if ($null -ne $script:lastPin -and $changed) {
                        $script:edges++
                        $lastEdgeUtc = [DateTime]::UtcNow.ToString('o')
                    }
                    $script:lastPin = $pirPin
                    $pinName = if ($pirPin) { 'HIGH' } else { 'LOW' }
                    # Update a changed digital input before slower ADC/UI work.
                    if ($changed) {
                        $motionRow = if ($ambient) { 3 } else { 1 }
                        Set-OledRow $motionRow ("PB5 {0} Edges {1}" -f $pinName,$script:edges)
                    }
                    # Read every ADC channel on every scan; no stale bank cache.
                    $bank = Read-Analog 'ANALOG'
                    foreach ($key in $bank.Keys) { $adc[$key] = $bank[$key] }
                    if ($script:extraSupported) {
                        try {
                            $bank = Read-Analog 'ANALOG EXTRA'
                            foreach ($key in $bank.Keys) { $adc[$key] = $bank[$key] }
                        } catch {
                            if ($_.Exception.Message -like 'ERR invalid command*') { $script:extraSupported = $false }
                            else { throw }
                        }
                    }
                    if ($script:tailSupported) {
                        try {
                            $bank = Read-Analog 'ANALOG TAIL'
                            foreach ($key in $bank.Keys) { $adc[$key] = $bank[$key] }
                        } catch {
                            if ($_.Exception.Message -like 'ERR invalid command*') { $script:tailSupported = $false }
                            else { throw }
                        }
                    }
                    if ($ambient -and $now - $lastAmbient -ge 0.1) {
                        try {
                            $ambient = Read-AmbientSensor $ambient
                            $ambientError = $null
                            if ($ambient.valid) {
                                $label = if ($ambient.saturated) { 'LUX SATURATED' } else { 'Lux: {0:N1}' -f $ambient.lux }
                                Set-OledRow 1 $label
                                Set-OledRow 2 ("C0 {0} IR {1}" -f $ambient.channel0,$ambient.channel1)
                            }
                        } catch { $ambientError = $_.Exception.Message }
                        $lastAmbient = $now
                    }
                    if (-not $Background -and $now - $lastUi -ge 0.1) {
                    Clear-Host
                    Write-Host "NUMAKER SENSOR LIVE  |  $sensorPort  |  $([DateTime]::Now.ToString('HH:mm:ss'))" -ForegroundColor Cyan
                    Write-Host 'CONTINUOUS INPUTS: both ADC banks read on every scan.'
                    Write-Host 'Pin mapping is being checked. ADC values are raw, not lux.' -ForegroundColor Yellow
                    Write-Host ''
                    Write-Host ("PB5 (unverified): {0,-4}    Input transitions: {1}" -f $pinName,$script:edges) -ForegroundColor Green
                    Write-Host ''
                    Write-Host 'LIGHT SENSOR MAPPING / ADC CHANNELS (0..4095)'
                    foreach ($channel in @(14,15,2,3,4,5,6,7,8,9,10,11)) {
                        $key = "PB$channel"
                        if ($adc.ContainsKey($key)) {
                            $value = $adc[$key]
                            $bar = '#' * [int][Math]::Round($value * 38 / 4095)
                            Write-Host ("{0,-4} {1,4}  {2,-38}" -f $key,$value,$bar)
                        } else { Write-Host ("{0,-4}  unavailable" -f $key) }
                    }
                    Write-Host ''
                    Write-Host ("PB digital bits: 0x{0:X4}    OLED updates: ON" -f $portB)
                    Write-Host ("Digital polls: {0}   Latest gap: {1:N0} ms   Largest gap: {2:N0} ms" -f $pollCount,$gapMs,$maxGapMs)
                    Write-Host 'Close this window to stop monitoring.'
                        $lastUi = $now
                    }
                    # Spread ADC OLED rows over successive fast polling cycles.
                    if ($now - $lastOled -ge 0.05) {
                            $first = ($oledRow - 1) * 2
                            $second = $first + 1
                            $one = if ($adc.ContainsKey("PB$first")) { [string]$adc["PB$first"] } else { '--' }
                            $two = if ($adc.ContainsKey("PB$second")) { [string]$adc["PB$second"] } else { '--' }
                            Set-OledRow $oledRow ("PB{0} {1,4} PB{2} {3,4}" -f $first,$one,$second,$two)
                        $oledRow++
                        if ($oledRow -gt 6) { $oledRow = if ($ambient) { 4 } else { 2 } }
                        if ($script:tailSupported) {
                            Set-OledRow 7 ("PB14 {0,4} PB15 {1,4}" -f $adc['PB14'],$adc['PB15'])
                        }
                        $lastOled = $now
                    }
                    if ($now - $lastSave -ge 0.03) {
                        Save-State @{status='online';port=$sensorPort;observed_pin='PB5';pir_candidate_pin='PB5';pir_raw=$pirPin;transitions=$script:edges;analog=$adc;ambient=$ambient;ambient_error=$ambientError;digital_response=$digital;oled_updates=($null -eq $script:splashStamp);oled_splash=($null -ne $script:splashStamp);oled_page=$script:oledPage;oled_splash_error=$splashError;pin_mapping_confirmed=$false;sampling_mode='continuous_full_scan';background_acquisition=[bool]$Background;adc_banks_per_scan=(2+[int]$script:tailSupported);tail_adc_supported=$script:tailSupported;poll_count=$pollCount;poll_interval_ms=[Math]::Round($gapMs,2);max_poll_gap_ms=[Math]::Round($maxGapMs,2);last_edge_utc=$lastEdgeUtc;console_selection_pauses_disabled=$selectionPausesDisabled}
                        $lastSave = $now
                    }
                    if ($now - $lastSplash -ge 0.5) {
                        try {
                            Update-OledSplash $ambient
                            $splashError = $null
                        } catch { $splashError = $_.Exception.Message }
                        $lastSplash = $now
                    }
                    Start-Sleep -Milliseconds 5
                }
            } catch {
                Save-State @{status='reconnecting';port=$sensorPort;error=$_.Exception.Message}
                if (-not $Background) { Write-Host ("Disconnected: " + $_.Exception.Message) -ForegroundColor Yellow }
            } finally {
                if ($null -ne $script:serial) {
                    $script:serial.Close()
                    $script:serial.Dispose()
                    $script:serial = $null
                }
            }
        }
        Start-Sleep -Seconds 2
    }
} finally {
    if ($null -ne $script:serial) { $script:serial.Close(); $script:serial.Dispose() }
}
