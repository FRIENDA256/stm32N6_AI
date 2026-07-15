param(
  [string]$HostAddress = "192.168.6.50",
  [int]$Iterations = 5,
  [int]$TimeoutMs = 90000,
  [int]$Threshold = 512,
  [int]$SettleMs = 1000
)

$ErrorActionPreference = "Stop"

if ($Iterations -lt 1) {
  throw "Iterations must be at least 1"
}

$commandScript = Join-Path $PSScriptRoot "tcp_ir.ps1"
$captureScript = Join-Path $PSScriptRoot "tcp_binary_capture.ps1"
$captureRoot = Join-Path $PSScriptRoot "net_captures\ir_ad_contention"
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
      Horizontal = [int]$jumpMatch.Groups[2].Value
      Vertical = [int]$jumpMatch.Groups[3].Value
      TotalJumps = [int]$jumpMatch.Groups[4].Value
      MaxDelta = [int]$jumpMatch.Groups[5].Value
      Repaired = if ($repairMatch.Success) { [int]$repairMatch.Groups[1].Value } else { -1 }
    })
  }
}

try {
  Write-Host "AD7606/Tiny1C contention A/B test"
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
