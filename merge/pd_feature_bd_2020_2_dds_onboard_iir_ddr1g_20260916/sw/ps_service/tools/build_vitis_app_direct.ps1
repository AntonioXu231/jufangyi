# Build the existing Vitis 2024.1 PS application without the Unified IDE task UI.
# Run after closing Vitis:
#   & 'F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916\sw\ps_service\tools\build_vitis_app_direct.ps1' -CheckOnly
#   & 'F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916\sw\ps_service\tools\build_vitis_app_direct.ps1'
# Expected output: F:\ps\lwip_echo_server9\build\lwip_echo_server9.elf
# Next: download that ELF to the Zynq and check CONFIG/STATUS over TCP.

[CmdletBinding()]
param(
    [string]$WorkspaceRoot = 'F:\ps',
    [switch]$CheckOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$appName = 'lwip_echo_server9'
$appDir = Join-Path $WorkspaceRoot $appName
$srcDir = Join-Path $appDir 'src'
$buildDir = Join-Path $appDir 'build'
$cmakeFile = Join-Path $srcDir 'CMakeLists.txt'
$serviceFile = Join-Path $srcDir 'pd_tcp_service.c'
$cacheFile = Join-Path $buildDir 'CMakeCache.txt'
$ninjaFile = Join-Path $buildDir 'build.ninja'
$elfFile = Join-Path $buildDir "$appName.elf"
$platformStatus = Join-Path $WorkspaceRoot 'platform4\export\.buildstatus'
$platformLib = Join-Path $WorkspaceRoot 'platform4\export\platform4\sw\standalone_ps7_cortexa9_0\lib\libxil.a'

foreach ($file in @($cmakeFile, $serviceFile, $cacheFile, $ninjaFile,
                    $platformStatus, $platformLib)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required file is missing: $file"
    }
}
if ((Get-Content -LiteralPath $platformStatus -Raw) -notmatch '(?m)^export=SUCCESS\s*$') {
    throw "platform4 has not exported successfully: $platformStatus"
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
Write-Output "APP=$appName PLATFORM=platform4 MODULES=$($moduleIncludes.Count) CMAKE_SERVICE_ENTRIES=$($serviceEntries.Count) CMAKE_REGEN_PENDING=$cmakePending"
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
$serviceObjects = [regex]::Matches($graph,
    '(?m)^build CMakeFiles/lwip_echo_server9\.elf\.dir/[^\r\n]*pd_tcp_service\.c\.obj: C_COMPILER')
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
Write-Output "PS_APP_BUILD_PASS file=$($elf.FullName) bytes=$($elf.Length)"
