param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [switch]$RequireBuildArtifacts
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:Passes = 0
$script:Failures = @()
$script:Warnings = @()

function Add-Pass {
    param([string]$Message)
    $script:Passes++
    Write-Host "[OK]   $Message" -ForegroundColor Green
}

function Add-Fail {
    param([string]$Message)
    $script:Failures += $Message
    Write-Host "[FAIL] $Message" -ForegroundColor Red
}

function Add-Warn {
    param([string]$Message)
    $script:Warnings += $Message
    Write-Host "[WARN] $Message" -ForegroundColor Yellow
}

function Get-ProjectPath {
    param([string]$RelativePath)
    return Join-Path $ProjectRoot $RelativePath
}

function Get-ProjectText {
    param([string]$RelativePath)

    $path = Get-ProjectPath $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Add-Fail "Missing file: $RelativePath"
        return ""
    }

    return [System.IO.File]::ReadAllText($path)
}

function Test-FileExists {
    param(
        [string]$RelativePath,
        [string]$Description
    )

    $path = Get-ProjectPath $RelativePath
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        Add-Pass $Description
    } else {
        Add-Fail "$Description (missing: $RelativePath)"
    }
}

function Test-Contains {
    param(
        [string]$RelativePath,
        [string]$Pattern,
        [string]$Description
    )

    $text = Get-ProjectText $RelativePath
    if ($text -match $Pattern) {
        Add-Pass $Description
    } else {
        Add-Fail "$Description (missing in $RelativePath)"
    }
}

function Test-OptionalContains {
    param(
        [string]$RelativePath,
        [string]$Pattern,
        [string]$Description
    )

    $path = Get-ProjectPath $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        if ($RequireBuildArtifacts) {
            Add-Fail "$Description (missing build artifact: $RelativePath)"
        } else {
            Add-Warn "$Description skipped; build artifact missing: $RelativePath"
        }
        return
    }

    $text = [System.IO.File]::ReadAllText($path)
    if ($text -match $Pattern) {
        Add-Pass $Description
    } else {
        if ($RequireBuildArtifacts) {
            Add-Fail "$Description (missing in $RelativePath)"
        } else {
            Add-Warn "$Description not found in optional artifact: $RelativePath"
        }
    }
}

if (-not (Test-Path -LiteralPath $ProjectRoot -PathType Container)) {
    throw "Project root does not exist: $ProjectRoot"
}

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
Write-Host "Checking Ethernet baseline in: $ProjectRoot"
Write-Host ""

Test-FileExists "fsbl_appli_all.ioc" "CubeMX project file exists"
Test-Contains "fsbl_appli_all.ioc" "NETXDUO\.NX_IP_PERIODIC_RATE=1000" "CubeMX NetX periodic rate is 1000"
Test-Contains "fsbl_appli_all.ioc" "ETH1\.MediaInterface=HAL_ETH_RGMII_MODE" "CubeMX ETH1 media interface is RGMII"
Test-Contains "Appli/AZURE_RTOS/App/app_azure_rtos_config.h" "#\s*define\s+NX_APP_MEM_POOL_SIZE\s+65536\b" "NetX byte pool is large enough for packet pool and service threads"
Test-Contains "Appli/Core/Src/cacheaxi.c" "#if\s+!defined\(APP_ENABLE_CACHEAXI\)[\s\S]*return;" "CACHEAXI stays disabled until ETH DMA cache maintenance exists"
Test-Contains "Appli/Core/Src/cacheaxi.c" "ETH DMA descriptors and NetX packet buffers" "CACHEAXI guard documents Ethernet DMA coherency reason"

