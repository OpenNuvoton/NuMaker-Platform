param([switch]$Stop,[switch]$Restart,[string]$Port='COM11')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$null=New-Item -ItemType Directory -Path (Join-Path $root 'build') -Force
$scriptPath=Join-Path $PSScriptRoot 'pir-live.ps1'
$sessionPath=Join-Path $root 'build\pir-live-session.json'
$parseTokens=$null;$parseErrors=$null
$null=[Management.Automation.Language.Parser]::ParseFile($scriptPath,[ref]$parseTokens,[ref]$parseErrors)
if($parseErrors.Count){throw ($parseErrors|Out-String)}
$session=@{}
if(Test-Path -LiteralPath $sessionPath){
    $old=Get-Content -LiteralPath $sessionPath -Raw|ConvertFrom-Json
    foreach($role in @('worker','view')){
        $idValue=$old.$role
        if($idValue -and (Get-Process -Id $idValue -ErrorAction SilentlyContinue)){
            $process=Get-CimInstance Win32_Process -Filter "ProcessId=$idValue"
            if($process.CommandLine -like ('*'+$scriptPath+'*')){$session[$role]=[int]$idValue}
        }
    }
}
if($Stop -or $Restart){
    foreach($idValue in $session.Values){Stop-Process -Id $idValue -Force}
    [IO.File]::WriteAllText($sessionPath,'{}');$session=@{}
    if($Stop){Write-Output 'PB5 monitor stopped; board firmware continues.';exit}
}
foreach($role in @('worker','view')){
    if(-not $session.ContainsKey($role)){
        $arguments=@('-NoProfile','-STA','-ExecutionPolicy','Bypass','-File',('"'+$scriptPath+'"'))
        $arguments+=@('-Port',$Port)
        if($role -eq 'worker'){$arguments+='-Worker'}
        $style=if($role -eq 'worker'){'Hidden'}else{'Normal'}
        $process=Start-Process powershell.exe -ArgumentList $arguments -WindowStyle $style -PassThru `
            -RedirectStandardOutput (Join-Path $root "build\pir-live-$role.out.log") `
            -RedirectStandardError (Join-Path $root "build\pir-live-$role.error.log")
        $session[$role]=$process.Id
    }
}
[IO.File]::WriteAllText($sessionPath,($session|ConvertTo-Json))
$session|ConvertTo-Json -Compress
