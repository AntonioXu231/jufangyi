# Build the existing Vitis 2024.1 PS application without the Unified IDE task UI.
# Run after closing Vitis:
#   & 'F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916\sw\ps_service\tools\build_vitis_app_direct.ps1' -CheckOnly
#   & 'F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916\sw\ps_service\tools\build_vitis_app_direct.ps1'
# Expected output: F:\ps\lwip_echo_server10\build\lwip_echo_server10.elf
# Next: download that ELF to the Zynq and check CONFIG/STATUS over TCP.

[CmdletBinding()]
param(
    [string]$WorkspaceRoot = 'F:\ps',
    [switch]$CheckOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$appName = 'lwip_echo_server10'
$platformName = 'platform5'
$appDir = Join-Path $WorkspaceRoot $appName
$srcDir = Join-Path $appDir 'src'
$buildDir = Join-Path $appDir 'build'
$cmakeFile = Join-Path $srcDir 'CMakeLists.txt'
$serviceFile = Join-Path $srcDir 'pd_tcp_service.c'
$userConfigFile = Join-Path $srcDir 'UserConfig.cmake'
$compileCommandsFile = Join-Path $buildDir 'compile_commands.json'
$cacheFile = Join-Path $buildDir 'CMakeCache.txt'
$ninjaFile = Join-Path $buildDir 'build.ninja'
$elfFile = Join-Path $buildDir "$appName.elf"
$platformStatus = Join-Path $WorkspaceRoot "$platformName\export\.buildstatus"
$platformLib = Join-Path $WorkspaceRoot "$platformName\export\$platformName\sw\standalone_ps7_cortexa9_0\lib\libxil.a"

foreach ($file in @($cmakeFile, $serviceFile, $cacheFile, $ninjaFile,
                    $platformStatus, $platformLib, $userConfigFile,
                    $compileCommandsFile)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required file is missing: $file"
    }
}
if ((Get-Content -LiteralPath $userConfigFile -Raw) -notmatch
    '(?m)^\s*set\(USER_COMPILE_OPTIMIZATION_LEVEL\s+-O2\s*\)\s*$') {
    throw "PS hot-loop optimization must be -O2: $userConfigFile"
}
if ((Get-Content -LiteralPath $platformStatus -Raw) -notmatch '(?m)^export=SUCCESS\s*$') {
    throw "$platformName has not exported successfully: $platformStatus"
}

$serviceEntries = @(Get-Content -LiteralPath $cmakeFile | Where-Object {
    $_ -match '^\s*collect\s*\(\s*PROJECT_LIB_SOURCES\s+(?:\.\./)?pd_tcp_service\.c\s*\)\s*$'
})
if ($serviceEntries.Count -eq 0) {
    throw "CMake does not include src/pd_tcp_service.c: $cmakeFile"
}
if (@($serviceEntries | Where-Object { $_ -match '\.\./' }).Count -ne 0) {
    throw "CMake still refers to the duplicate project-root pd_tcp_service.c: $cmakeFile"
}

$serviceText = Get-Content -LiteralPath $serviceFile -Raw
$moduleIncludes = [regex]::Matches($serviceText, '(?m)^\s*#include\s+"(modules/[^\"]+)"')
if ($moduleIncludes.Count -eq 0) {
    throw "The modular TCP service has no module includes: $serviceFile"
}
foreach ($include in $moduleIncludes) {
    $modulePath = Join-Path $srcDir ($include.Groups[1].Value.Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
        throw "Missing TCP service module: $modulePath"
    }
}

