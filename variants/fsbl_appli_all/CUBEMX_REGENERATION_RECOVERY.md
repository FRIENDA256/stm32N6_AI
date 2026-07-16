# CubeMX 重新生成后的恢复清单

更新日期：2026-07-16

## 目的

`fsbl_appli_all` 包含 CubeMX 生成代码、手工板级适配、NetX 性能优化和
Neural-ART 模型代码。CubeMX 只能保护部分 `USER CODE` 区域，重新生成后仍可能
覆盖 Makefile、PHY 驱动和模型配套参数。本清单用于在每次生成后快速恢复并验证
工程，避免出现能生成但不能编译、能编译但不能联网或 AI 权重不匹配的情况。

## 当前必须保留的 CubeMX 配置

- NPU 时钟：800 MHz。
- NPU RAM 时钟：800 MHz。
- ETH1 kernel clock：100 MHz，来源为 PLL3/IC12。
- SPI3：25 MHz，HPDMA1 Channel 0 用于 RX、Channel 1 用于 TX；PC10/PC11/PC12
  GPIO speed 必须为 `Very High`。
- HPDMA1 Channel 0/1：DMA priority 为 High，NVIC priority 为 8。
- XSPI1 MemorySize：512 MB。
- XSPI1 RISAF region 1：`0x00000000-0x03FFFFFF`。
- AI 模型：`tiny_temporal_mixer_8ch_int8_qdq.onnx`，目标为 Neural-ART N6。
- 当前权重文件大小：82689 字节。
- 当前权重 SHA-256：
  `A79CE62CAC662DC6C77FF526EDF0E2770973D8CD4073F132725054599FEF996F`。

## 已放入 USER CODE 的保护项

下列修补位于 `USER CODE` 区域，正常情况下 CubeMX 会保留，但生成后仍应通过
`git diff` 确认。

1. `Appli/AZURE_RTOS/App/app_azure_rtos_config.h`
   - 在 `USER CODE BEGIN EC` 中覆盖 `NX_APP_MEM_POOL_SIZE=98304`。

2. `Appli/NetXDuo/App/nx_user.h`
   - 在 `USER CODE BEGIN 1` 中启用 `NX_ENABLE_INTERFACE_CAPABILITY`。
   - 设置 `NX_IP_PERIODIC_RATE=1000`，与 ThreadX 1 kHz tick 一致。

3. `Appli/Core/Src/dcmipp.c`
   - 在 `DCMIPP_MspInit 0` 中声明 `RIMC_MasterConfig_t RIMC_master`。
   - 在 `DCMIPP_Init 2` 中关闭持续产生噪声的 CSI DPHY data-lane error 中断。

4. `Appli/Core/Inc/stm32n6xx_it.h` 和 `Appli/Core/Src/stm32n6xx_it.c`
   - 保留 `CACHEAXI_IRQHandler()`，转发到 ATON runtime 的
     `NPU_CACHE_IRQHandler()`。

5. `Appli/Core/Src/eth.c`
   - 在 `MACADDRESS` 用户区恢复本地 MAC `02:00:00:00:00:01`。
   - 在 `ETH1_MspInit 1` 中释放板上未连接的 PF5，并恢复经过验证的 RGMII pull
     和 speed 配置。
   - CubeMX 可能把相同名称的 `MACADDRESS` 用户区复制到
     `HAL_ETH_MspInit()`。该位置没有 `MACAddr` 变量，必须保持为空，否则会出现
     `MACAddr undeclared` 编译错误。

6. `Appli/Core/Src/main.c`
   - 在 `RIF_Init 1` 中恢复 ETH1 secure/non-privileged 属性。
   - 保留 DCMIPP/CSI secure/privileged 属性。

7. `Appli/NetXDuo/App/app_netxduo.c`
   - `USER CODE 0` 保持为空并位于 `MX_NetXDuo_Init()` 内。
   - 文件作用域辅助函数全部放在函数结束后的 `USER CODE 1` 中。不要再把静态
     函数放入 `USER CODE 0`，否则下次生成可能把它们嵌入
     `MX_NetXDuo_Init()`，产生 `invalid storage class` 编译错误。

8. `Appli/Core/Src/hpdma.c`
   - `MX_HPDMA1_Init()` 的 USER CODE 区必须启用 HPDMA1 时钟。
   - 必须设置并启用 HPDMA1 Channel 0/1 中断，NVIC priority 为 8。
   - 当前 CubeMX 版本可能生成空的 `MX_HPDMA1_Init()`，即使 `.ioc` 已启用中断，
     所以每次生成后都要检查实际 C 代码。

