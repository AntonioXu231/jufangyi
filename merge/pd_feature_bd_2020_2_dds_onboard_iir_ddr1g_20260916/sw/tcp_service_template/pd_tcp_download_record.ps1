param(
    [ValidateSet('EVENT', 'SNAP')]
    [string]$Kind = 'SNAP',
    [ValidateRange(0, 15)]
    [int]$Index = 0,
    [string]$OutFile = '.\pd_record.bin',
    [string]$HostIp = '192.168.1.10',
    [ValidateRange(1, 65535)]
    [int]$Port = 6001,
    [ValidateRange(1, 16384)]
    [int]$ChunkBytes = 16384
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($null -eq ('PdTcpCrc32' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
public static class PdTcpCrc32 {
    public static UInt32 Compute(byte[] data) {
        UInt32 crc = 0xFFFFFFFFU;
        foreach (byte value in data) {
            crc ^= value;
            for (int bit = 0; bit < 8; ++bit)
                crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
        return ~crc;
    }
}
'@
}

function Read-AsciiLine([System.Net.Sockets.NetworkStream]$Stream) {
    $bytes = [System.Collections.Generic.List[byte]]::new()
    while ($true) {
        $v = $Stream.ReadByte()
        if ($v -lt 0) { throw 'TCP peer closed while reading an ASCII header.' }
        if ($v -eq 10) { break }
        if ($v -ne 13) { [void]$bytes.Add([byte]$v) }
        if ($bytes.Count -gt 511) { throw 'ASCII header exceeds 511 bytes.' }
    }
    return [System.Text.Encoding]::ASCII.GetString($bytes.ToArray())
}

function Read-Exactly([System.Net.Sockets.NetworkStream]$Stream, [int]$Count) {
    [byte[]]$data = New-Object byte[] $Count
    $done = 0
    while ($done -lt $Count) {
        $n = $Stream.Read($data, $done, $Count - $done)
        if ($n -le 0) { throw "TCP peer closed after $done of $Count payload bytes." }
        $done += $n
    }
    return ,$data
}

function Send-Line([System.Net.Sockets.NetworkStream]$Stream, [string]$Line) {
    [byte[]]$data = [System.Text.Encoding]::ASCII.GetBytes("$Line`r`n")
    $Stream.Write($data, 0, $data.Length)
}

$client = [System.Net.Sockets.TcpClient]::new()
$client.ReceiveTimeout = 30000
$client.SendTimeout = 30000
$client.Connect($HostIp, $Port)
$stream = $client.GetStream()

try {
    $banner = Read-AsciiLine $stream
    if ($banner -notmatch '^PD_ACQ TCP V2 READY') {
        throw "Target is not TCP service V2: $banner"
    }

    Send-Line $stream "$Kind $Index"
    $metadata = Read-AsciiLine $stream
    if ($metadata -match '^ERR') { throw "Target rejected record: $metadata" }
    if ($metadata -notmatch '\bbytes=(\d+)\b') {
        throw "Cannot obtain byte length from: $metadata"
    }
    [int]$total = $Matches[1]
    if ($total -le 0) { throw 'Record has zero length.' }

    $outDir = Split-Path -Parent $OutFile
    if ($outDir) { [System.IO.Directory]::CreateDirectory($outDir) | Out-Null }
    $file = [System.IO.File]::Open($OutFile,
        [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
        for ([int]$offset = 0; $offset -lt $total; $offset += $ChunkBytes) {
            [int]$want = [Math]::Min($ChunkBytes, $total - $offset)
            Send-Line $stream "GET $Kind $Index $offset $want"
            $header = Read-AsciiLine $stream
            $pattern = "^DATA V2 kind=$Kind index=$Index offset=$offset bytes=(\d+) crc32=([0-9A-Fa-f]{8})$"
            if ($header -notmatch $pattern) { throw "Unexpected DATA header: $header" }
            [int]$actual = $Matches[1]
            [uint32]$expectedCrc = [Convert]::ToUInt32($Matches[2], 16)
            if ($actual -ne $want) { throw "Length mismatch at offset ${offset}: expected $want, got $actual" }
            [byte[]]$chunk = Read-Exactly $stream $actual
            [uint32]$actualCrc = [PdTcpCrc32]::Compute($chunk)
            if ($actualCrc -ne $expectedCrc) {
                throw ('CRC32 mismatch at offset {0}: expected {1:X8}, got {2:X8}' -f $offset, $expectedCrc, $actualCrc)
            }
            $file.Write($chunk, 0, $chunk.Length)
            Write-Progress -Activity "Downloading $Kind $Index" -Status "$($offset + $actual) / $total bytes" -PercentComplete (100.0 * ($offset + $actual) / $total)
        }
    }
    finally {
        $file.Dispose()
        Write-Progress -Activity "Downloading $Kind $Index" -Completed
    }

    $sidecar = [ordered]@{
        kind = $Kind; index = $Index; bytes = $total; source_metadata = $metadata
        downloaded_utc = [DateTime]::UtcNow.ToString('o'); target = "$HostIp`:$Port"
    } | ConvertTo-Json
    [System.IO.File]::WriteAllText("$OutFile.json", $sidecar, [System.Text.Encoding]::UTF8)
    Write-Host "DOWNLOAD_PASS bytes=$total crc32=verified file=$OutFile"
}
finally {
    $stream.Dispose()
    $client.Dispose()
}