Test-Contains "Middlewares/ST/netxduo/common/drivers/ethernet/rtl8211/nx_stm32_phy_driver.c" "static\s+int32_t\s+rtl8211_enable_rxc_output\s*\(" "RTL8211F RXC output helper exists"
Test-Contains "Middlewares/ST/netxduo/common/drivers/ethernet/rtl8211/nx_stm32_phy_driver.c" "rtl8211_enable_rxc_output\s*\(\s*\)" "RTL8211F RXC output helper is called"
Test-Contains "Middlewares/ST/netxduo/common/drivers/ethernet/rtl8211/nx_stm32_phy_driver.c" "RTL8211_PHYCR2_RXC_ENABLE" "RTL8211F RXC enable bit is written"
Test-Contains "Middlewares/ST/netxduo/common/drivers/ethernet/rtl8211/nx_stm32_phy_driver.c" "LIMIT_RTL8211F_TO_100M_FULL" "RTL8211F 100M full-duplex guard is present"
Test-Contains "Middlewares/ST/netxduo/common/drivers/ethernet/rtl8211/nx_stm32_phy_driver.c" "RTL8211_GBCR_1000BT_FD" "RTL8211F gigabit advertisement can be cleared"
Test-Contains "Middlewares/ST/netxduo/common/drivers/ethernet/rtl8211/nx_stm32_phy_driver.c" "RTL8211_ANAR_100BTX_FD" "RTL8211F 100M full-duplex advertisement is configured"

Test-Contains "Drivers/BSP/Components/rtl8211/rtl8211.c" "ENABLE_RTL8211F_TXDELAY" "RTL8211F TX delay support exists"
Test-Contains "Drivers/BSP/Components/rtl8211/rtl8211.c" "ENABLE_RTL8211F_RXDELAY" "RTL8211F RX delay support exists"
Test-Contains "Drivers/BSP/Components/rtl8211/rtl8211.c" "DISABLE_RTL8211F_EEE" "RTL8211F EEE disable support exists"
Test-Contains "Drivers/BSP/Components/rtl8211/rtl8211.c" "ReadReg\(pObj->DevAddr,\s*RTL8211_MIICR1_PD08,[\s\S]*RTL8211_MIICR1_TXDLY_ENABLE" "RTL8211F TX delay preserves strap-configured RGMII bits"
Test-Contains "Drivers/BSP/Components/rtl8211/rtl8211.c" "ReadReg\(pObj->DevAddr,\s*RTL8211_MIICR2_PD08,[\s\S]*RTL8211_MIICR2_RXDLY_ENABLE" "RTL8211F RX delay preserves strap-configured RGMII bits"

Test-Contains "Appli/NetXDuo/App/nx_user.h" "#\s*define\s+NX_IP_PERIODIC_RATE\s+1000\b" "NetX periodic rate is fixed at 1000"
Test-Contains "Appli/NetXDuo/Target/nx_stm32_eth_config.h" "#\s*define\s+ETH_PHY_1000MBITS_SUPPORTED\b" "ETH PHY gigabit support define is present"

Test-Contains "Makefile/Appli/Makefile" "Middlewares/ST/netxduo/common/drivers/ethernet/rtl8211/nx_stm32_phy_driver\.c" "Appli Makefile builds RTL8211 PHY driver"
Test-Contains "Makefile/Appli/Makefile" "-DETH_PHY_1000MBITS_SUPPORTED\b" "Appli Makefile defines ETH_PHY_1000MBITS_SUPPORTED"
Test-Contains "Makefile/Appli/Makefile" "-DLIMIT_RTL8211F_TO_100M_FULL=1\b" "Appli Makefile limits RTL8211F to 100M full"
Test-Contains "Makefile/Appli/Makefile" "-DENABLE_RTL8211F_TXDELAY=1\b" "Appli Makefile enables RTL8211F TX delay"
Test-Contains "Makefile/Appli/Makefile" "-DENABLE_RTL8211F_RXDELAY=1\b" "Appli Makefile enables RTL8211F RX delay"
Test-Contains "Makefile/Appli/Makefile" "-DDISABLE_RTL8211F_EEE=1\b" "Appli Makefile disables RTL8211F EEE"
Test-Contains "Makefile/Appli/Makefile" "-DRTL8211_INIT_TO=10000\b" "Appli Makefile keeps RTL8211 init timeout"

