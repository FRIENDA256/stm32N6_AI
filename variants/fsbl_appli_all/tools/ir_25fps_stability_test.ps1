param(
  [string]$HostAddress = "192.168.6.50",
  [ValidateRange(5, 3600)]
  [int]$DurationSeconds = 30,
  [ValidateRange(0, 120)]
  [int]$WarmupSeconds = 10,
  [ValidateRange(1, 25)]
  [int]$ExpectedTempFps = 25,
  [double]$MaxBackgroundImageFps = 0.1,
  [double]$MinTempFps = 24.0,
  [int]$MaxAverageCaptureMs = 40,
  [int]$MaxDeadlineMisses = 2,
  [switch]$RequireCleanBaseline
)

$ErrorActionPreference = "Stop"
$commandScript = Join-Path $PSScriptRoot "tcp_ir.ps1"

function Send-BoardCommand {
  param([string]$Command)

  $response = & powershell -NoProfile -ExecutionPolicy Bypass `
    -File $commandScript `
    -HostAddress $HostAddress `
    -Command $Command `
    -TimeoutMs 60000
  if ($LASTEXITCODE -ne 0) {
    throw "Board command failed: $Command"
  }
  return ($response -join "`n").Trim()
}

function Parse-IrStatus {
  param([string]$Text)

  $suspendMatch = [regex]::Match($Text, '\ssusp=(\d+)')
  $match = [regex]::Match(
    $Text,
    'target=(\d+)/(\d+)\s+sck=(\d+)(?:\s+masrx=(\d+))?.*?image=(\d+)\s+temp=(\d+).*?temp_ms=(\d+).*?err=(\d+)\s+late=(\d+).*?max_ms=(\d+)\s+elapsed_ms=(\d+).*?dma_wait=(\d+)\s+dma_irq=(\d+).*?spierr=0x([0-9A-Fa-f]+)'
  )
  if (-not $match.Success) {
    throw "Unable to parse IRSTAT: $Text"
  }

  return [pscustomobject]@{
    TargetImage = [uint64]$match.Groups[1].Value
    TargetTemp = [uint64]$match.Groups[2].Value
    SckHz = [uint64]$match.Groups[3].Value
    ReportsMasterRxAutoSuspend = $match.Groups[4].Success
    MasterRxAutoSuspend = if ($match.Groups[4].Success) { [uint64]$match.Groups[4].Value } else { [uint64]0 }
    ReportsTimeoutSnapshot = ($Text -match '\ssr=0x[0-9A-Fa-f]+')
    ReportsStableDiagnosticLayout = ($Text -match '\sdiag=3\s')
    SuspendResumes = if ($suspendMatch.Success) { [uint64]$suspendMatch.Groups[1].Value } else { [uint64]0 }
    Image = [uint64]$match.Groups[5].Value
    Temp = [uint64]$match.Groups[6].Value
    TempAverageMs = [uint64]$match.Groups[7].Value
    Errors = [uint64]$match.Groups[8].Value
    Late = [uint64]$match.Groups[9].Value
    MaxMs = [uint64]$match.Groups[10].Value
    ElapsedMs = [uint64]$match.Groups[11].Value
    DmaWaitTimeouts = [uint64]$match.Groups[12].Value
    DmaIrqErrors = [uint64]$match.Groups[13].Value
    SpiError = [Convert]::ToUInt64($match.Groups[14].Value, 16)
  }
}

function Parse-AiStatus {
  param([string]$Text)

  $match = [regex]::Match($Text, 'runs=(\d+).*?run_err=(\d+)')
  if (-not $match.Success) {
    throw "Unable to parse AISTAT: $Text"
  }
  return [pscustomobject]@{
    Runs = [uint64]$match.Groups[1].Value
    Errors = [uint64]$match.Groups[2].Value
  }
}

function Parse-AdStatus {
  param([string]$Text)

  $match = [regex]::Match($Text, 'paused=(\d+)\s+idle=(\d+)\s+latest=(\d+).*?seq=(\d+)')
  if (-not $match.Success) {
    throw "Unable to parse ADSTAT: $Text"
  }
  return [pscustomobject]@{
    Paused = [uint64]$match.Groups[1].Value
    Idle = [uint64]$match.Groups[2].Value
    Latest = [uint64]$match.Groups[3].Value
    Sequence = [uint64]$match.Groups[4].Value
  }
}

function Read-StableAdStatus {
  for ($attempt = 0; $attempt -lt 10; $attempt++) {
    $text = Send-BoardCommand -Command "ADSTAT"
    $status = Parse-AdStatus $text
    if ($status.Latest -eq 1) {
      return [pscustomobject]@{ Text = $text; Status = $status }
    }
    Start-Sleep -Milliseconds 20
  }

  throw "ADSTAT did not return a valid latest frame after 10 attempts"
}

Write-Host "Tiny1C temperature stability test"
Write-Host "Target: $HostAddress, expected: ${ExpectedTempFps}fps, warmup: ${WarmupSeconds}s, measurement: ${DurationSeconds}s"

if ($WarmupSeconds -gt 0) {
  Start-Sleep -Seconds $WarmupSeconds
}

