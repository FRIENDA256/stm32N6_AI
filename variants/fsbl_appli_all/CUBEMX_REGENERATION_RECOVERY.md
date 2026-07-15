# CubeMX 重新生成后的恢复清单

更新日期：2026-07-14

## 目的

`fsbl_appli_all` 包含 CubeMX 生成代码、手工板级适配、NetX 性能优化和
Neural-ART 模型代码。CubeMX 只能保护部分 `USER CODE` 区域，重新生成后仍可能
覆盖 Makefile、PHY 驱动和模型配套参数。本清单用于在每次生成后快速恢复并验证
工程，避免出现能生成但不能编译、能编译但不能联网或 AI 权重不匹配的情况。

## 当前必须保留的 CubeMX 配置

- NPU 时钟：600 MHz。
- NPU RAM 时钟：400 MHz。
- ETH1 kernel clock：100 MHz，来源为 PLL3/IC12。
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

6. `Appli/Core/Src/main.c`
   - 在 `RIF_Init 1` 中恢复 ETH1 secure/non-privileged 属性。
   - 保留 DCMIPP/CSI secure/privileged 属性。

7. `Appli/NetXDuo/App/app_netxduo.c`
   - `USER CODE 0` 保持为空并位于 `MX_NetXDuo_Init()` 内。
   - 文件作用域辅助函数全部放在函数结束后的 `USER CODE 1` 中。不要再把静态
     函数放入 `USER CODE 0`，否则下次生成可能把它们嵌入
     `MX_NetXDuo_Init()`，产生 `invalid storage class` 编译错误。

## 每次生成后必须手工检查的项目

### 1. Appli Makefile

文件：`Makefile/Appli/Makefile`

- `OPT` 必须为 `-O2`。
- `C_SOURCES` 必须包含：
  - `app_tcp_command.c`、`app_udp_echo.c`
  - `ad7606_spi_dma.c`、`app_ad7606.c`、`app_ai.c`
  - `app_callbacks.c`、`app_camera_imx219.c`
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

### 4. AI 模型与权重

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
