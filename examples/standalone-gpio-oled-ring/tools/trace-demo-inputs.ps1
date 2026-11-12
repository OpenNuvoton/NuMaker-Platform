param([int]$Seconds=60,[string]$SensorPort='COM11',[string]$RelayPort='COM5',[string]$LightingPort='COM3')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$path=Join-Path $root 'build\demo-input-trace.jsonl'
$summary=Join-Path $root 'build\demo-input-trace.json'
$ports=@{};$report=@{started_utc=[DateTime]::UtcNow.ToString('o');mode='read-only';samples=0;passed=$false}
function Read-Demo([string]$name){
    $s=$ports[$name];$s.WriteLine('DEMO STATUS')
    for($i=0;$i -lt 8;$i++){
        $line=$s.ReadLine().Trim()
        if($line.StartsWith('ERR ')){throw "$name $line"}
        if($line.StartsWith('OK DEMO ')){
            $v=@{port=$name;utc=[DateTime]::UtcNow.ToString('o')}
            foreach($m in [regex]::Matches($line,'([A-Z][A-Z0-9_]*) (\d+)')){$v[$m.Groups[1].Value]=[long]$m.Groups[2].Value}
            return $v
        }
    }
    throw "$name did not return a demo status"
}
try{
    [System.IO.File]::WriteAllText($path,'')
    foreach($name in @($SensorPort,$RelayPort,$LightingPort)){
        Write-Output "Opening $name for read-only timing trace"
        $s=[System.IO.Ports.SerialPort]::new($name,115200,'None',8,'One')
        $s.NewLine="`n";$s.ReadTimeout=2500;$s.WriteTimeout=2500;$s.DtrEnable=$false;$s.RtsEnable=$false
        $ports[$name]=$s;$s.Open();$s.DiscardInBuffer()
    }
    $clock=[Diagnostics.Stopwatch]::StartNew();$nextSlow=0
    while($clock.Elapsed.TotalSeconds -lt $Seconds){
        $names=if($clock.ElapsedMilliseconds -ge $nextSlow){$nextSlow=$clock.ElapsedMilliseconds+1000;@($SensorPort,$RelayPort,$LightingPort)}else{@($SensorPort)}
        foreach($name in $names){
            $v=Read-Demo $name;$v.trace_ms=$clock.ElapsedMilliseconds
            [System.IO.File]::AppendAllText($path,(($v|ConvertTo-Json -Compress)+[Environment]::NewLine))
            $report.samples++
        }
        Start-Sleep -Milliseconds 150
    }
    $report.passed=$true
}catch{$report.error=$_.Exception.Message;Write-Warning $report.error}
finally{
    foreach($s in $ports.Values){if($s.IsOpen){$s.Close()};$s.Dispose()}
    $report.finished_utc=[DateTime]::UtcNow.ToString('o')
    [System.IO.File]::WriteAllText($summary,($report|ConvertTo-Json -Depth 5))
    $report|ConvertTo-Json -Compress
}
if(-not $report.passed){exit 1}
