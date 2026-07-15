param(
  [string]$HostAddress = "192.168.6.50",
  [int]$Iterations = 3,
  [int]$IdleMs = 30000,
  [int]$TimeoutMs = 90000,
  [int]$Threshold = 512,
  [switch]$RestartPreview
)

$ErrorActionPreference = "Stop"

if ($Iterations -lt 1) {
  throw "Iterations must be at least 1"
}
if ($IdleMs -lt 0) {
  throw "IdleMs cannot be negative"
}

$commandScript = Join-Path $PSScriptRoot "tcp_ir.ps1"
$captureScript = Join-Path $PSScriptRoot "tcp_binary_capture.ps1"
$captureRoot = Join-Path $PSScriptRoot "net_captures\ir_capture_idle"
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
    Write-Host "[$Mode] temperature capture $index/$Iterations"
    $output = & powershell -NoProfile -ExecutionPolicy Bypass `
      -File $captureScript `
      -Command IRGETTEMP `
      -HostAddress $HostAddress `
      -TimeoutMs $TimeoutMs `
      -OutputDir $OutputDir `
      -TempFilter despeckle `
      -TempDespikeThreshold $Threshold 2>&1
    if ($LASTEXITCODE -ne 0) {
      $output | ForEach-Object { Write-Host $_ }
      throw "Temperature capture failed in mode $Mode"
    }

    $output | ForEach-Object { Write-Host $_ }
    $text = $output -join "`n"
    $jumpMatch = [regex]::Match(
      $text,
      'Temp raw jumps: threshold=(\d+) horizontal=(\d+) vertical=(\d+) total=(\d+) max_delta=(\d+) comparisons=(\d+)'
    )
    $repairMatch = [regex]::Match($text, 'Temp filter: .* repaired=(\d+)')
    if (-not $jumpMatch.Success) {
      throw "Capture output did not contain raw jump metrics"
    }

    $results.Add([pscustomobject]@{
      Mode = $Mode
      Capture = $index
      TotalJumps = [int]$jumpMatch.Groups[4].Value
      MaxDelta = [int]$jumpMatch.Groups[5].Value
      Repaired = if ($repairMatch.Success) { [int]$repairMatch.Groups[1].Value } else { -1 }
    })
  }
}

try {
  Write-Host "Tiny1C background-acquisition idle A/B test"
  Write-Host "Target: $HostAddress, captures per mode: $Iterations, idle wait: ${IdleMs}ms"
  Write-Host "Restart preview after idle: $RestartPreview"

  [void](Send-BoardCommand -Command "IRRESUME")
  Capture-TestGroup -Mode "IR_RUNNING" -OutputDir (Join-Path $captureRoot "ir_running")

  [void](Send-BoardCommand -Command "IRPAUSE")
  $pauseStatus = Send-BoardCommand -Command "IRSTAT"
  if ($pauseStatus -notmatch 'pause=1\s+active=0') {
    throw "Tiny1C background acquisition did not reach paused idle state"
  }

  Write-Host "Waiting ${IdleMs}ms with background acquisition paused..."
  Start-Sleep -Milliseconds $IdleMs

  $comparisonMode = "IR_PAUSED_IDLE"
  $comparisonDir = "ir_paused_idle"
  if ($RestartPreview) {
    [void](Send-BoardCommand -Command "IRRESTART")
    $pauseStatus = Send-BoardCommand -Command "IRSTAT"
    if ($pauseStatus -notmatch 'pause=1\s+active=0') {
      throw "IRRESTART unexpectedly resumed background acquisition"
    }
    $comparisonMode = "IR_RESTARTED"
    $comparisonDir = "ir_restarted"
  }

  Capture-TestGroup -Mode $comparisonMode -OutputDir (Join-Path $captureRoot $comparisonDir)

  $pauseStatus = Send-BoardCommand -Command "IRSTAT"
  if ($pauseStatus -notmatch 'pause=1\s+active=0') {
    throw "IRGETTEMP unexpectedly resumed background acquisition"
  }
}
finally {
  Write-Host ""
  Write-Host "Restoring Tiny1C background acquisition..."
  try {
    [void](Send-BoardCommand -Command "IRRESUME")
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
  $repairMeasure = $_.Group.Repaired | Measure-Object -Minimum -Maximum -Average
  [pscustomobject]@{
    Mode = $_.Name
    Frames = $_.Count
    JumpAvg = [Math]::Round($jumpMeasure.Average, 1)
    JumpMin = $jumpMeasure.Minimum
    JumpMax = $jumpMeasure.Maximum
    RepairAvg = [Math]::Round($repairMeasure.Average, 1)
    RepairMin = $repairMeasure.Minimum
    RepairMax = $repairMeasure.Maximum
  }
} | Format-Table -AutoSize

Write-Host "Raw frames are saved under: $captureRoot"
