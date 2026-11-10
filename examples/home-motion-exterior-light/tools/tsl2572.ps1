# TSL25721: address 0x39, PC0 SDA / PC1 SCL, ID register 0x92 = 0x34.
# Register layout and lux coefficients: ams OSRAM TSL2572 datasheet v2-00.
function Read-AmbientRegister([int]$register, [int]$count) {
    $reply = Send-Board "I2C ALT READ 57 $register $count"
    if ($reply -notmatch '^OK I2C DATA 57 ([0-9A-F]+)$' -or $Matches[1].Length -ne 2*$count) {
        throw 'Unexpected ambient sensor register response'
    }
    $hex = $Matches[1]
    $bytes = [byte[]]::new($count)
    for ($index=0; $index -lt $count; $index++) {
        $bytes[$index] = [Convert]::ToByte($hex.Substring(2*$index,2),16)
    }
    return ,$bytes
}

function Write-AmbientRegister([int]$register, [int]$value) {
    $hex = '{0:X2}{1:X2}' -f $register,$value
    $null = Send-Board "I2C ALT WRITE 57 $hex"
}

function Get-AmbientLux([int]$channel0, [int]$channel1) {
    $countsPerLux = 51.87 / 60.0 # integration 51.87 ms, 1x gain, glass factor 1
    $lux1 = ($channel0 - 1.87*$channel1) / $countsPerLux
    $lux2 = (0.63*$channel0 - $channel1) / $countsPerLux
    return [Math]::Max(0.0,[Math]::Max($lux1,$lux2))
}

function Initialize-AmbientSensor {
    [byte[]]$identity = Read-AmbientRegister 146 1
    if ($identity[0] -ne 0x34) { throw ('Unexpected ambient chip ID 0x{0:X2}' -f $identity[0]) }
    [byte[]]$enabled = Read-AmbientRegister 128 1
    Write-AmbientRegister 128 1 # PON
    Start-Sleep -Milliseconds 3
    Write-AmbientRegister 129 237 # ATIME = 0xED: 19 x 2.73 ms
    Write-AmbientRegister 143 0 # CONTROL = 1x gain
    Write-AmbientRegister 141 0 # CONFIG = standard gain / no long wait
    Write-AmbientRegister 128 3 # PON | AEN, continuous ambient conversions
    return @{model='TSL25721';family='TSL2572';address='0x39';sda='PC0';scl='PC1';chip_id='0x34';integration_ms=51.87;gain=1;initial_enable_register=[int]$enabled[0];valid=$false;lux=$null}
}

function Read-AmbientSensor($previous) {
    [byte[]]$status = Read-AmbientRegister 147 1
    $result = @{} + $previous
    $result.valid = [bool]($status[0] -band 1)
    if ($result.valid) {
        # Auto-increment starting at C0DATA (0x14): command 0xA0 | 0x14.
        [byte[]]$bytes = Read-AmbientRegister 180 4
        $channel0 = [int]$bytes[0] -bor ([int]$bytes[1] -shl 8)
        $channel1 = [int]$bytes[2] -bor ([int]$bytes[3] -shl 8)
        $result.channel0 = $channel0
        $result.channel1 = $channel1
        $result.saturated = $channel0 -ge 19456 -or $channel1 -ge 19456
        $result.lux = [Math]::Round((Get-AmbientLux $channel0 $channel1),2)
        $result.updated_utc = [DateTime]::UtcNow.ToString('o')
    }
    return $result
}