$irStartText = Send-BoardCommand -Command "IRSTAT"
$aiStartText = Send-BoardCommand -Command "AISTAT"
$adStartSnapshot = Read-StableAdStatus
$adStartText = $adStartSnapshot.Text
$irStart = Parse-IrStatus $irStartText
$aiStart = Parse-AiStatus $aiStartText
$adStart = $adStartSnapshot.Status

Write-Host $irStartText
Write-Host $aiStartText
Write-Host $adStartText

if (-not $irStart.ReportsMasterRxAutoSuspend) {
  throw "Firmware does not report masrx; flash the auto-suspend diagnostic firmware before running this test"
}
if (-not $irStart.ReportsTimeoutSnapshot) {
  throw "Firmware does not report the SPI timeout snapshot; flash the current Tiny1C diagnostic firmware before running this test"
}
if (-not $irStart.ReportsStableDiagnosticLayout) {
  throw "Firmware does not include SUSP IRQ recovery; flash the current Tiny1C diagnostic firmware before running this test"
}

Start-Sleep -Seconds $DurationSeconds

$irEndText = Send-BoardCommand -Command "IRSTAT"
$aiEndText = Send-BoardCommand -Command "AISTAT"
$adEndSnapshot = Read-StableAdStatus
$adEndText = $adEndSnapshot.Text
$irEnd = Parse-IrStatus $irEndText
$aiEnd = Parse-AiStatus $aiEndText
$adEnd = $adEndSnapshot.Status

Write-Host $irEndText
Write-Host $aiEndText
Write-Host $adEndText

$measurementMs = [int64]$irEnd.ElapsedMs - [int64]$irStart.ElapsedMs
if ($measurementMs -le 0) {
  throw "Invalid board elapsed interval: $measurementMs ms"
}
$imageDelta = [double]($irEnd.Image - $irStart.Image)
$tempDelta = [double]($irEnd.Temp - $irStart.Temp)
$errorDelta = [uint64]($irEnd.Errors - $irStart.Errors)
$lateDelta = [uint64]($irEnd.Late - $irStart.Late)
$dmaWaitDelta = [uint64]($irEnd.DmaWaitTimeouts - $irStart.DmaWaitTimeouts)
$suspendResumeDelta = [int64]$irEnd.SuspendResumes - [int64]$irStart.SuspendResumes
$aiRunDelta = [uint64]($aiEnd.Runs - $aiStart.Runs)
$aiErrorDelta = [uint64]($aiEnd.Errors - $aiStart.Errors)
$adSequenceDelta = [int64]$adEnd.Sequence - [int64]$adStart.Sequence
$imageFps = ($imageDelta * 1000.0) / $measurementMs
$tempFps = ($tempDelta * 1000.0) / $measurementMs

$checks = [ordered]@{
  Configuration = (($irEnd.TargetImage -eq 0) -and ($irEnd.TargetTemp -eq $ExpectedTempFps) -and
                   ($irEnd.SckHz -eq 50000000) -and ($irEnd.MasterRxAutoSuspend -eq 1))
  CleanBaseline = ((-not $RequireCleanBaseline) -or
                   (($irStart.Errors -eq 0) -and ($irStart.DmaWaitTimeouts -eq 0) -and
                    ($irStart.DmaIrqErrors -eq 0) -and ($irStart.SpiError -eq 0)))
  ImageDisabled = ($imageFps -le $MaxBackgroundImageFps)
  TemperatureRate = ($tempFps -ge $MinTempFps)
  CaptureErrors = ($errorDelta -eq 0)
  DmaWaitTimeouts = ($dmaWaitDelta -eq 0)
  DeadlineMisses = ($lateDelta -le $MaxDeadlineMisses)
  CaptureLatency = ($irEnd.TempAverageMs -le $MaxAverageCaptureMs)
  AdRunning = (($adEnd.Paused -eq 0) -and ($adSequenceDelta -gt 0))
  AiRunning = (($aiRunDelta -gt 0) -and ($aiErrorDelta -eq 0))
}

Write-Host ""
Write-Host ("Measured interval={0:N3}s image={1:N2} fps temp={2:N2} fps capture_err_delta={3} late_delta={4} temp_avg_ms={5} max_ms={6}" -f `
  ($measurementMs / 1000.0), $imageFps, $tempFps, $errorDelta, $lateDelta, $irEnd.TempAverageMs, $irEnd.MaxMs)
Write-Host ("AD seq delta={0}; AI runs delta={1}, run_err delta={2}" -f `
  $adSequenceDelta, $aiRunDelta, $aiErrorDelta)
Write-Host ("SUSP resume delta={0}; DMA wait delta={1}; baseline err={2}, dma_wait={3}, dma_irq={4}, spierr=0x{5:X8}; strict={6}" -f `
  $suspendResumeDelta, $dmaWaitDelta, $irStart.Errors, $irStart.DmaWaitTimeouts,
  $irStart.DmaIrqErrors, $irStart.SpiError, [bool]$RequireCleanBaseline)

$failed = @()
foreach ($entry in $checks.GetEnumerator()) {
  $state = if ($entry.Value) { "PASS" } else { "FAIL" }
  Write-Host ("{0,-18} {1}" -f $entry.Key, $state)
  if (-not $entry.Value) {
    $failed += $entry.Key
  }
}

if ($failed.Count -ne 0) {
  throw "Tiny1C temperature stability test failed: $($failed -join ', ')"
}

Write-Host "Tiny1C temperature stability test passed."
