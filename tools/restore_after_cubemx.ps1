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

function Read-Text {
  param([string]$Path)

  if (-not (Test-Path -LiteralPath $Path)) {
    throw "File not found: $Path"
  }

  return Get-Content -LiteralPath $Path -Raw
}

function Write-Text {
  param(
    [string]$Path,
    [string]$Text
  )

  for ($i = 0; $i -lt 5; $i++) {
    try {
      Set-Content -LiteralPath $Path -Value $Text -Encoding ASCII
      return
    }
    catch [System.IO.IOException] {
      if ($i -eq 4) {
        throw
      }
      Start-Sleep -Milliseconds 300
    }
  }
}

function Write-Lines {
  param(
    [string]$Path,
    [string[]]$Lines
  )

  for ($i = 0; $i -lt 5; $i++) {
    try {
      Set-Content -LiteralPath $Path -Value $Lines -Encoding ASCII
      return
    }
    catch [System.IO.IOException] {
      if ($i -eq 4) {
        throw
      }
      Start-Sleep -Milliseconds 300
    }
  }
}

function Ensure-Lines-After {
  param(
    [string]$Path,
    [string]$Anchor,
    [string[]]$LinesToAdd
  )

  $lines = Get-Content -LiteralPath $Path
  $changed = $false

  foreach ($lineToAdd in $LinesToAdd) {
    if ($lines -contains $lineToAdd) {
      continue
    }

    $out = New-Object System.Collections.Generic.List[string]
    $inserted = $false
    foreach ($line in $lines) {
      $out.Add($line)
      if (-not $inserted -and $line -eq $Anchor) {
        $out.Add($lineToAdd)
        $inserted = $true
      }
    }

    if (-not $inserted) {
      throw "Anchor not found in ${Path}: $Anchor"
    }

    $lines = $out.ToArray()
    $changed = $true
  }

  if ($changed) {
    Write-Lines -Path $Path -Lines $lines
  }
}

function Ensure-Text-After {
  param(
    [string]$Path,
    [string]$Anchor,
    [string]$TextToAdd
  )

  $text = Read-Text -Path $Path
  if ($text.Contains($TextToAdd)) {
    return
  }
  if (-not $text.Contains($Anchor)) {
    throw "Anchor not found in ${Path}: $Anchor"
  }

  $text = $text.Replace($Anchor, $Anchor + "`r`n" + $TextToAdd)
  Write-Text -Path $Path -Text $text
}

function Ensure-Replacement {
  param(
    [string]$Path,
    [string]$OldText,
    [string]$NewText
  )

  $text = Read-Text -Path $Path
  if ($text.Contains($NewText)) {
    return
  }
  if (-not $text.Contains($OldText)) {
    throw "Text to replace not found in ${Path}: $OldText"
  }

  $text = $text.Replace($OldText, $NewText)
  Write-Text -Path $Path -Text $text
}

$appliMakefilePath = Join-Path $variantPath "Makefile\Appli\Makefile"
$fsblMakefilePath = Join-Path $variantPath "Makefile\FSBL\Makefile"
$topMakefilePath = Join-Path $variantPath "Makefile\Makefile"
$appliMainPath = Join-Path $variantPath "Appli\Core\Src\main.c"
$appliGpioPath = Join-Path $variantPath "Appli\Core\Src\gpio.c"
$fsblXspiPath = Join-Path $variantPath "FSBL\Core\Src\xspi.c"

Ensure-Lines-After `
  -Path $appliMakefilePath `
  -Anchor "../../Appli/Core/Src/main.c \" `
  -LinesToAdd @(
    "../../Appli/Core/Src/app_main.c \",
    "../../Appli/Core/Src/ad7606_spi_dma.c \",
    "../../Appli/Core/Src/ext_ram_test.c \",
    "../../Appli/Core/Src/sysmem.c \",
    "../../Appli/Core/Src/syscalls.c \"
  )
Write-Host "Checked Appli Makefile custom sources."

Ensure-Lines-After `
  -Path $fsblMakefilePath `
  -Anchor "../../FSBL/Core/Src/stm32n6xx_hal_msp.c \" `
  -LinesToAdd @(
    "../../FSBL/Core/Src/extmem.c \",
    "../../FSBL/Core/Src/psram_diag.c \"
  )

Ensure-Lines-After `
  -Path $fsblMakefilePath `
  -Anchor "../../Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_xspi.c \" `
  -LinesToAdd @(
    "../../../../Middlewares/ST/STM32_ExtMem_Manager/stm32_extmem.c \",
    "../../../../Middlewares/ST/STM32_ExtMem_Manager/boot/stm32_boot_lrun.c \",
    "../../../../Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp/stm32_sfdp_data.c \",
    "../../../../Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp/stm32_sfdp_driver.c \",
    "../../../../Middlewares/ST/STM32_ExtMem_Manager/sal/stm32_sal_xspi.c \"
  )

Ensure-Lines-After `
  -Path $fsblMakefilePath `
  -Anchor "-I../../FSBL/Core/Inc \" `
  -LinesToAdd @(
    "-I../../../../Middlewares/ST/STM32_ExtMem_Manager \",
    "-I../../../../Middlewares/ST/STM32_ExtMem_Manager/boot \",
    "-I../../../../Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp \",
    "-I../../../../Middlewares/ST/STM32_ExtMem_Manager/sal \"
  )
Write-Host "Checked FSBL Makefile ExtMem/PSRAM sources."

Ensure-Replacement `
  -Path $fsblXspiPath `
  -OldText "hxspi1.Init.ClockPrescaler = 1;" `
  -NewText "hxspi1.Init.ClockPrescaler = 3;"
Ensure-Replacement `
  -Path $fsblXspiPath `
  -OldText "hxspi1.Init.DelayHoldQuarterCycle = HAL_XSPI_DHQC_DISABLE;" `
  -NewText "hxspi1.Init.DelayHoldQuarterCycle = HAL_XSPI_DHQC_ENABLE;"
Ensure-Replacement `
  -Path $fsblXspiPath `
  -OldText "hxspi2.Init.ClockPrescaler = 0;" `
  -NewText "hxspi2.Init.ClockPrescaler = 3;"
Ensure-Replacement `
  -Path $fsblXspiPath `
  -OldText "hxspi2.Init.ClockPrescaler = 1;" `
  -NewText "hxspi2.Init.ClockPrescaler = 3;"
Write-Host "Checked FSBL XSPI1 PSRAM timing and XSPI2 Flash timing."

Ensure-Replacement `
  -Path $appliGpioPath `
  -OldText "HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_RESET);" `
  -NewText "HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_SET);"
Write-Host "Checked Appli GPIO defaults."

Ensure-Text-After `
  -Path $appliMainPath `
  -Anchor "  __HAL_RCC_RIFSC_CLK_ENABLE();" `
  -TextToAdd "  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPI1, RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);`r`n  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPIM, RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);"
Write-Host "Checked Appli RIF secure attributes."

Ensure-Replacement `
  -Path $topMakefilePath `
  -OldText "cd FSBL && `$(MAKE)" `
  -NewText "`$(MAKE) -C FSBL"
Ensure-Replacement `
  -Path $topMakefilePath `
  -OldText "cd Appli && `$(MAKE)" `
  -NewText "`$(MAKE) -C Appli"
Ensure-Replacement `
  -Path $topMakefilePath `
  -OldText "main=    cd  Secure && `$(MAKE)" `
  -NewText "main=    `$(MAKE) -C Secure"
Write-Host "Checked top-level Makefile recursive make commands."

$mainText = Read-Text -Path $appliMainPath
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
