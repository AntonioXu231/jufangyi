<#!
Bounded PS-4 TCP soak test for the modular acquisition service.

This is a validation client, not the final upper-computer application.  It
allows the documented PS DDR archive rings to overwrite, but rejects service
faults, non-zero hardware slot drops, and a failure to stop cleanly.
#>
[CmdletBinding()]
param(
    [string]$IpAddress = '192.168.1.10',
    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 120,
    [ValidateRange(1, 60)]
    [int]$StatusPeriodSeconds = 5,
    [ValidateRange(1, 60)]
    [int]$StopTimeoutSeconds = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$client = $null
$stream = $null
$writer = $null
$reader = $null

function Send-PdLine {
    param([Parameter(Mandatory = $true)][string]$Command)

    $writer.WriteLine($Command)
    $reply = $reader.ReadLine()
    if ($null -eq $reply) { throw "TCP peer closed while waiting for '$Command'" }
    Write-Host "$(Get-Date -Format 'HH:mm:ss')  $Command  =>  $reply"
    return $reply
}

function Get-StatusField {
    param(
        [Parameter(Mandatory = $true)][string]$Status,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $match = [regex]::Match($Status, "(?:^|\s)$Name=([^\s]*)")
    if (-not $match.Success) { throw "STATUS is missing '$Name': $Status" }
    return $match.Groups[1].Value
}

try {
    $client = [System.Net.Sockets.TcpClient]::new($IpAddress, 6001)
    $client.ReceiveTimeout = 10000
    $stream = $client.GetStream()
    $stream.ReadTimeout = 10000
    $writer = [System.IO.StreamWriter]::new($stream)
    $reader = [System.IO.StreamReader]::new($stream)
    $writer.AutoFlush = $true

    $banner = $reader.ReadLine()
    if ($null -eq $banner) { throw 'TCP peer closed before its banner.' }
    Write-Host "Connected: $banner"

    $initial = Send-PdLine 'STATUS'
    if ((Get-StatusField $initial 'state') -ne '0') {
        throw "Service must be idle before the soak test: $initial"
    }
    $initialCatalog = Send-PdLine 'CATALOG'

    $startReply = Send-PdLine 'START 0'
    if ($startReply -ne 'OK start accepted') { throw "Continuous START rejected: $startReply" }

    $deadline = (Get-Date).AddSeconds($DurationSeconds)
    $lastEventNext = -1L
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds $StatusPeriodSeconds
        $status = Send-PdLine 'STATUS'
        if ((Get-StatusField $status 'state') -ne '1') { throw "Service left RUNNING state: $status" }
        if ((Get-StatusField $status 'drops') -ne '0') { throw "Hardware slot drops observed: $status" }
        if ($status -notmatch 'err=$') { throw "Acquisition service error: $status" }

        $catalog = Send-PdLine 'CATALOG'
        $match = [regex]::Match($catalog, 'event_seq=\[(\d+),(\d+)\)')
        if (-not $match.Success) { throw "Malformed CATALOG: $catalog" }
        $eventNext = [Int64]$match.Groups[2].Value
        if ($eventNext -le $lastEventNext) { throw "Event sequence did not advance: $catalog" }
        $lastEventNext = $eventNext
    }

    $stopReply = Send-PdLine 'STOP'
    if ($stopReply -ne 'OK stop requested') { throw "STOP rejected: $stopReply" }

    $stopDeadline = (Get-Date).AddSeconds($StopTimeoutSeconds)
    do {
        Start-Sleep -Seconds 1
        $finalStatus = Send-PdLine 'STATUS'
        if ((Get-StatusField $finalStatus 'state') -eq '0') { break }
    } while ((Get-Date) -lt $stopDeadline)

    if ((Get-StatusField $finalStatus 'state') -ne '0') {
        throw "Service did not return to IDLE: $finalStatus"
    }
    if ((Get-StatusField $finalStatus 'drops') -ne '0') {
        throw "Hardware slot drops observed at clean stop: $finalStatus"
    }
    if ($finalStatus -notmatch 'err=$') { throw "Service error at clean stop: $finalStatus" }

    $finalCatalog = Send-PdLine 'CATALOG'
    Write-Host "SOAK_TEST_PASS duration_s=$DurationSeconds final='$finalStatus' catalog='$finalCatalog'"
}
finally {
    if ($null -ne $reader) { $reader.Dispose() }
    if ($null -ne $writer) { $writer.Dispose() }
    if ($null -ne $stream) { $stream.Dispose() }
    if ($null -ne $client) { $client.Dispose() }
}
