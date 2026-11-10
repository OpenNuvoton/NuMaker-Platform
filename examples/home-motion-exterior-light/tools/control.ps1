param(
    [string]$Port = "COM3",
    [string[]]$CommandText = @("STATUS"),
    [switch]$AllowErrors
)
$ErrorActionPreference = "Stop"
$serial = New-Object System.IO.Ports.SerialPort $Port,115200,"None",8,"One"
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.ReadTimeout = 3000
$serial.WriteTimeout = 3000
$serial.NewLine = "`n"
try {
    $serial.Open()
    Start-Sleep -Milliseconds 100
    $serial.DiscardInBuffer()
    foreach ($command in $CommandText) {
        if ($command.Length -gt 127 -or $command -match '[^\x20-\x7e]') {
            throw "Commands must contain at most 127 printable ASCII characters."
        }
        if ([string]::IsNullOrWhiteSpace($command)) { continue }
        Write-Output "> $command"
        $serial.WriteLine($command)
        do {
            try {
                $reply = $serial.ReadLine().TrimEnd("`r")
            } catch {
                throw "No response on $Port. Check that Nu-Link VCOM switches 1 and 2 are ON and the control firmware is running."
            }
            Write-Output $reply
            if ($reply.StartsWith("ERR ") -and -not $AllowErrors) {
                throw "Board rejected command: $reply"
            }
        } while (-not ($reply.StartsWith("OK ") -or $reply.StartsWith("ERR ")))
    }
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
