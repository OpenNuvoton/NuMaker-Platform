param([ValidateRange(0,100)][int]$Duty=60,[string]$Port='COM5')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$report=@{started_utc=[DateTime]::UtcNow.ToString('o');duty=$Duty;usb_mode='read-only';samples=@();passed=$false}
$s=$null
try{
    $s=[IO.Ports.SerialPort]::new($Port,115200,'None',8,'One')
    $s.NewLine="`n";$s.ReadTimeout=2500;$s.WriteTimeout=2500;$s.DtrEnable=$false;$s.RtsEnable=$false
    $s.Open();$s.DiscardInBuffer()
    $deadline=[DateTime]::UtcNow.AddSeconds(45);$readySamples=0
    do{
        $s.WriteLine('DEMO STATUS');$v=$null
        for($i=0;$i -lt 8;$i++){
            $line=$s.ReadLine().Trim()
            if($line.StartsWith('ERR ')){throw $line}
            if($line.StartsWith('OK DEMO ')){
                $v=@{utc=[DateTime]::UtcNow.ToString('o')}
                foreach($m in [regex]::Matches($line,'([A-Z][A-Z0-9_]*) (\d+)')){$v[$m.Groups[1].Value]=[long]$m.Groups[2].Value}
                break
            }
        }
        if($null -eq $v -or $v.NODE -ne 2 -or $v.FIXED_DIMMER -ne 1 -or $v.PWM -ne $Duty){throw 'Relay did not report the expected fixed-dimmer diagnostic firmware.'}
        if($v.WARMUP_MS -gt 0 -and $v.RELAY -ne 0){throw 'Relay fired during warmup.'}
        $report.samples+=$v
        if($v.WARMUP_MS -eq 0 -and $v.READY_MASK -eq 7){$readySamples++}else{$readySamples=0}
        if($readySamples -ge 12){break}
        Start-Sleep -Milliseconds 1000
    }while([DateTime]::UtcNow -lt $deadline)
    if($readySamples -lt 12){throw 'Fresh traffic from the full chain did not remain ready for 12 samples.'}
    $first=$report.samples[$report.samples.Count-12];$last=$report.samples[-1]
    if($last.RX_PACKETS-$first.RX_PACKETS -lt 5){throw 'Sensor packets stopped arriving.'}
    $s.WriteLine('STATUS');$report.output_status=$s.ReadLine().Trim()
    $report.passed=$true
}catch{$report.error=$_.Exception.Message;Write-Warning $report.error}
finally{
    if($null -ne $s){if($s.IsOpen){$s.Close()};$s.Dispose()}
    $report.finished_utc=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText((Join-Path $root 'build\fixed-dimmer-verification.json'),($report|ConvertTo-Json -Depth 7))
    $report|ConvertTo-Json -Depth 7 -Compress
}
if(-not $report.passed){exit 1}
