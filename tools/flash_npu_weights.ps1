param(
  [string]$Programmer = "D:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe",
  [string]$ExternalLoader = "D:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr",
  [string]$Weights = "variants\fsbl_appli_lrun\tiny_temporal_mixer_8ch_int8_atonbuf.xSPI2.raw",
  [string]$Address = "0x71000000"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$WeightsPath = if ([System.IO.Path]::IsPathRooted($Weights)) {
  $Weights
} else {
  Join-Path $Root $Weights
}

if (-not (Test-Path -LiteralPath $Programmer)) {
  throw "STM32_Programmer_CLI.exe not found: $Programmer"
}

if (-not (Test-Path -LiteralPath $ExternalLoader)) {
  throw "External loader not found: $ExternalLoader"
}

if (-not (Test-Path -LiteralPath $WeightsPath)) {
  throw "NPU weights raw not found: $WeightsPath"
}

$DownloadPath = $WeightsPath
$TempBinPath = $null
if ([System.IO.Path]::GetExtension($WeightsPath) -ieq ".raw") {
  $TempBinPath = Join-Path ([System.IO.Path]::GetTempPath()) (
    [System.IO.Path]::GetFileNameWithoutExtension($WeightsPath) + ".bin"
  )
  Copy-Item -LiteralPath $WeightsPath -Destination $TempBinPath -Force
  $DownloadPath = $TempBinPath
}

Write-Host "Programming NPU weights"
Write-Host "  raw:     $WeightsPath"
if ($TempBinPath) {
  Write-Host "  bin:     $TempBinPath"
}
Write-Host "  address: $Address"
Write-Host "  loader:  $ExternalLoader"

& $Programmer `
  -c port=SWD mode=HOTPLUG `
  -el $ExternalLoader `
  -w $DownloadPath $Address `
  -v

if ($LASTEXITCODE -ne 0) {
  throw "STM32_Programmer_CLI failed with exit code $LASTEXITCODE"
}
