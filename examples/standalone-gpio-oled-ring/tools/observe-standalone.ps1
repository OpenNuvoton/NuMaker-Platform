param([string]$SensorPort='COM11',[string]$RelayPort='COM5',[string]$LightingPort='COM3')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$ports=@{}
$report=@{started_utc=[DateTime]::UtcNow.ToString('o');usb_mode='read-only';samples=@();passed=$false}
function Send-Read([string]$name,[string]$command) {
    if($command -notin @('STATUS','DEMO STATUS','DEMO FRAME','LINK STATUS','RING STATUS')){throw 'Observer permits read-only commands only.'}
    $s=$ports[$name];$s.WriteLine($command)
    for($i=0;$i -lt 15;$i++) {
        $reply=$s.ReadLine().Trim()
        if($reply.StartsWith('ERR ')){throw "$name $reply"}
        if($reply.StartsWith('OK ')){return $reply}
    }
    throw 'Missing readback'
}
function Fields([string]$reply) {
    $v=@{};foreach($m in [regex]::Matches($reply,'([A-Z][A-Z0-9_]*) (\d+)')){$v[$m.Groups[1].Value]=[long]$m.Groups[2].Value};return $v
}
try {
    foreach($name in @($SensorPort,$RelayPort,$LightingPort)) {
        $report.stage="Opening $name for read-only diagnostics";Write-Output $report.stage
        [System.IO.File]::WriteAllText((Join-Path $root 'build\standalone-startup-verification.json'),($report|ConvertTo-Json -Depth 10))
        $s=[System.IO.Ports.SerialPort]::new($name,115200,'None',8,'One')
        $s.NewLine="`n";$s.ReadTimeout=2500;$s.WriteTimeout=2500;$s.DtrEnable=$false;$s.RtsEnable=$false
        $ports[$name]=$s;$s.Open();Start-Sleep -Milliseconds 150;$s.DiscardInBuffer()
    }
    $deadline=[DateTime]::UtcNow.AddSeconds(50)
    do {
        $allReady=$true;$sample=@{utc=[DateTime]::UtcNow.ToString('o');boards=@{}}
        foreach($name in @($SensorPort,$RelayPort,$LightingPort)) {
            $v=Fields(Send-Read $name 'DEMO STATUS');$sample.boards[$name]=$v
            if($v.WARMUP_MS -gt 0 -or $v.READY_MASK -ne 7){$allReady=$false}
            if($v.WARMUP_MS -gt 0 -and ($v.RELAY -ne 0 -or $v.STROBE_MS -gt 0)){throw "$name fired during warmup"}
        }
        $report.samples+=$sample
        if(-not $allReady){Start-Sleep -Milliseconds 1000}
    }while(-not $allReady -and [DateTime]::UtcNow -lt $deadline)
    if(-not $allReady){throw 'All-board readiness was not reached after warmup.'}
    $report.before=$sample.boards
    foreach($s in $ports.Values){$s.Close();$s.Dispose()};$ports=@{}
    Write-Output 'Warmup complete, all three ready. No serial ports open for 12 seconds.'
    Start-Sleep -Seconds 12
    $report.after=@{}
    foreach($name in @($SensorPort,$RelayPort,$LightingPort)) {
        $s=[System.IO.Ports.SerialPort]::new($name,115200,'None',8,'One')
        $s.NewLine="`n";$s.ReadTimeout=2500;$s.WriteTimeout=2500;$s.DtrEnable=$false;$s.RtsEnable=$false
        $ports[$name]=$s;$s.Open();Start-Sleep -Milliseconds 150;$s.DiscardInBuffer()
        $v=Fields(Send-Read $name 'DEMO STATUS')
        $v.link=Send-Read $name 'LINK STATUS';$v.ring=Send-Read $name 'RING STATUS'
        $v.outputs=Send-Read $name 'STATUS';$report.after[$name]=$v
        if($v.RUNNING -ne 1 -or $v.READY_MASK -ne 7 -or $v.TX_PACKETS-$report.before[$name].TX_PACKETS -lt 5 -or
           $v.RX_PACKETS-$report.before[$name].RX_PACKETS -lt 5){throw "$name did not continue autonomously"}
        if($name -eq $SensorPort -and ($v.PIR_SAMPLES-$report.before[$name].PIR_SAMPLES -lt 200 -or
           $v.LUX_SAMPLES-$report.before[$name].LUX_SAMPLES -lt 50)) {
            throw 'Independent PIR and ambient acquisition did not continue with serial ports closed.'
        }
        $frame=Send-Read $name 'DEMO FRAME'
        if($frame -notmatch '^OK DEMO FRAME ([0-9A-F]{2048})$'){throw 'Invalid OLED frame'}
        $hex=$Matches[1];$bytes=[byte[]]::new(1024)
        for($i=0;$i -lt 1024;$i++){$bytes[$i]=[Convert]::ToByte($hex.Substring(2*$i,2),16)}
        [System.IO.File]::WriteAllBytes((Join-Path $root "build\standalone-oled-$name.bin"),$bytes)
        $v|ConvertTo-Json -Depth 5 -Compress
    }
    $report.passed=$true
}catch{$report.error=$_.Exception.Message;Write-Warning $report.error}
finally {
    foreach($s in $ports.Values){if($s.IsOpen){$s.Close()};$s.Dispose()}
    $report.finished_utc=[DateTime]::UtcNow.ToString('o')
    [System.IO.File]::WriteAllText((Join-Path $root 'build\standalone-startup-verification.json'),($report|ConvertTo-Json -Depth 10))
}
if(-not $report.passed){exit 1}
