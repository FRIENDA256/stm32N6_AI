param(
  [string]$HostAddress = "192.168.6.50",
  [ValidateRange(5, 3600)]
  [int]$DurationSeconds = 180,
  [ValidateRange(0, 120)]
  [int]$WarmupSeconds = 10,
  [switch]$RequireCleanBaseline
)

$ErrorActionPreference = "Stop"
$testScript = Join-Path $PSScriptRoot "ir_25fps_stability_test.ps1"
$parameters = @{
  HostAddress = $HostAddress
  DurationSeconds = $DurationSeconds
  WarmupSeconds = $WarmupSeconds
  ExpectedTempFps = 20
  MinTempFps = 19.0
  MaxAverageCaptureMs = 48
}
if ($RequireCleanBaseline) {
  $parameters.RequireCleanBaseline = $true
}

& $testScript @parameters
