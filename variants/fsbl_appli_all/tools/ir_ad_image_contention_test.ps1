param(
  [string]$HostAddress = "192.168.6.50",
  [int]$Iterations = 10,
  [int]$TimeoutMs = 90000,
  [ValidateRange(1, 255)]
  [int]$Threshold = 48,
  [int]$SettleMs = 2000
)

$ErrorActionPreference = "Stop"

if ($Iterations -lt 1) {
  throw "Iterations must be at least 1"
}

$commandScript = Join-Path $PSScriptRoot "tcp_ir.ps1"
$captureScript = Join-Path $PSScriptRoot "tcp_binary_capture.ps1"
$captureRoot = Join-Path $PSScriptRoot "net_captures\ir_ad_image_contention"
$results = [System.Collections.Generic.List[object]]::new()

function Send-BoardCommand {
  param([string]$Command)

  $response = & powershell -NoProfile -ExecutionPolicy Bypass `
    -File $commandScript `
    -Command $Command `
    -HostAddress $HostAddress `
    -TimeoutMs 10000
  if ($LASTEXITCODE -ne 0) {
    throw "Board command failed: $Command"
  }
  $response | ForEach-Object { Write-Host $_ }
  return ($response -join "`n")
}

function Capture-TestGroup {
  param(
    [string]$Mode,
    [string]$OutputDir
  )

  New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
  for ($index = 1; $index -le $Iterations; $index++) {
    Write-Host ""
    Write-Host "[$Mode] image capture $index/$Iterations"
    $output = & powershell -NoProfile -ExecutionPolicy Bypass `
      -File $captureScript `
      -Command IRGETIMG `
      -HostAddress $HostAddress `
      -TimeoutMs $TimeoutMs `
      -OutputDir $OutputDir `
      -ImageSpeckleThreshold $Threshold 2>&1
    if ($LASTEXITCODE -ne 0) {
      $output | ForEach-Object { Write-Host $_ }
      throw "Image capture failed in mode $Mode"
    }

    $output | ForEach-Object { Write-Host $_ }
    $text = $output -join "`n"
    $noiseMatch = [regex]::Match(
      $text,
      'Image spatial noise: threshold=(\d+) horizontal=(\d+) vertical=(\d+) total=(\d+) max_delta=(\d+) dark=(\d+) bright=(\d+) dark_max=(\d+) dark_xy=(-?\d+),(-?\d+) bright_max=(\d+) bright_xy=(-?\d+),(-?\d+)'
    )
    if (-not $noiseMatch.Success) {
      throw "Capture output did not contain image noise metrics"
    }

    $results.Add([pscustomobject]@{
      Mode = $Mode
      Capture = $index
      TotalJumps = [int]$noiseMatch.Groups[4].Value
      MaxDelta = [int]$noiseMatch.Groups[5].Value
      Dark = [int]$noiseMatch.Groups[6].Value
      Bright = [int]$noiseMatch.Groups[7].Value
      DarkMax = [int]$noiseMatch.Groups[8].Value
      DarkXY = "$($noiseMatch.Groups[9].Value),$($noiseMatch.Groups[10].Value)"
      BrightMax = [int]$noiseMatch.Groups[11].Value
      BrightXY = "$($noiseMatch.Groups[12].Value),$($noiseMatch.Groups[13].Value)"
    })
  }
}

try {
  Write-Host "AD7606/Tiny1C image contention A/B test"
  Write-Host "Target: $HostAddress, captures per mode: $Iterations, threshold: $Threshold"

  [void](Send-BoardCommand -Command "ADRESUME")
  Start-Sleep -Milliseconds $SettleMs
  Capture-TestGroup -Mode "AD_RUNNING" -OutputDir (Join-Path $captureRoot "ad_running")

  [void](Send-BoardCommand -Command "ADPAUSE")
  Start-Sleep -Milliseconds $SettleMs
  $pauseStatus = Send-BoardCommand -Command "ADSTAT"
  if ($pauseStatus -notmatch 'paused=1\s+idle=1') {
    throw "AD7606 receiver did not reach paused idle state"
  }
  Capture-TestGroup -Mode "AD_PAUSED" -OutputDir (Join-Path $captureRoot "ad_paused")
}
finally {
  Write-Host ""
  Write-Host "Restoring AD7606 receiver..."
  try {
    [void](Send-BoardCommand -Command "ADRESUME")
  }
  catch {
    Write-Warning $_
  }
}

Write-Host ""
Write-Host "Per-frame results:"
$results | Format-Table -AutoSize

Write-Host "Summary:"
$results | Group-Object Mode | ForEach-Object {
  $jumpMeasure = $_.Group.TotalJumps | Measure-Object -Minimum -Maximum -Average
  $darkMeasure = $_.Group.Dark | Measure-Object -Minimum -Maximum -Average
  $brightMeasure = $_.Group.Bright | Measure-Object -Minimum -Maximum -Average
  [pscustomobject]@{
    Mode = $_.Name
    Frames = $_.Count
    JumpAvg = [Math]::Round($jumpMeasure.Average, 1)
    JumpMin = $jumpMeasure.Minimum
    JumpMax = $jumpMeasure.Maximum
    DarkAvg = [Math]::Round($darkMeasure.Average, 1)
    DarkMin = $darkMeasure.Minimum
    DarkMax = $darkMeasure.Maximum
    BrightAvg = [Math]::Round($brightMeasure.Average, 1)
    BrightMin = $brightMeasure.Minimum
    BrightMax = $brightMeasure.Maximum
  }
} | Format-Table -AutoSize

Write-Host "Raw frames are saved under: $captureRoot"
