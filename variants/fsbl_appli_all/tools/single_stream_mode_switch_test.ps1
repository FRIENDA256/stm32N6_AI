param(
  [string]$HostAddress = "192.168.6.50",
  [int]$SettleSeconds = 5,
  [int]$TimeoutMs = 10000
)

$ErrorActionPreference = "Stop"
$tcpTool = Join-Path $PSScriptRoot "tcp_ir.ps1"

function Invoke-BoardCommand {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Command
  )

  $output = & powershell -NoProfile -ExecutionPolicy Bypass `
    -File $tcpTool `
    -Command $Command `
    -HostAddress $HostAddress `
    -TimeoutMs $TimeoutMs

  if ($LASTEXITCODE -ne 0) {
    throw "Command failed: $Command"
  }

  return (($output | Out-String).Trim())
}

function Get-Counter {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Text,
    [Parameter(Mandatory = $true)]
    [string]$Name
  )

  $match = [regex]::Match($Text, "(?:^|\s)$([regex]::Escape($Name))=(\d+)(?:\s|$)")
  if (-not $match.Success) {
    throw "Unable to parse '$Name' from: $Text"
  }
  return [uint64]$match.Groups[1].Value
}

function Read-StreamCounters {
  $telemetry = Invoke-BoardCommand -Command "STREAM STAT"
  $infrared = Invoke-BoardCommand -Command "IRSTREAM STAT"
  $irCapture = Invoke-BoardCommand -Command "IRSTAT"
  $ad = Invoke-BoardCommand -Command "ADSTAT"
  $ai = Invoke-BoardCommand -Command "AISTAT"

  return [pscustomobject]@{
    Telemetry = Get-Counter -Text $telemetry -Name "msg"
    Infrared = Get-Counter -Text $infrared -Name "msg"
    IrCapture = Get-Counter -Text $irCapture -Name "temp"
    AdRaw = Get-Counter -Text $ad -Name "raw"
    AiRuns = Get-Counter -Text $ai -Name "runs"
    TelemetryText = $telemetry
    InfraredText = $infrared
    IrCaptureText = $irCapture
    AdText = $ad
    AiText = $ai
  }
}

function Assert-Mode {
  param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("ADAI", "IR")]
    [string]$Mode
  )

  $response = Invoke-BoardCommand -Command "MODE $Mode"
  if ($response -notmatch "^OK MODE=$Mode") {
    throw "Mode switch failed: $response"
  }
  Write-Host $response
}

Write-Host "Single-stream mode switch test"
Write-Host "Target: $HostAddress, settle: ${SettleSeconds}s"

$ping = Invoke-BoardCommand -Command "PING"
if ($ping -notmatch "^PONG") {
  throw "Unexpected PING response: $ping"
}

$testFailure = $null
$restoreFailure = $null

try {
  Assert-Mode -Mode "ADAI"
  $adaiStart = Read-StreamCounters
  Start-Sleep -Seconds $SettleSeconds
  $adaiEnd = Read-StreamCounters

  $adaiTelemetryDelta = $adaiEnd.Telemetry - $adaiStart.Telemetry
  $adaiInfraredDelta = $adaiEnd.Infrared - $adaiStart.Infrared
  $adaiIrCaptureDelta = $adaiEnd.IrCapture - $adaiStart.IrCapture
  $adaiAdDelta = $adaiEnd.AdRaw - $adaiStart.AdRaw
  $adaiAiDelta = $adaiEnd.AiRuns - $adaiStart.AiRuns
  if (($adaiTelemetryDelta -eq 0) -or
      ($adaiInfraredDelta -ne 0) -or
      ($adaiIrCaptureDelta -gt 1) -or
      ($adaiAdDelta -eq 0) -or
      ($adaiAiDelta -eq 0)) {
    throw "ADAI gate failed: telemetry delta=$adaiTelemetryDelta, IR delta=$adaiInfraredDelta, IR capture delta=$adaiIrCaptureDelta, AD delta=$adaiAdDelta, AI delta=$adaiAiDelta"
  }
  Write-Host "ADAI PASS telemetry_delta=$adaiTelemetryDelta ir_delta=$adaiInfraredDelta ir_capture_delta=$adaiIrCaptureDelta ad_delta=$adaiAdDelta ai_delta=$adaiAiDelta"

  Assert-Mode -Mode "IR"
  $irStart = Read-StreamCounters
  Start-Sleep -Seconds $SettleSeconds
  $irEnd = Read-StreamCounters

  $irTelemetryDelta = $irEnd.Telemetry - $irStart.Telemetry
  $irInfraredDelta = $irEnd.Infrared - $irStart.Infrared
  $irIrCaptureDelta = $irEnd.IrCapture - $irStart.IrCapture
  $irAdDelta = $irEnd.AdRaw - $irStart.AdRaw
  $irAiDelta = $irEnd.AiRuns - $irStart.AiRuns
  if (($irTelemetryDelta -ne 0) -or
      ($irInfraredDelta -eq 0) -or
      ($irIrCaptureDelta -eq 0) -or
      ($irAdDelta -ne 0) -or
      ($irAiDelta -gt 1)) {
    throw "IR gate failed: telemetry delta=$irTelemetryDelta, IR delta=$irInfraredDelta, IR capture delta=$irIrCaptureDelta, AD delta=$irAdDelta, AI delta=$irAiDelta"
  }
  Write-Host "IR PASS telemetry_delta=$irTelemetryDelta ir_delta=$irInfraredDelta ir_capture_delta=$irIrCaptureDelta ad_delta=$irAdDelta ai_delta=$irAiDelta"
}
catch {
  $testFailure = $_
  Write-Warning "Test body failed: $($_.Exception.Message)"
}
finally {
  try {
    Assert-Mode -Mode "ADAI"
  }
  catch {
    $restoreFailure = $_
    Write-Warning "Unable to restore ADAI mode: $($_.Exception.Message)"
  }
}

if ($null -ne $testFailure) {
  throw $testFailure
}
if ($null -ne $restoreFailure) {
  throw $restoreFailure
}

Write-Host "Single-stream mode switch test passed."
