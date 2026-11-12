$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
foreach($file in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1') {
    $tokens=$null;$errors=$null
    $null=[Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors|Out-String)}
}
$flasher=Join-Path $PSScriptRoot 'flash-oled-boards.ps1'
$example=Join-Path $root 'config/boards.example.json'
function Must-Reject([hashtable]$arguments,[string]$expected){
    $rejected=$false
    try{& $flasher @arguments}catch{
        if($_.Exception.Message -notmatch $expected){throw}
        $rejected=$true
    }
    if(-not $rejected){throw 'An invalid flashing configuration was accepted.'}
}
Must-Reject @{StandaloneDemo=$true;Board='sensor';TargetConfig=$example} 'Configure each role'
Must-Reject @{StandaloneDemo=$true;Board='sensor';FixedDimmer=60;TargetConfig=$example} 'requires the standalone dt57'
Must-Reject @{StandaloneDemo=$true;Board='dt57';PB5Display=$true;TargetConfig=$example} 'requires the standalone sensor'
Must-Reject @{StandaloneDemo=$true;Board='sensor';TargetConfig=(Join-Path $root 'config/does-not-exist.json')} 'Copy config/boards.example.json'
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('numaker-config-'+[guid]::NewGuid()+'.json')
try{
    $config=Get-Content -LiteralPath $example -Raw|ConvertFrom-Json
    $config.boards[1].parent=$config.boards[0].parent
    [IO.File]::WriteAllText($scratch,($config|ConvertTo-Json -Depth 5))
    Must-Reject @{StandaloneDemo=$true;Board='sensor';TargetConfig=$scratch} 'must be unique'
}finally{if(Test-Path -LiteralPath $scratch){Remove-Item -LiteralPath $scratch}}
Write-Output 'PASS: all PowerShell files parse; invalid identities, missing configuration and wrong diagnostic roles rejected before elevation or device access'