Test-Contains "Makefile/Appli/STM32N657XX_LRUN.ld" "\.RxDecripSection\s+0x34100000\s+\(NOLOAD\)" "ETH RX descriptors are fixed at 0x34100000"
Test-Contains "Makefile/Appli/STM32N657XX_LRUN.ld" "\.TxDecripSection\s+0x341000C0\s+\(NOLOAD\)" "ETH TX descriptors are fixed at 0x341000C0"
Test-Contains "Makefile/Appli/STM32N657XX_LRUN.ld" "KEEP\(\*\(\.RxDecripSection\)\)" "ETH RX descriptor section is kept"
Test-Contains "Makefile/Appli/STM32N657XX_LRUN.ld" "KEEP\(\*\(\.TxDecripSection\)\)" "ETH TX descriptor section is kept"

Test-Contains "Appli/Core/Src/eth.c" "__attribute__\(\(section\(\""\.RxDecripSection\""\)\)\)" "ETH RX descriptor symbol uses RxDecripSection"
Test-Contains "Appli/Core/Src/eth.c" "__attribute__\(\(section\(\""\.TxDecripSection\""\)\)\)" "ETH TX descriptor symbol uses TxDecripSection"
Test-Contains "Appli/Core/Src/eth.c" "heth1\.Init\.RxBuffLen\s*=\s*1536" "ETH RX buffer length is 1536"
Test-Contains "Appli/Core/Src/eth.c" "MACAddr\[0\]\s*=\s*0x02;[\s\S]*MACAddr\[5\]\s*=\s*0x01;" "ETH MAC address matches verified local-admin address"
Test-Contains "Appli/Core/Src/eth.c" "GPIO_InitStruct\.Pin\s*=\s*GPIO_PIN_10\|GPIO_PIN_15\s*\|\s*GPIO_PIN_14\|GPIO_PIN_8\|GPIO_PIN_2\|GPIO_PIN_9\s*\|\s*GPIO_PIN_11\|GPIO_PIN_13\|GPIO_PIN_12;" "ETH GPIOF init keeps PF5 out of AF mode"
Test-Contains "Appli/Core/Src/eth.c" "HAL_GPIO_DeInit\(GPIOF,\s*GPIO_PIN_10\|GPIO_PIN_7\|GPIO_PIN_15\s*\|\s*GPIO_PIN_14\|GPIO_PIN_8\|GPIO_PIN_2\|GPIO_PIN_9\s*\|\s*GPIO_PIN_11\|GPIO_PIN_13\|GPIO_PIN_0\|GPIO_PIN_12\);" "ETH GPIOF deinit keeps PF5 out of mask"
Test-Contains "Appli/Core/Src/eth.c" "RCC_ETH1CLKSOURCE_IC12" "ETH kernel clock uses IC12 instead of 200 MHz HCLK"
Test-Contains "Appli/Core/Src/eth.c" "ICSelection\[RCC_IC12\]\.ClockDivider\s*=\s*12" "ETH IC12 divider keeps kernel clock at 100 MHz"
Test-Contains "Appli/Core/Src/eth.c" "HAL_NVIC_SetPriority\(ETH1_IRQn,\s*7,\s*0\)" "ETH IRQ priority is RTOS-friendly"
Test-Contains "Middlewares/ST/netxduo/common/drivers/ethernet/nx_stm32_eth_driver.c" "BroadcastFilter\s*=\s*DISABLE" "ETH driver allows broadcast frames"
Test-Contains "Middlewares/ST/netxduo/common/drivers/ethernet/nx_stm32_eth_driver.c" "BroadcastFilter\s*=\s*DISABLE;[\s\S]*HAL_ETH_SetMACFilterConfig\(&eth_handle,\s*&FilterConfig\)" "ETH MAC filter config is applied after enabling broadcasts"