## 每次生成后必须手工检查的项目

### 1. Appli Makefile

文件：`Makefile/Appli/Makefile`

- `OPT` 必须为 `-O2`。
- `C_SOURCES` 必须包含：
  - `app_tcp_command.c`、`app_udp_echo.c`
  - `ad7606_spi_dma.c`、`app_ad7606.c`、`app_ai.c`
  - `app_ir_capture.c`、`app_callbacks.c`、`app_camera_imx219.c`
  - `app_console.c`、`app_timebase.c`
  - `tiny1c_debug_driver.c`、`tiny1c_port_stm32_hal.c`
  - `eth_diagnostics.c`
  - `Drivers/BSP/Components/imx219/imx219.c`
- C/ASM include path 必须包含 `Drivers/BSP/Components/imx219`。
- `C_DEFS` 必须包含：

```make
-DLIMIT_RTL8211F_TO_100M_FULL=0
-DENABLE_RTL8211F_TXDELAY=1
-DENABLE_RTL8211F_RXDELAY=1
-DDISABLE_RTL8211F_EEE=1
```

### 2. FSBL Makefile

文件：`Makefile/FSBL/Makefile`

- 恢复 `FSBL/Core/Src/extmem.c`。
- 恢复 STM32 ExtMem Manager 的 `stm32_extmem.c`、boot、SFDP 和 SAL 源文件。
- 恢复 `STM32_ExtMem_Manager`、`boot`、`nor_sfdp`、`sal` 四个 include path。
- 缺失这些项目时，典型报错是 `stm32_extmem.h: No such file or directory`。

### 3. RTL8211 板级修补

- `Drivers/BSP/Components/rtl8211/rtl8211.c`：Tx/Rx delay 寄存器必须使用
  read-modify-write，不能直接写单个 delay bit，否则会清除 strap 位。
- `Middlewares/ST/netxduo/common/drivers/ethernet/rtl8211/nx_stm32_phy_driver.c`：
  初始化后必须调用 `rtl8211_enable_rxc_output()`。
- 确认 EEE 已通过 Makefile 宏关闭。

### 4. SPI3 HPDMA 迁移

- `Appli/Core/Src/spi.c`：SPI3 TX 必须链接 HPDMA1 Channel 1，RX 必须链接
  HPDMA1 Channel 0。
- `fsbl_appli_all.ioc` 和 `Appli/Core/Src/spi.c`：PC10/PC11/PC12 必须保持
  `GPIO_SPEED_FREQ_VERY_HIGH`，避免 25/50 MHz SPI 信号边沿过慢。
- TX memory source 和 RX memory destination 必须使用 Port 1；外设侧使用 Port 0。
- `Appli/Core/Src/hpdma.c`：确认 HPDMA1 时钟与 Channel 0/1 NVIC 已启用。
- `Appli/Core/Src/stm32n6xx_it.c`：Channel 0/1 IRQ 必须分别调用对应句柄的
  `HAL_DMA_IRQHandler()`。
- `Appli/Core/Src/main.c`：Channel 0/1 必须配置 secure/privileged source 和
  destination attributes。

### 5. AI 模型与权重

模型每次重新生成后必须同步替换以下三个部分：

- `Appli/X-CUBE-AI/App/tiny_temporal_mixer_8ch_int8.c/.h`
- `tiny_temporal_mixer_8ch_int8_atonbuf.xSPI2.raw`
- `Appli/Core/Src/app_ai.c` 中的权重探针和输入量化参数

当前模型输入为 INT8 `[1,1,8,1024]`，输入 scale 为 `0.00675407844`、zero point
为 0。训练预处理为 `raw/32768`，固件采用定点等价换算：

```text
q = clamp(round(raw * 37903 / 2^23), -128, 127)
```

当前权重前 256 字节和为 `0x00007BB1`。替换模型后使用以下 PowerShell 命令
重新计算，并更新 `APP_AI_WEIGHT_PROBE_SUM`：

```powershell
$b = [IO.File]::ReadAllBytes(".\tiny_temporal_mixer_8ch_int8_atonbuf.xSPI2.raw")
$sum = 0
0..255 | ForEach-Object { $sum += $b[$_] }
"0x{0:X8}" -f $sum
```

