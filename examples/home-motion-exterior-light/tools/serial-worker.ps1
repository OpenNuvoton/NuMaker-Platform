param([string]$Port = "COM3")
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$serial = New-Object System.IO.Ports.SerialPort $Port,115200,"None",8,"One"
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.ReadTimeout = 3000
$serial.WriteTimeout = 3000
$serial.NewLine = "`n"
@{ready=$true; pid=$PID} | ConvertTo-Json -Compress
try {
    while ($null -ne ($line = [Console]::In.ReadLine())) {
        $requestId = $null
        try {
            if ($line.Length -gt 8192) { throw "Bridge request too large." }
            $request = $line | ConvertFrom-Json
            $requestId = $request.id
            if ($request.quit) { break }
            $readTimeout = 3000
            if ($null -ne $request.read_timeout_ms) {
                $readTimeout = [int]$request.read_timeout_ms
                if ($readTimeout -lt 100 -or $readTimeout -gt 20000) { throw "Invalid serial read timeout." }
            }
            $serial.ReadTimeout = $readTimeout
            if (-not $request.commands -or $request.commands.Count -gt 64) {
                throw "Expected one to sixty-four board commands."
            }
            foreach ($command in $request.commands) {
                if ($command -isnot [string] -or $command.Length -gt 127 -or $command -match '[^\x20-\x7e]') {
                    throw "Invalid serial command."
                }
            }
            if (-not $serial.IsOpen) {
                $serial.Open()
                Start-Sleep -Milliseconds 100
                $serial.DiscardInBuffer()
            }
            $responses = @()
            foreach ($command in $request.commands) {
                $serial.WriteLine($command)
                do {
                    $reply = $serial.ReadLine().TrimEnd("`r")
                } while (-not ($reply.StartsWith("OK ") -or $reply.StartsWith("ERR ")))
                if ($reply.StartsWith("ERR ")) { throw "Board rejected command: $reply" }
                $responses += $reply
            }
            @{id=$requestId; ok=$true; responses=$responses} | ConvertTo-Json -Compress
        } catch {
            $message = $_.Exception.Message
            # A protocol rejection keeps the connection; transport errors reopen it.
            if (-not $message.StartsWith("Board rejected command:")) {
                if ($serial.IsOpen) { $serial.Close() }
            }
            @{id=$requestId; ok=$false; error=$message} | ConvertTo-Json -Compress
        }
    }
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
