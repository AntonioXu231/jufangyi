<#
  Bounded board-level PS-11 rule-verdict regression for the DDS prototype.
  It creates and disposes its own TCP connection; no caller functions or
  PowerShell session variables are required.
#>
param(
    [string]$TargetIp = '192.168.1.10',
    [int]$Port = 6001,
    [int]$Packets = 512,
    [int]$WaitSeconds = 8,
    [int]$ThresholdPermille = 1000,
    [ValidateRange(0, 15)][int]$ChannelMask = 15,
    [ValidateRange(0, 15)][int]$ExpectedHitMask = 2
)

$ErrorActionPreference = 'Stop'
$client = $null
$stream = $null
$writer = $null
$reader = $null
$thresholdConfigured = $false

function Invoke-PdCommand {
    param([string]$Command)

    Write-Host ">> $Command"
    $writer.WriteLine($Command)
    $reply = $reader.ReadLine()
    if ($null -eq $reply) { throw 'Target closed the TCP connection.' }
    Write-Host "<< $reply"
    return $reply
}

function Require-Pd {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

try {
    $client = [System.Net.Sockets.TcpClient]::new()
    $client.ReceiveTimeout = 60000
    $client.SendTimeout = 60000
    $client.Connect($TargetIp, $Port)
    $stream = $client.GetStream()
    $writer = [System.IO.StreamWriter]::new($stream)
    $reader = [System.IO.StreamReader]::new($stream)
    $writer.AutoFlush = $true

    $banner = $reader.ReadLine()
    Write-Host "<< $banner"
    Require-Pd ($banner -match 'PD_ACQ TCP PS-2 READY') 'Unexpected TCP service banner.'

    $config = Invoke-PdCommand 'CONFIG'
    Require-Pd ($config -match 'api=8') 'PS-11 API 8 is not running; rebuild and download the current ELF.'

    $start = Invoke-PdCommand "START $Packets"
    Require-Pd ($start -match '^OK start accepted') 'Acquisition start was rejected.'
    Start-Sleep -Seconds $WaitSeconds

    $status = Invoke-PdCommand 'STATUS'
    Require-Pd ($status -match 'state=0') "Acquisition did not return to IDLE: $status"
    Require-Pd ($status -match 'err=$') "Acquisition reported an error: $status"

    $sweep = Invoke-PdCommand 'SWEEP AUTO SNAP 0'
    Require-Pd ($sweep -match '^SWEEP index=(\d+) sweep=(\d+)') "Sweep record was not created: $sweep"
    $sweepIndex = [int]$Matches[1]

    $null = Invoke-PdCommand "SET ALERT MASK $ChannelMask"
    $null = Invoke-PdCommand "SET ALERT DELTA $ThresholdPermille"
    $thresholdConfigured = $true
    $rule = Invoke-PdCommand "ALERT SWEEP $sweepIndex"
    Require-Pd ($rule -match ("hit_mask=0x{0:x}" -f $ExpectedHitMask)) "Unexpected enabled rule result: $rule"

    $null = Invoke-PdCommand 'SET ALERT DELTA 0'
    $thresholdConfigured = $false
    $disabled = Invoke-PdCommand "ALERT SWEEP $sweepIndex"
    Require-Pd ($disabled -match 'hit_mask=0x0') "Rule did not disable cleanly: $disabled"

    Write-Host "PS11_ALERT_TEST_PASS threshold=$ThresholdPermille mask=0x$('{0:x}' -f $ChannelMask) expected_hit=0x$('{0:x}' -f $ExpectedHitMask)"
}
finally {
    if ($thresholdConfigured -and $writer -and $reader) {
        try {
            $writer.WriteLine('SET ALERT DELTA 0')
            [void]$reader.ReadLine()
        } catch { }
    }
    if ($reader) { $reader.Dispose() }
    if ($writer) { $writer.Dispose() }
    if ($stream) { $stream.Dispose() }
    if ($client) { $client.Dispose() }
}