# Fail fast if the active Vitis app has not received the canonical modular PS
# sources. The UART entry point under sw/ps_service/src is intentionally not
# compared because this network app uses tcp/main.c.
$serviceRoot = Split-Path -Parent $PSScriptRoot
$sourcePairs = [System.Collections.Generic.List[object]]::new()
foreach ($source in Get-ChildItem -LiteralPath (Join-Path $serviceRoot 'include') -File -Filter '*.h') {
    $sourcePairs.Add([pscustomobject]@{ Source = $source.FullName; Target = (Join-Path $srcDir $source.Name) })
}
foreach ($source in Get-ChildItem -LiteralPath (Join-Path $serviceRoot 'src') -File |
                 Where-Object { $_.Name -ne 'main.c' -and $_.Extension -in '.c', '.h' }) {
    $sourcePairs.Add([pscustomobject]@{ Source = $source.FullName; Target = (Join-Path $srcDir $source.Name) })
}
$tcpRoot = Join-Path $serviceRoot 'tcp'
foreach ($sourceName in @('main.c', 'pd_tcp_service.c', 'pd_tcp_service.h')) {
    $sourcePairs.Add([pscustomobject]@{
        Source = Join-Path $tcpRoot $sourceName
        Target = Join-Path $srcDir $sourceName
    })
}
$tcpModules = Join-Path $tcpRoot 'modules'
foreach ($source in Get-ChildItem -LiteralPath $tcpModules -Recurse -File -Filter '*.inc') {
    $relative = $source.FullName.Substring($tcpModules.TrimEnd('\').Length + 1)
    $sourcePairs.Add([pscustomobject]@{
        Source = $source.FullName
        Target = Join-Path (Join-Path $srcDir 'modules') $relative
    })
}
foreach ($pair in $sourcePairs) {
    if (-not (Test-Path -LiteralPath $pair.Target -PathType Leaf)) {
        throw "Vitis app source is missing; copy the canonical PS source first: $($pair.Target)"
    }
    $sourceHash = (Get-FileHash -LiteralPath $pair.Source -Algorithm SHA256).Hash
    $targetHash = (Get-FileHash -LiteralPath $pair.Target -Algorithm SHA256).Hash
    if ($sourceHash -ne $targetHash) {
        throw "Vitis app source is stale; synchronize from $($pair.Source) to $($pair.Target)"
    }
}

$makeLine = Get-Content -LiteralPath $cacheFile | Where-Object {
    $_ -like 'CMAKE_MAKE_PROGRAM:FILEPATH=*'
} | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($makeLine)) {
    throw "CMAKE_MAKE_PROGRAM is missing from $cacheFile"
}
$ninjaExe = $makeLine.Substring($makeLine.IndexOf('=') + 1)
if (-not (Test-Path -LiteralPath $ninjaExe -PathType Leaf)) {
    throw "Vitis Ninja executable is missing: $ninjaExe"
}

# Vitis generated the post-link size command as a bare executable name. Find
# its directory from the compiler selected by this exact CMake build tree.
$compilerInfo = Get-ChildItem -LiteralPath (Join-Path $buildDir 'CMakeFiles') `
    -Recurse -File -Filter 'CMakeCCompiler.cmake' | Select-Object -First 1
if ($null -eq $compilerInfo) {
    throw "CMake C compiler information is missing from $buildDir"
}
$compilerMatch = [regex]::Match((Get-Content -LiteralPath $compilerInfo.FullName -Raw),
    '(?m)^set\(CMAKE_C_COMPILER "([^"]+arm-none-eabi-gcc\.exe)"\)')
if (-not $compilerMatch.Success) {
    throw "Cannot identify the ARM compiler in $($compilerInfo.FullName)"
}
$toolchainBin = Split-Path -Parent $compilerMatch.Groups[1].Value
$sizeExe = Join-Path $toolchainBin 'arm-none-eabi-size.exe'
if (-not (Test-Path -LiteralPath $sizeExe -PathType Leaf)) {
    throw "The Vitis post-link size tool is missing: $sizeExe"
}

$cmakePending = (Get-Item -LiteralPath $cmakeFile).LastWriteTimeUtc -gt
                (Get-Item -LiteralPath $ninjaFile).LastWriteTimeUtc
Write-Output "APP=$appName PLATFORM=$platformName MODULES=$($moduleIncludes.Count) CMAKE_SERVICE_ENTRIES=$($serviceEntries.Count) CMAKE_REGEN_PENDING=$cmakePending"
Write-Output "PS_APP_SOURCE_SYNC_PASS FILES=$($sourcePairs.Count)"
Write-Output "NINJA=$ninjaExe"
Write-Output "TOOLCHAIN_BIN=$toolchainBin"

if ($CheckOnly) {
    Write-Output 'PS_APP_BUILD_CHECK_PASS'
    return
}

if (@(Get-Process -Name 'vitisng-ide' -ErrorAction SilentlyContinue).Count -ne 0) {
    throw 'Save files and close Vitis before running this build to avoid concurrent writes.'
}

$originalPath = $env:PATH
$buildExitCode = -1
try {
    $env:PATH = "$toolchainBin;$originalPath"
    & $ninjaExe -C $buildDir all -v
    $buildExitCode = $LASTEXITCODE
} finally {
    $env:PATH = $originalPath
}
if ($buildExitCode -ne 0) {
    throw "PS app build failed (Ninja exit code $buildExitCode). Read the first compiler/linker error above."
}

$graph = Get-Content -LiteralPath $ninjaFile -Raw
$serviceObjectPattern = '(?m)^build CMakeFiles/' + [regex]::Escape($appName) +
                        '\.elf\.dir/[^\r\n]*pd_tcp_service\.c\.obj: C_COMPILER'
$serviceObjects = [regex]::Matches($graph, $serviceObjectPattern)
if ($serviceObjects.Count -ne 1) {
    throw "ELF was built, but generated Ninja graph still contains $($serviceObjects.Count) TCP service objects."
}
if (-not (Test-Path -LiteralPath $elfFile -PathType Leaf)) {
    throw "Ninja returned success without producing the app ELF: $elfFile"
}
$elf = Get-Item -LiteralPath $elfFile
if ($elf.Length -eq 0) {
    throw "The app ELF is empty: $elfFile"
}
$compileDatabase = @(Get-Content -LiteralPath $compileCommandsFile -Raw | ConvertFrom-Json)
foreach ($sourceName in @('pd_tcp_service.c', 'pd_spectrum.c')) {
    $entry = $compileDatabase | Where-Object {
        [System.IO.Path]::GetFileName($_.file) -eq $sourceName
    } | Select-Object -First 1
    if ($null -eq $entry) {
        throw "The generated compile database does not contain $sourceName."
    }
    $command = if ($entry.command) { $entry.command } else { $entry.arguments -join ' ' }
    $optimizationFlags = [regex]::Matches($command, '(?<!\S)-O(?:0|1|2|3|s)(?!\S)')
    if ($optimizationFlags.Count -eq 0 -or
        $optimizationFlags[$optimizationFlags.Count - 1].Value -ne '-O2') {
        throw "$sourceName did not resolve to final -O2 optimization in compile_commands.json."
    }
}
Write-Output 'PS_APP_OPTIMIZATION_PASS pd_tcp_service.c=-O2 pd_spectrum.c=-O2'
Write-Output "PS_APP_BUILD_PASS file=$($elf.FullName) bytes=$($elf.Length)"