`tools/flash_npu_weights.ps1` 的默认权重路径必须指向
`variants\fsbl_appli_all\tiny_temporal_mixer_8ch_int8_atonbuf.xSPI2.raw`。

## 生成后验证顺序

1. 查看差异：

```powershell
git diff -- variants/fsbl_appli_all
```

2. 编译 Appli：

```powershell
cd variants\fsbl_appli_all\Makefile\Appli
make -j8
```

3. 编译 FSBL：

```powershell
cd ..\FSBL
make -j8
```

4. 烧录后检查串口至少包含：

```text
NetX init OK
AI thread start
AI ready model=tiny_temporal_mixer_8ch_int8 source=AD7606
```

5. PC 侧回归：

```powershell
ping 192.168.6.50
python .\tools\net_throughput_test.py --mode both --no-fill
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\tcp_ir.ps1 -Command AISTAT
```

6. `AISTAT` 应满足：`init=1`、`ready=1`、`fault=0`、`weights=1`，并显示
   `prep=s16_qscale0p006754_window1024`。

## 2026-07-14 本次恢复结果

- Appli 编译通过：`text=178556`、`data=292`、`bss=917924`。
- FSBL 编译通过：`text=37524`、`data=184`、`bss=3192`。
- 保留本次生成的 600 MHz NPU、PLL3 ETH 时钟、512 MB XSPI1 和新模型文件。
- 未执行烧录；板级启动、网络吞吐与推理运行状态仍需硬件回归。

## 2026-07-15 Tiny1C SPI3 50 MHz trial

- Keep `SPI3` transfers split into 4096-byte protocol chunks.
- In `Appli/Core/Src/spi.c`, restore
  `hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;`.
- The current SPI3 kernel clock is 200 MHz, so `/4` gives a 50 MHz SCK.
- This matches the local Tiny1C reference project, which uses a 100 MHz SPI3
  kernel clock with `/2` for the same 50 MHz SCK.
- The `.ioc` intentionally remains at `/8` during this code-only trial. If the
  hardware regression is stable, update CubeMX to `/4` before a later code
  regeneration or reapply this patch afterwards.

### Hardware result

- The 50 MHz trial is rejected on the current board.
- Startup frame discard repeatedly failed, `IRSTAT err` increased continuously,
  and both `IRGETIMG` and `IRGETTEMP` returned capture errors.
- AD7606, Ethernet, and AI remained healthy, localizing the regression to the
  Tiny1C SPI3 link.
- Keep both generated code and CubeMX at `/8` (25 MHz). The SPI DMA semaphore
  optimization and 4096-byte protocol chunks remain enabled.

## 2026-07-16 HPDMA1 迁移恢复结果

- SPI3 RX/TX 已从 GPDMA1 Channel 9/8 迁移到 HPDMA1 Channel 0/1。
- 补齐 CubeMX 未生成的 HPDMA1 时钟和 Channel 0/1 NVIC 初始化。
- 恢复 Appli Makefile、自定义采集/网络源文件、IMX219、RTL8211 和 FSBL ExtMem
  Manager 配置。
- 修复 CubeMX 将本地 MAC 用户代码复制到 `HAL_ETH_MspInit()` 导致的
  `MACAddr undeclared` 编译错误。
- AI 模型权重保持 82689 字节，SHA-256 与权重探针未变化，无需重新烧录权重。
- Appli 编译通过：`text=186012`、`data=332`、`bss=967108`。
- FSBL 编译通过：`text=37540`、`data=184`、`bss=3192`。
- 板级回归通过：25 MHz 后台采集连续运行约 59 分钟，Tiny1C 图像约
  9.50 fps、温度约 4.94 fps，采集错误为 0。
- 50 MHz HPDMA 测试在 AD7606 运行和暂停两种状态下均完成 100/100 帧，
  DMA/HAL 错误为 0；AD7606 运行时仍可见极少量高幅跳点，因此正式运行继续
  使用 25 MHz。
- 网络回归通过：TCP no-fill 约 218 Mbps，UDP no-fill 约 279.5 Mbps，UDP
  接收率 100%，无序号缺口和错误包。
- AI 连续完成 82932 次推理，`run_err=0`、`resets=0`，平均推理约 6 ms。
- Tiny1C 偶尔会在软重启后输出固定温度帧；完整断电可恢复真实温度数据，后续
  应增加固定帧检测以及可控电源复位机制。
