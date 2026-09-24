<#
  Integrated PS regression for the DDS prototype (API 11).
  This is a host-side verification tool, not the final upper-computer program.
  It owns all TCP resources so it can be run in a new PowerShell session.
#>
param(
    [string]$TargetIp = '192.168.1.10',
    [int]$Port = 6001,
    [int]$Packets = 512,
    [int]$WaitSeconds = 8,
    [ValidateRange(0, 15)][int]$ChannelMask = 15,
    [int]$ThresholdPermille = 1000,
    [switch]$ClearAtEnd
)

$ErrorActionPreference = 'Stop'
$client = $null; $stream = $null; $writer = $null; $reader = $null
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
    $client.ReceiveTimeout = 60000; $client.SendTimeout = 60000
    $client.Connect($TargetIp, $Port)
    $stream = $client.GetStream()
    $writer = [System.IO.StreamWriter]::new($stream)
    $reader = [System.IO.StreamReader]::new($stream)
    $writer.AutoFlush = $true

    $banner = $reader.ReadLine(); Write-Host "<< $banner"
    Require-Pd ($banner -match 'PD_ACQ TCP PS-2 READY') 'Unexpected TCP service banner.'
    $config = Invoke-PdCommand 'CONFIG'
    Require-Pd ($config -match 'api=11.*prpd_bins=64') 'API 11 with 64 PRPD bins is not running; build and download the current ELF.'
    $fft = Invoke-PdCommand 'FFT SELFTEST'
    Require-Pd ($fft -match '^FFT_SELFTEST_PASS') "FFT self-test failed: $fft"

    $start = Invoke-PdCommand "START $Packets"
    Require-Pd ($start -match '^OK start accepted') 'Acquisition start was rejected.'
    Start-Sleep -Seconds $WaitSeconds
    $status = Invoke-PdCommand 'STATUS'
    Require-Pd ($status -match 'state=0') "Acquisition did not return to IDLE: $status"
    Require-Pd ($status -match 'err=$') "Acquisition reported an error: $status"

    $catalog = Invoke-PdCommand 'CATALOG'
    Require-Pd ($catalog -match 'event_seq=\[(\d+),(\d+)\).*snap_seq=\[(\d+),(\d+)\)') "Catalog reply is malformed: $catalog"
    $firstEvent = [uint32]$Matches[1]; $nextEvent = [uint32]$Matches[2]
    $firstSnapshot = [uint32]$Matches[3]; $nextSnapshot = [uint32]$Matches[4]
    Require-Pd ($nextEvent -gt $firstEvent) 'No retained feature event exists.'
    Require-Pd ($nextSnapshot -gt $firstSnapshot) 'No retained snapshot exists; increase packet count or wait time.'

    $detail = Invoke-PdCommand "EVENT DETAIL SEQ $firstEvent"
    Require-Pd ($detail -match '^EVENT_DETAIL .*peaks=\d+ cycles=\d+') "Event packet decoder failed: $detail"
    $prpd = Invoke-PdCommand 'PRPD SUMMARY'
    Require-Pd ($prpd -match '^PRPD seq=\[\d+,\d+\) packets=\d+ bins=64 ') "PRPD summary is malformed: $prpd"
    $prpdBins = Invoke-PdCommand 'PRPD BINS 0 0 8'
    Require-Pd ($prpdBins -match '^PRPD_BINS seq=\[\d+,\d+\) ch=0 first=0 count=8 values=') "PRPD bins are malformed: $prpdBins"

    $null = Invoke-PdCommand "SET ALERT MASK $ChannelMask"
    $null = Invoke-PdCommand "SET ALERT DELTA $ThresholdPermille"
    $thresholdConfigured = $true
    $batch = Invoke-PdCommand 'BATCH AUTO'
    Require-Pd ($batch -match '^BATCH snap_seq=\[(\d+),(\d+)\) processed=(\d+) sweep_seq=\[(\d+),(\d+)\)') "Batch operation failed: $batch"
    $batchFirstSnapshot = [uint32]$Matches[1]; $batchNextSnapshot = [uint32]$Matches[2]
    $processed = [uint32]$Matches[3]; $batchFirstSweep = [uint32]$Matches[4]; $batchNextSweep = [uint32]$Matches[5]
    Require-Pd ($processed -eq ($batchNextSnapshot - $batchFirstSnapshot)) 'Batch count and snapshot sequence range disagree.'
    Require-Pd (($batchNextSweep - $batchFirstSweep) -eq $processed) 'Batch did not create one sweep per snapshot.'

    $sweep = Invoke-PdCommand "SWEEP SNAP SEQ $batchFirstSnapshot"
    Require-Pd ($sweep -match '^SWEEP index=') "Batch sweep is not queryable: $sweep"
    $feature = Invoke-PdCommand "FEATURE SWEEP SNAP SEQ $batchFirstSnapshot"
    Require-Pd ($feature -match '^FEATURE index=') "Batch feature is not queryable: $feature"
    $alert = Invoke-PdCommand "ALERT SWEEP SNAP SEQ $batchFirstSnapshot"
    Require-Pd ($alert -match '^ALERT index=') "Batch alert is not queryable: $alert"
    $report = Invoke-PdCommand 'REPORT'
    Require-Pd ($report -match '^REPORT api=11 ') "Report is malformed: $report"
    Require-Pd ($report -match 'err=$') "Report includes acquisition error: $report"

    $null = Invoke-PdCommand 'SET ALERT DELTA 0'
    $thresholdConfigured = $false
    if ($ClearAtEnd) {
        $clear = Invoke-PdCommand 'CLEAR'
        Require-Pd ($clear -match '^OK CLEAR') "CLEAR failed: $clear"
    }
    Write-Host "PS_FINAL_REGRESSION_PASS events=[$firstEvent,$nextEvent) snapshots=[$firstSnapshot,$nextSnapshot) processed=$processed"
}
finally {
    if ($thresholdConfigured -and $writer -and $reader) {
        try { $writer.WriteLine('SET ALERT DELTA 0'); [void]$reader.ReadLine() } catch { }
    }
    if ($reader) { $reader.Dispose() }; if ($writer) { $writer.Dispose() }
    if ($stream) { $stream.Dispose() }; if ($client) { $client.Dispose() }
}
