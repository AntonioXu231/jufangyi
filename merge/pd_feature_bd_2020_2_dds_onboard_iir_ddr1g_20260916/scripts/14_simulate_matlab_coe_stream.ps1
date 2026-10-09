param(
    [string]$VivadoRoot = 'F:\vivado20\Vivado\2020.2',
    [string]$WorkRoot = 'F:\xinya\v5\.codex_tmp'
)

$ErrorActionPreference = 'Stop'
$projectDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$rtlDir = Join-Path $projectDir 'pd_feature_bd_2020_2.srcs\sources_1\imports\rtl'
$ipDir = Join-Path $projectDir 'pd_feature_bd_2020_2.srcs\sources_1\ip'
$simModel = Join-Path $ipDir 'pd_pd_template_rom_ch0\simulation\blk_mem_gen_v8_4.v'
$romNames = 0..3 | ForEach-Object { "pd_pd_template_rom_ch$_" }
$romModels = $romNames | ForEach-Object { Join-Path $ipDir "$_\sim\$_.v" }
$schedule = Join-Path $rtlDir 'pd_pd_event_schedule.v'
$source = Join-Path $rtlDir 'pd_dds_adc_source.v'
$testbench = Join-Path $projectDir 'sim\tb_pd_dds_matlab_template.v'
$xvlog = Join-Path $VivadoRoot 'bin\xvlog.bat'
$xelab = Join-Path $VivadoRoot 'bin\xelab.bat'
$xsim = Join-Path $VivadoRoot 'bin\xsim.bat'
$mifFiles = $romNames | ForEach-Object { Join-Path $ipDir "$_\$_.mif" }

foreach ($required in @($simModel, $schedule, $source, $testbench, $xvlog, $xelab, $xsim) + $romModels + $mifFiles) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required simulation input missing: $required. Run MATLAB generator, then source scripts/13_configure_matlab_coe_roms.tcl in Vivado 2020.2."
    }
}

$simParent = Join-Path $WorkRoot 'pd_coe_stream_xsim'
New-Item -ItemType Directory -Path $simParent -Force | Out-Null
$simDir = Join-Path $simParent (Get-Date -Format 'yyyyMMdd_HHmmss')
New-Item -ItemType Directory -Path $simDir | Out-Null
$env:TEMP = $simDir
$env:TMP = $simDir
Copy-Item -LiteralPath $mifFiles -Destination $simDir
$xsimIni = Join-Path $VivadoRoot 'data\xsim\xsim.ini'
if (Test-Path -LiteralPath $xsimIni -PathType Leaf) {
    Copy-Item -LiteralPath $xsimIni -Destination $simDir
}

Push-Location $simDir
try {
    $compileSources = @($simModel) + $romModels + @($schedule, $source, $testbench)
    & $xvlog --relax --work xil_defaultlib @compileSources
    if ($LASTEXITCODE -ne 0) { throw "xvlog failed with exit code $LASTEXITCODE" }

    & $xelab --relax --debug typical -s tb_pd_dds_matlab_template xil_defaultlib.tb_pd_dds_matlab_template
    if ($LASTEXITCODE -ne 0) { throw "xelab failed with exit code $LASTEXITCODE" }

    & $xsim tb_pd_dds_matlab_template --runall
    if ($LASTEXITCODE -ne 0) { throw "xsim failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

Write-Host "PD_COE_STREAM_SIM_PASS workdir=$simDir"
