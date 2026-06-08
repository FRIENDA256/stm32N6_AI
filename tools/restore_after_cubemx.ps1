param(
  [string]$VariantRoot
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

if ([string]::IsNullOrWhiteSpace($VariantRoot)) {
  $VariantRoot = Join-Path $repoRoot "variants\fsbl_appli_lrun"
}

$variantPath = Resolve-Path -LiteralPath $VariantRoot
$makefilePath = Join-Path $variantPath "Makefile\Appli\Makefile"
$mainPath = Join-Path $variantPath "Appli\Core\Src\main.c"

function Add-Line-After {
  param(
    [string[]]$Lines,
    [string]$Anchor,
    [string]$LineToAdd
  )

  if ($Lines -contains $LineToAdd) {
    return $Lines
  }

  $out = New-Object System.Collections.Generic.List[string]
  $inserted = $false
  foreach ($line in $Lines) {
    $out.Add($line)
    if (-not $inserted -and $line -eq $Anchor) {
      $out.Add($LineToAdd)
      $inserted = $true
    }
  }

  if (-not $inserted) {
    throw "Anchor not found: $Anchor"
  }

  return $out.ToArray()
}

if (-not (Test-Path -LiteralPath $makefilePath)) {
  throw "Makefile not found: $makefilePath"
}

$makefileLines = Get-Content -LiteralPath $makefilePath
$makefileLines = Add-Line-After `
  -Lines $makefileLines `
  -Anchor "../../Appli/Core/Src/main.c \" `
  -LineToAdd "../../Appli/Core/Src/app_main.c \"
Set-Content -LiteralPath $makefilePath -Value $makefileLines -Encoding ASCII
Write-Host "Checked Appli Makefile custom sources."

if (-not (Test-Path -LiteralPath $mainPath)) {
  throw "main.c not found: $mainPath"
}

$mainText = Get-Content -LiteralPath $mainPath -Raw
$requiredSnippets = @(
  '#include "app_main.h"',
  "App_Main_Init();",
  "App_Main_Task();"
)

$missing = @()
foreach ($snippet in $requiredSnippets) {
  if (-not $mainText.Contains($snippet)) {
    $missing += $snippet
  }
}

if ($missing.Count -gt 0) {
  Write-Warning "main.c is missing app entry snippets after CubeMX generation:"
  foreach ($snippet in $missing) {
    Write-Warning "  $snippet"
  }
  Write-Warning "Restore the calls inside USER CODE sections before building."
}
else {
  Write-Host "Checked main.c app entry snippets."
}
