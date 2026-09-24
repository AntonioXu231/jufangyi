<#
  Bounded board-level PS-12 batch-analysis regression for the DDS prototype.
  The script owns its TCP connection, so it does not depend on a PowerShell
  function left over from an earlier terminal session.
#>
param(
    [string]$TargetIp = '192.168.1.10',
    [int]$Port = 6001,
    [int]$Packets = 512,
    [int]$WaitSeconds = 8,
    [ValidateRange(0, 15)][int]$ChannelMask = 15,
    [int]$ThresholdPermille = 1000
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
    Require-Pd ($config -match 'api=9') 'PS-12 API 9 is not running; rebuild and download the current ELF.'

    $start = Invoke-PdCommand "START $Packets"
    Require-Pd ($start -match '^OK start accepted') 'Acquisition start was rejected.'
    Start-Sleep -Seconds $WaitSeconds

    $status = Invoke-PdCommand 'STATUS'
    Require-Pd ($status -match 'state=0') "Acquisition did not return to IDLE: $status"
    Require-Pd ($status -match 'err=$') "Acquisition reported an error: $status"

    $null = Invoke-PdCommand "SET ALERT MASK $ChannelMask"
    $null = Invoke-PdCommand "SET ALERT DELTA $ThresholdPermille"
    $thresholdConfigured = $true

    $batch = Invoke-PdCommand 'BATCH AUTO'
    Require-Pd ($batch -match '^BATCH snap_seq=\[(\d+),(\d+)\) processed=(\d+) sweep_seq=\[(\d+),(\d+)\)') "Batch operation failed: $batch"
    $firstSnapshotSequence = [uint32]$Matches[1]
    $nextSnapshotSequence = [uint32]$Matches[2]
    $processed = [uint32]$Matches[3]
    $firstSweepSequence = [uint32]$Matches[4]
    $nextSweepSequence = [uint32]$Matches[5]
    Require-Pd ($processed -eq ($nextSnapshotSequence - $firstSnapshotSequence)) 'Batch processed count does not match its snapshot sequence window.'
    Require-Pd ($processed -gt 0) 'Batch did not process a retained snapshot.'
    Require-Pd (($nextSweepSequence - $firstSweepSequence) -eq $processed) 'Batch did not create exactly one sweep per snapshot.'

    $sweep = Invoke-PdCommand "SWEEP SNAP SEQ $firstSnapshotSequence"
    Require-Pd ($sweep -match '^SWEEP index=') 'First batch sweep is not queryable by snapshot sequence.'
    $feature = Invoke-PdCommand "FEATURE SWEEP SNAP SEQ $firstSnapshotSequence"
    Require-Pd ($feature -match '^FEATURE index=') 'First batch feature is not queryable by snapshot sequence.'
    $alert = Invoke-PdCommand "ALERT SWEEP SNAP SEQ $firstSnapshotSequence"
    Require-Pd ($alert -match '^ALERT index=') 'First batch rule verdict is not queryable by snapshot sequence.'

    $null = Invoke-PdCommand 'SET ALERT DELTA 0'
    $thresholdConfigured = $false
    Write-Host "PS12_BATCH_TEST_PASS processed=$processed snap_seq=[$firstSnapshotSequence,$nextSnapshotSequence) sweep_seq=[$firstSweepSequence,$nextSweepSequence)"
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
