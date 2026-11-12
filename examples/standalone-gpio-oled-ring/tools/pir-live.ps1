param([switch]$Worker,[string]$Port='COM11')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$null=New-Item -ItemType Directory -Path (Join-Path $root 'build') -Force
$statePath=Join-Path $root 'build\pir-live-latest.json'

if($Worker){
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $logPath=Join-Path $root ('build\pir-live-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'.jsonl')
    $serial=$null;$level=$null;$since=0;$edges=0;$samples=0;$lastHigh=$null;$lastLow=$null
    function Save-State($state){
        $state.updated_utc=[DateTime]::UtcNow.ToString('o');$state.worker_pid=$PID
        $state.port=$Port;$state.log=$logPath
        [System.IO.File]::WriteAllText($statePath,($state|ConvertTo-Json -Depth 5 -Compress))
    }
    try{
        while($true){
            try{
                Save-State @{status='connecting'}
                $serial=[System.IO.Ports.SerialPort]::new($Port,115200,'None',8,'One')
                $serial.NewLine="`n";$serial.ReadTimeout=2000;$serial.WriteTimeout=2000
                $serial.DtrEnable=$false;$serial.RtsEnable=$false;$serial.Open();$serial.DiscardInBuffer()
                # Every request is diagnostic. The MCU retains full ownership of sensing and outputs.
                $level=$null
                while($true){
                    $serial.WriteLine('DEMO STATUS')
                    $fields=$null
                    for($attempt=0;$attempt -lt 8;$attempt++){
                        $line=$serial.ReadLine().Trim()
                        if($line.StartsWith('ERR ')){throw $line}
                        if($line.StartsWith('OK DEMO ')){
                            $fields=@{}
                            foreach($m in [regex]::Matches($line,'([A-Z][A-Z0-9_]*) (\d+)')){
                                $fields[$m.Groups[1].Value]=[long]$m.Groups[2].Value
                            }
                            break
                        }
                    }
                    if($null -eq $fields -or $fields.NODE -ne 1 -or -not $fields.ContainsKey('PIR') -or
                       -not $fields.ContainsKey('PIR_SAMPLES')){throw 'Expected sensor-node PB5 telemetry.'}
                    $now=$clock.ElapsedMilliseconds
                    $next=if(($fields.VALID -band 2) -eq 0){'INVALID'}elseif($fields.PIR -gt 1800){'HIGH'}elseif($fields.PIR -lt 100){'LOW'}else{'MID'}
                    if($next -ne $level){
                        if($null -ne $level){
                            if($level -eq 'HIGH'){$lastHigh=$now-$since}
                            if($level -eq 'LOW'){$lastLow=$now-$since}
                            $edges++
                        }
                        $level=$next;$since=$now
                    }
                    $samples++
                    $state=@{status='online';pb5=$fields.PIR;level=$level;level_ms=$now-$since;transitions=$edges;
                        last_high_ms=$lastHigh;last_low_ms=$lastLow;samples=$samples;elapsed_ms=$now;fields=$fields}
                    Save-State $state
                    [System.IO.File]::AppendAllText($logPath,(($state|ConvertTo-Json -Depth 5 -Compress)+[Environment]::NewLine))
                    Start-Sleep -Milliseconds 150
                }
            }catch{
                Save-State @{status='offline';error=$_.Exception.Message}
            }finally{
                if($null -ne $serial){if($serial.IsOpen){$serial.Close()};$serial.Dispose();$serial=$null}
            }
            Start-Sleep -Seconds 2
        }
    }finally{if($null -ne $serial){if($serial.IsOpen){$serial.Close()};$serial.Dispose()}}
    exit
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms.DataVisualization
[System.Windows.Forms.Application]::EnableVisualStyles()
$form=[System.Windows.Forms.Form]::new()
$form.Text='Live PIR / PB5 - read-only';$form.ClientSize=[Drawing.Size]::new(860,600)
$form.MinimumSize=[Drawing.Size]::new(720,500);$form.BackColor=[Drawing.Color]::FromArgb(20,24,32)
$form.ForeColor=[Drawing.Color]::White
$layout=[Windows.Forms.TableLayoutPanel]::new();$layout.Dock='Fill';$layout.ColumnCount=1;$layout.RowCount=6
foreach($height in @(32,90,40,48,0,56)){
    $style=[Windows.Forms.RowStyle]::new()
    if($height){$style.SizeType='Absolute';$style.Height=$height}else{$style.SizeType='Percent';$style.Height=100}
    $null=$layout.RowStyles.Add($style)
}
$form.Controls.Add($layout)
function Add-Label([int]$row,[float]$size){
    $label=[Windows.Forms.Label]::new();$label.Dock='Fill';$label.AutoSize=$false
    $label.Padding=[Windows.Forms.Padding]::new(16,2,4,0)
    $label.Font=[Drawing.Font]::new('Consolas',$size)
    $layout.Controls.Add($label,0,$row);return $label
}
$connection=Add-Label 0 12;$connection.Text='Connecting to sensor telemetry...'
$raw=Add-Label 1 48;$raw.Text='PB5  ----'
$timing=Add-Label 2 17;$timing.Text='Waiting for a sample'
$details=Add-Label 3 11
$footer=Add-Label 5 10
$footer.Text="Raw ADC 0..4095 | HIGH >1800 | LOW <100 | MID between thresholds`r`nUSB diagnostics only. Closing this window leaves the boards running."
$chart=[Windows.Forms.DataVisualization.Charting.Chart]::new();$chart.Dock='Fill';$chart.BackColor=$form.BackColor
$area=[Windows.Forms.DataVisualization.Charting.ChartArea]::new('PB5');$area.BackColor=$form.BackColor
$area.AxisY.Minimum=0;$area.AxisY.Maximum=4095;$area.AxisY.Title='PB5 ADC'
$area.AxisX.Title='Seconds since monitor started'
foreach($axis in @($area.AxisX,$area.AxisY)){
    $axis.LabelStyle.ForeColor=[Drawing.Color]::Silver;$axis.TitleForeColor=[Drawing.Color]::Silver
    $axis.LineColor=[Drawing.Color]::DimGray;$axis.MajorGrid.LineColor=[Drawing.Color]::FromArgb(42,48,58)
}
$chart.ChartAreas.Add($area)
$series=[Windows.Forms.DataVisualization.Charting.Series]::new('PB5')
$series.ChartType='StepLine';$series.Color=[Drawing.Color]::Cyan;$series.BorderWidth=2
$chart.Series.Add($series);$layout.Controls.Add($chart,0,4)
$script:lastSample=-1
$timer=[Windows.Forms.Timer]::new();$timer.Interval=150
$timer.Add_Tick({
    try{
        $share=[IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream=[IO.File]::Open($statePath,'Open','Read',$share)
        try{$reader=[IO.StreamReader]::new($stream);try{$state=$reader.ReadToEnd()|ConvertFrom-Json}finally{$reader.Dispose()}}
        finally{$stream.Dispose()}
        $age=([DateTime]::UtcNow-[DateTime]::Parse($state.updated_utc).ToUniversalTime()).TotalSeconds
        if($state.status -ne 'online' -or $age -gt 2){
            $connection.Text="$($state.port)  $($state.status)  $($state.error)";$connection.ForeColor=[Drawing.Color]::Orange
            $raw.Text='PB5  ----';$timing.Text='Waiting for fresh telemetry';return
        }
        $f=$state.fields
        $connection.Text=('{0}  LIVE  |  read-only  |  sample age {1:N2}s' -f $state.port,$age)
        $connection.ForeColor=[Drawing.Color]::LightGreen
        $raw.Text=('PB5  {0,4}   {1}' -f $state.pb5,$state.level)
        $raw.ForeColor=if($state.level -eq 'HIGH'){[Drawing.Color]::Orange}else{[Drawing.Color]::Cyan}
        $high=if($null -eq $state.last_high_ms){'--'}else{('{0:N1}s' -f ($state.last_high_ms/1000))}
        $low=if($null -eq $state.last_low_ms){'--'}else{('{0:N1}s' -f ($state.last_low_ms/1000))}
        $timing.Text=('{0} for {1:N1}s | last HIGH {2} | last LOW {3}' -f $state.level,($state.level_ms/1000),$high,$low)
        $details.Text=('Lux {0} | Dimmer {1}% | Relay {2} | Timer {3:N1}s | Motion #{4} | Pending {5}' -f
            $f.LUX,$f.PWM,$f.RELAY,($f.TIMER_MS/1000),$f.MOTION_SEQ,$f.PENDING)+"`r`n"+
            ('PIR samples {0} | Light samples {1} | Transitions {2} | Ready {3}/7 | Warmup {4:N1}s' -f
            $f.PIR_SAMPLES,$f.LUX_SAMPLES,$state.transitions,$f.READY_MASK,($f.WARMUP_MS/1000))
        if($state.samples -ne $script:lastSample){
            $script:lastSample=$state.samples;$x=$state.elapsed_ms/1000.0
            $null=$series.Points.AddXY($x,[double]$state.pb5)
            while($series.Points.Count -gt 1 -and $series.Points[0].XValue -lt $x-60){$series.Points.RemoveAt(0)}
            $area.AxisX.Minimum=[Math]::Max(0,$x-60);$area.AxisX.Maximum=[Math]::Max(60,$x)
        }
    }catch{}
})
$form.Add_Shown({$timer.Start();$form.Activate()})
$form.Add_FormClosed({$timer.Stop();$timer.Dispose()})
$null=$form.ShowDialog();$form.Dispose()