Test-Contains "Appli/Core/Src/stm32n6xx_hal_msp.c" "HAL_PWREx_EnableVddIO5\s*\(" "VDDIO5 stays enabled for Ethernet RGMII GPIOF/G pins"
Test-Contains "Appli/Core/Src/app_threadx.c" "#\s*define\s+APP_AD7606_AUTOSTART\s+1U\b" "AD7606 autostart is enabled in the integrated build"
Test-Contains "Appli/NetXDuo/App/app_netxduo.c" "#\s*define\s+APP_NETX_ENABLE_PERIODIC_STATS\s+0U\b" "NetX periodic diagnostics are quiet by default"
Test-Contains "Appli/Core/Src/gpio.c" "HAL_NVIC_SetPriority\(AD_IRQ_EXTI_IRQn,\s*8,\s*0\)" "AD7606 EXTI IRQ does not outrank ETH IRQ"
Test-Contains "Appli/Core/Src/gpdma.c" "HAL_NVIC_SetPriority\(GPDMA1_Channel10_IRQn,\s*8,\s*0\)" "AD7606 SPI4 TX DMA IRQ does not outrank ETH IRQ"
Test-Contains "Appli/Core/Src/gpdma.c" "HAL_NVIC_SetPriority\(GPDMA1_Channel11_IRQn,\s*8,\s*0\)" "AD7606 SPI4 RX DMA IRQ does not outrank ETH IRQ"
Test-Contains "Appli/Core/Src/spi.c" "HAL_NVIC_SetPriority\(SPI4_IRQn,\s*8,\s*0\)" "AD7606 SPI4 IRQ does not outrank ETH IRQ"
Test-Contains "Appli/Core/Src/main.c" "HAL_RIF_RIMC_ConfigMasterAttributes\(RIF_MASTER_INDEX_ETH1" "RIF config includes ETH1 master"
Test-Contains "Appli/Core/Src/main.c" "HAL_RIF_RISC_SetSlaveSecureAttributes\(RIF_RISC_PERIPH_INDEX_ETH1" "RIF config includes ETH1 slave"
Test-Contains "Appli/Core/Src/main.c" "Ethernet_PrintClockDebug\s*\(" "Startup prints Ethernet clock debug"
Test-Contains "Appli/Core/Src/stm32n6xx_it.c" "Ethernet_RecordIrq\s*\(" "ETH IRQ records runtime count"

Test-Contains "FSBL/Core/Src/main.c" "MX_EXTMEM_Init\s*\(" "FSBL initializes external memory"
Test-Contains "FSBL/Core/Src/main.c" "BOOT_Application\s*\(" "FSBL boots Appli image"
Test-Contains "Makefile/FSBL/Makefile" "STM32_ExtMem_Manager/stm32_extmem\.c" "FSBL Makefile builds STM32 ExtMem manager"
Test-Contains "Makefile/FSBL/Makefile" "STM32_ExtMem_Manager/boot/stm32_boot_lrun\.c" "FSBL Makefile builds LRUN boot helper"

Test-OptionalContains "Makefile/Appli/build/fsbl_appli_all_Appli.map" "0x34100000\s+DMARxDscrTab" "Built map places DMARxDscrTab at 0x34100000"
Test-OptionalContains "Makefile/Appli/build/fsbl_appli_all_Appli.map" "0x341000c0\s+DMATxDscrTab" "Built map places DMATxDscrTab at 0x341000C0"
Test-OptionalContains "Makefile/Appli/build/fsbl_appli_all_Appli.map" "\.bss\.nx_byte_pool_buffer\s+0x[0-9a-fA-F]+\s+0x10000\b" "Built map keeps NetX byte pool at 64 KB"

foreach ($artifact in @(
    "Makefile/FSBL/build/fsbl_appli_all_FSBL-trusted.bin",
    "Makefile/Appli/build/fsbl_appli_all_Appli-trusted.bin"
)) {
    $path = Get-ProjectPath $artifact
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        Add-Pass "Trusted binary exists: $artifact"
    } elseif ($RequireBuildArtifacts) {
        Add-Fail "Trusted binary missing: $artifact"
    } else {
        Add-Warn "Trusted binary missing: $artifact"
    }
}

Write-Host ""
Write-Host "Ethernet baseline check: $($script:Passes) passed, $($script:Warnings.Count) warnings, $($script:Failures.Count) failures."

if ($script:Warnings.Count -gt 0) {
    Write-Host ""
    Write-Host "Warnings:" -ForegroundColor Yellow
    foreach ($warning in $script:Warnings) {
        Write-Host "  - $warning" -ForegroundColor Yellow
    }
}

if ($script:Failures.Count -gt 0) {
    Write-Host ""
    Write-Host "Failures:" -ForegroundColor Red
    foreach ($failure in $script:Failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

exit 0
