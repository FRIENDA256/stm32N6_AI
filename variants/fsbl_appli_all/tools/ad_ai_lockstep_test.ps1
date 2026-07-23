param(
  [string]$HostAddress = "192.168.6.50",
  [ValidateRange(10, 3600)]
  [int]$DurationSeconds = 60,
  [ValidateRange(0, 120)]
  [int]$WarmupSeconds = 10,
  [ValidateRange(1.0, 50.0)]
  [double]$MinFrameRate = 48.0,
  [ValidateRange(0, 8)]
  [int]$SnapshotTolerance = 2,
  [ValidateRange(0, 32)]
  [int]$MaxInputQueueDepth = 4,
  [switch]$PauseInfrared
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

function Parse-AdStatus {
  param([string]$Text)

  $match = [regex]::Match(
    $Text,
    'raw=(\d+)\s+enq=(\d+)\s+deq=(\d+)\s+q=(\d+)/(\d+)\s+qov=(\d+)\s+src_gap=(\d+)\s+block_gap=(\d+)'
  )
  if (-not $match.Success) {
    throw "Unable to parse lockstep ADSTAT fields: $Text"
  }

  return [pscustomobject]@{
    Raw = [int64]$match.Groups[1].Value
    Enqueued = [int64]$match.Groups[2].Value
    Dequeued = [int64]$match.Groups[3].Value
    QueueDepth = [int64]$match.Groups[4].Value
    QueueHighWater = [int64]$match.Groups[5].Value
    QueueOverflow = [int64]$match.Groups[6].Value
    SourceGap = [int64]$match.Groups[7].Value
    BlockGap = [int64]$match.Groups[8].Value
  }
}

function Parse-AiStatus {
  param([string]$Text)

  $match = [regex]::Match(
    $Text,
    'runs=(\d+).*?in=(\d+)\s+warm=(\d+)\s+src_gap=(\d+)\s+inf_gap=(\d+)\s+copy_err=(\d+)\s+run_err=(\d+)\s+late=(\d+).*?outq=(\d+)/(\d+)\s+oqov=(\d+).*?target_hz=(\d+)'
  )
  if (-not $match.Success) {
    throw "Unable to parse lockstep AISTAT fields: $Text"
  }

  return [pscustomobject]@{
    Runs = [int64]$match.Groups[1].Value
    Inputs = [int64]$match.Groups[2].Value
    Warmups = [int64]$match.Groups[3].Value
    SourceGap = [int64]$match.Groups[4].Value
    InferenceGap = [int64]$match.Groups[5].Value
    CopyErrors = [int64]$match.Groups[6].Value
    RunErrors = [int64]$match.Groups[7].Value
    DeadlineMisses = [int64]$match.Groups[8].Value
    OutputQueueDepth = [int64]$match.Groups[9].Value
    OutputQueueHighWater = [int64]$match.Groups[10].Value
    OutputQueueOverflow = [int64]$match.Groups[11].Value
    TargetHz = [int64]$match.Groups[12].Value
  }
}

function Delta {
  param([int64]$End, [int64]$Start)
  return $End - $Start
}

$infraredPaused = $false

try {
  Write-Host "AD7606 -> AI 50 fps lockstep test"
  Write-Host "Target: $HostAddress, warmup: ${WarmupSeconds}s, measurement: ${DurationSeconds}s"

  if ($PauseInfrared) {
    Write-Host (Send-BoardCommand -Command "IRPAUSE")
    $infraredPaused = $true
  }

  if ($WarmupSeconds -gt 0) {
    Start-Sleep -Seconds $WarmupSeconds
  }

  $adStartText = Send-BoardCommand -Command "ADSTAT"
  $aiStartText = Send-BoardCommand -Command "AISTAT"
  $adStart = Parse-AdStatus $adStartText
  $aiStart = Parse-AiStatus $aiStartText

  Write-Host $adStartText
  Write-Host $aiStartText

  Start-Sleep -Seconds $DurationSeconds

  $adEndText = Send-BoardCommand -Command "ADSTAT"
  $aiEndText = Send-BoardCommand -Command "AISTAT"
  $adEnd = Parse-AdStatus $adEndText
  $aiEnd = Parse-AiStatus $aiEndText

  Write-Host $adEndText
  Write-Host $aiEndText

  $rawDelta = Delta $adEnd.Raw $adStart.Raw
  $enqueueDelta = Delta $adEnd.Enqueued $adStart.Enqueued
  $dequeueDelta = Delta $adEnd.Dequeued $adStart.Dequeued
  $queueOverflowDelta = Delta $adEnd.QueueOverflow $adStart.QueueOverflow
  $sourceGapDelta = Delta $adEnd.SourceGap $adStart.SourceGap
  $blockGapDelta = Delta $adEnd.BlockGap $adStart.BlockGap

  $runDelta = Delta $aiEnd.Runs $aiStart.Runs
  $inputDelta = Delta $aiEnd.Inputs $aiStart.Inputs
  $warmupDelta = Delta $aiEnd.Warmups $aiStart.Warmups
  $aiSourceGapDelta = Delta $aiEnd.SourceGap $aiStart.SourceGap
  $inferenceGapDelta = Delta $aiEnd.InferenceGap $aiStart.InferenceGap
  $copyErrorDelta = Delta $aiEnd.CopyErrors $aiStart.CopyErrors
  $runErrorDelta = Delta $aiEnd.RunErrors $aiStart.RunErrors
  $outputOverflowDelta = Delta $aiEnd.OutputQueueOverflow $aiStart.OutputQueueOverflow

  $rawRate = $rawDelta / [double]$DurationSeconds
  $runRate = $runDelta / [double]$DurationSeconds
  $rawRunDifference = [math]::Abs($rawDelta - $runDelta)
  $dequeueInputDifference = [math]::Abs($dequeueDelta - $inputDelta)
  $inputAccountingDifference = [math]::Abs(
    $inputDelta - ($runDelta + $warmupDelta + $runErrorDelta)
  )

  $checks = [ordered]@{
    FirmwareTarget = ($aiEnd.TargetHz -eq 50)
    AdFrameRate = ($rawRate -ge $MinFrameRate)
    AiFrameRate = ($runRate -ge $MinFrameRate)
    AdQueueConservation = (($enqueueDelta + $queueOverflowDelta) -eq $rawDelta)
    DequeueMatchesAiInput = ($dequeueInputDifference -le $SnapshotTolerance)
    InputAccounting = ($inputAccountingDifference -le 1)
    AdAiLockstep = ($rawRunDifference -le $SnapshotTolerance)
    AdQueueOverflow = ($queueOverflowDelta -eq 0)
    AdQueueDepth = ($adEnd.QueueDepth -le $MaxInputQueueDepth)
    SourceSequence = ($sourceGapDelta -eq 0)
    SourceSampleBlocks = ($blockGapDelta -eq 0)
    AiSourceSequence = ($aiSourceGapDelta -eq 0)
    InferenceSequence = ($inferenceGapDelta -eq 0)
    CopyErrors = ($copyErrorDelta -eq 0)
    RunErrors = ($runErrorDelta -eq 0)
    OutputQueueOverflow = ($outputOverflowDelta -eq 0)
  }

  Write-Host ""
  Write-Host ("AD raw delta={0}, enqueued={1}, dequeued={2}, rate={3:N2} fps, q={4}/{5}, qov={6}" -f `
    $rawDelta, $enqueueDelta, $dequeueDelta, $rawRate,
    $adEnd.QueueDepth, $adEnd.QueueHighWater, $queueOverflowDelta)
  Write-Host ("AI input delta={0}, runs={1}, warmups={2}, rate={3:N2} fps, late={4}, outq={5}/{6}, oqov={7}" -f `
    $inputDelta, $runDelta, $warmupDelta, $runRate,
    (Delta $aiEnd.DeadlineMisses $aiStart.DeadlineMisses),
    $aiEnd.OutputQueueDepth, $aiEnd.OutputQueueHighWater, $outputOverflowDelta)
  Write-Host ("Snapshot differences: raw-runs={0}, dequeue-input={1}, input-accounting={2}" -f `
    $rawRunDifference, $dequeueInputDifference, $inputAccountingDifference)

  $failed = @()
  foreach ($entry in $checks.GetEnumerator()) {
    $state = if ($entry.Value) { "PASS" } else { "FAIL" }
    Write-Host ("{0,-22} {1}" -f $entry.Key, $state)
    if (-not $entry.Value) {
      $failed += $entry.Key
    }
  }

  if ($failed.Count -ne 0) {
    throw "AD/AI lockstep test failed: $($failed -join ', ')"
  }

  Write-Host "AD7606 -> AI lockstep test passed."
}
finally {
  if ($infraredPaused) {
    try {
      Write-Host (Send-BoardCommand -Command "IRRESUME")
    }
    catch {
      Write-Warning "IRRESUME failed: $($_.Exception.Message)"
    }
  }
}
