# stm32N6_AI

## Variants

The repository root keeps the verified TrustZone boot-chain baseline:

```text
FSBL -> AppliSecure -> AppliNonSecure
```

The simplified LRUN variant should be placed under:

```text
variants/fsbl_appli_lrun/
```

That variant is intended for PSRAM memory-mapped bring-up and future AI
deployment work, following the official `VENC_RTSP_Server` style:

```text
FSBL -> Appli
```

这是一个面向 `STM32N6570-DK` 的 VS Code + Makefile 测试工程，用来验证从 CubeMX 生成的最小配置工程，到 STM32N6 三段式外部 Flash 启动链路的完整开发流程。

当前工程已经验证：

- 外部 Flash boot 可以启动 FSBL。
- FSBL 可以从外部 Flash 加载 Secure / NonSecure 镜像。
- Secure 工程可以完成 SAU / RIF / RISAF 安全隔离配置。
- Secure 可以跳转到 NonSecure。
- NonSecure 主循环正常运行。
- USART3 通过 PD8 / PD9 输出心跳信息，波特率 `115200 8N1`。
- SPI4 通过 GPDMA1 接收 AD7606 采集卡数据帧。
- PO1 LED 已绑定 SPI4 采集状态，用作通信健康指示。

## 工程来源

本工程参考 STM32CubeN6 官方示例：

```text
Projects/STM32N6570-DK/Templates/Template_Isolation_LRUN
```

初始工程由 CubeMX 生成，只保留了当前阶段需要的最小外设：

- SWD / ST-LINK 调试口
- PO1 LED
- 外部 Flash 相关 XSPI2 / XSPIM 引脚
- USART3，PD8 TX / PD9 RX
- SPI4，PE12 SCK / PE13 MISO / PE14 MOSI
- AD_IRQ，PE8，上升沿 EXTI，用于采集卡数据就绪握手
- AD_CS，PB0，由 NonSecure 软件控制
- GPDMA1 Channel 10 / 11，用于 SPI4 TX / RX
- 必要的 RIF / SAU / RISAF 配置

## 工程结构

```text
FSBL/                 First Stage BootLoader
AppliSecure/          Secure application
AppliNonSecure/       NonSecure application
Makefile/             三段工程的 Makefile / linker script / startup
Middlewares/ST/       STM32_ExtMem_Manager
Drivers/              STM32N6 HAL / CMSIS
.vscode/              VS Code 构建、签名、烧录任务
docs/                 调试记录和注意事项
```

三段工程职责：

- `FSBL`：初始化系统时钟、XSPI2 外部 Flash、ExtMem Manager，并调用 `BOOT_Application()`。
- `AppliSecure`：配置 TrustZone、SAU、RIF、RISAF，然后跳入 NonSecure。
- `AppliNonSecure`：当前作为功能验证区，负责 USART3 日志输出、SPI4 DMA 采集链路验证和 LED 状态指示。

## 当前验证信号

NonSecure 主循环位于：

```text
AppliNonSecure/Core/Src/main.c
```

当前现象：

- 串口启动后输出：

```text
STM32N6_AI AppNS USART3 start
```

- 之后周期输出：

```text
STM32N6_AI AppNS heartbeat
```

- SPI4 采集质量统计每 5 秒输出一次：

```text
SPI4 quality win=5000ms frame=250 crc_ok=250 crc_bad=0 dma_err=0 bad_hdr=0 bad_len=0 len_warn=0 Bps=412000
SPI4 quality gap irq=0 max_irq_delta=0 seq=0 max_seq_delta=0 raw_gap=0 max_raw_gap=0 raw_bad=0 dt_ms=[7,15] sample_delta=[358,716]
```

当前稳定状态下，`crc_bad / dma_err / bad_hdr / bad_len / raw_gap` 应保持为 `0`。`frame` 约为 `250 frame / 5 s`，对应约 `50 frame/s`；单帧 `8240 bytes` 时，吞吐约 `412000 B/s`。

PO1 LED 当前含义：

```text
熄灭       超过 1.5 s 没有收到有效 CRC_OK 采集帧
慢闪       持续收到有效 SPI4 采集帧，通信健康
快闪 3 s   最近出现过 CRC 错、坏头、坏长度、DMA/HAL 异常
```

LED 参数位于 `AppliNonSecure/Core/Src/main.c`：

```c
#define AD_SPI_LED_NO_FRAME_TIMEOUT_MS 1500U
#define AD_SPI_LED_ERROR_HOLD_MS      3000U
#define AD_SPI_LED_GOOD_TOGGLE_MS     500U
#define AD_SPI_LED_ERROR_TOGGLE_MS    100U
```

串口参数：

```text
USART3
TX: PD8
RX: PD9
115200 baud
8 data bits
No parity
1 stop bit
No flow control
```

注意：USART3 当前使用 `PCLK1` 作为时钟源。曾经使用 `MSI` 时，因为 FSBL 将 MSI 配成 16 MHz，而 AppNS HAL 中 `MSI_VALUE` 仍按 4 MHz 计算，导致实际波特率偏差约 4 倍，串口接收为乱码。已在 `AppliNonSecure/Core/Src/usart.c` 中改为：

```c
PeriphClkInitStruct.Usart3ClockSelection = RCC_USART3CLKSOURCE_PCLK1;
```

SPI4 参数：

```text
SPI4 master
SCK:  PE12
MISO: PE13
MOSI: PE14
CS:   PB0, AD_CS, software GPIO
IRQ:  PE8, AD_IRQ, rising edge EXTI
Clock source: HSI, 64 MHz
Prescaler: /8, about 8 Mbit/s
DMA RX: GPDMA1 Channel 11
DMA TX: GPDMA1 Channel 10
```

注意：`STM32N6_AI.ioc` 中 SPI4 prescaler 可能仍显示 CubeMX 生成时的值；当前源码在 `AppliNonSecure/Core/Src/spi.c` 中手工调为：

```c
hspi4.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
```

## 构建

推荐直接使用 VS Code 任务：

```text
Build: All
```

手动构建顺序如下：

```powershell
make -C Makefile/AppliSecure
make -C Makefile/FSBL
make -C Makefile/AppliNonSecure
```

必须先构建 `AppliSecure`，因为 `AppliNonSecure` 链接时依赖 Secure 侧生成的 CMSE import library：

```text
Makefile/AppliSecure/build/secure_nsclib.o
```

## 签名

STM32N6 外部 Flash 启动需要烧录带 STM32 header 的 trusted bin，不能直接烧 raw bin。

推荐使用 VS Code 任务：

```text
Sign: All
```

生成文件：

```text
Makefile/FSBL/build/STM32N6_AI_FSBL-trusted.bin
Makefile/AppliSecure/build/STM32N6_AI_AppliSecure-trusted.bin
Makefile/AppliNonSecure/build/STM32N6_AI_AppliNonSecure-trusted.bin
```

SigningTool 关键参数：

```text
-hv 2.3
-align
-nk
-of 0x80000000
-t fsbl
```

`-hv 2.3` 和 `-align` 会让 payload 相对 trusted bin 起始地址偏移 `0x400`，因此三段工程的链接地址和跳转地址都要考虑这个偏移。

## 外部 Flash 烧录布局

当前布局与官方 DK 外部 Flash boot 流程保持一致：

```text
FSBL trusted image            0x70000000
AppliSecure trusted image     0x70100000
AppliNonSecure trusted image  0x70180000
```

推荐使用 VS Code 任务：

```text
Flash: BootChain EXT (FSBL->Secure->NonSecure)
```

该任务会按顺序执行：

1. 构建三段工程。
2. 生成三段 trusted bin。
3. 使用 STM32CubeProgrammer CLI 和官方 external loader 烧录外部 Flash。

External loader：

```text
MX66UW1G45G_STM32N6570-DK.stldr
```

不要用 OpenOCD 的 `program <bin> 0x70000000 verify` 直接写外部 Flash。调试中已经验证过，当前 OpenOCD target 配置没有注册外部 Flash bank，会报 `Programming Failed`。外部 Flash 烧录走 CubeProgrammer + `.stldr`。

## 关键安全配置

### SAU

Secure 侧 SAU 配置位于：

```text
AppliSecure/Core/Inc/partition_stm32n657xx.h
```

当前关键区域：

```text
Region 0: NSC veneer
Region 1: NonSecure SRAM, 0x24100000 - 0x241FFFFF
Region 2: NonSecure peripheral window, 0x40000000 - 0x4FFFFFFF
```

如果 SAU 未启用，或没有把 NonSecure SRAM 配出来，Secure 跳转 NonSecure 时容易进入 SecureFault，典型表现是 `INVEP`。

### NSC veneer

Secure linker script 中需要正确导出 NSC veneer 边界：

```text
Makefile/AppliSecure/STM32N657XX_LRUN_s.ld
```

当前使用：

```ld
_sNSCVeneer = ADDR(.gnu.sgstubs);
_eNSCVeneer = ADDR(.gnu.sgstubs) + SIZEOF(.gnu.sgstubs) - 1;
```

调试时曾遇到 `_eNSCVeneer < _sNSCVeneer` 的异常，导致 SAU NSC 区域错误。后续如果新增 Secure callable 函数，应检查 map 文件中 `.gnu.sgstubs` 区域是否正常。

### RIF / RISAF

Secure 侧隔离配置位于：

```text
AppliSecure/Core/Src/main.c
```

当前策略：

- XSPI2 / XSPIM：Secure，用于 FSBL / Secure 配置外部 Flash。
- SPI4：NonSecure。
- USART3：NonSecure。
- GPDMA1 Channel 10 / 11：NonSecure / NonPrivileged，用于 SPI4 TX / RX DMA。
- GPIOB：NonSecure，供 AD_CS PB0 使用。
- GPIOD：NonSecure，供 USART3 PD8 / PD9 使用。
- GPIOE：NonSecure，供 AD_IRQ PE8 和 SPI4 PE12 / PE13 / PE14 使用。
- GPIOO：NonSecure，供 PO1 LED 使用。
- EXTI Line 8：NonSecure / NonPrivileged，用于 AD_IRQ。
- PB0 / PD8 / PD9 / PE8 / PE12 / PE13 / PE14 / PO1 pin attribute：NonSecure。
- RISAF3 CPU AXI RAM1：NonSecure 区域，用于 AppNS。
- RISAF2 / RISAF7：保持 Secure。

## 调试记录摘要

### 1. OpenOCD 不能直接烧外部 Flash

最初尝试用 OpenOCD：

```text
program <trusted.bin> 0x70000000 verify
```

结果失败，日志中出现 `flash bank` / `Programming Failed`。原因是 OpenOCD 当前没有配置外部 Flash bank。解决方式是改用 STM32CubeProgrammer CLI 加官方 external loader。

### 2. 先用 LED 排查，不优先依赖 UART

早期 BootChain / SecureFault 阶段，UART 可能因为时钟、RIF、SAU 或阻塞发送而不可用。调试时先把 PO1 用作状态灯，逐步区分：

- 是否进入 FSBL。
- 是否进入 Secure。
- 是否进入 SecureFault。
- 是否进入 NonSecure。
- 是否进入 NonSecure 主循环。

最终用 NonSecure 主循环 500 ms LED 翻转确认三段跳转链路已跑通。

### 3. SecureFault INVEP

调试中观察到过 SecureFault，LED 编码表现为“长闪 + 1 短闪 + 长停顿”，对应 `INVEP`。根因集中在 Secure 跳 NonSecure 时入口安全属性不正确。

最终修复点：

- 启用 Secure SAU。
- 将 `0x24100000 - 0x241FFFFF` 配为 NonSecure。
- 将外设地址窗口配为 NonSecure。
- 修正 `.gnu.sgstubs` 的 `_sNSCVeneer/_eNSCVeneer`。
- 确认 NonSecure vector table 位于 `0x24100400`。

### 4. 串口乱码

LED 已正常闪烁后，USART3 初次输出是乱码。原因不是程序未运行，而是 USART3 时钟源选择了 MSI，HAL 计算波特率使用的 `MSI_VALUE` 与 FSBL 实际配置后的 MSI 频率不一致。

修复方式：

- USART3 时钟源改为 `PCLK1`。
- 串口工具保持 `115200 8N1`。

修复后串口正常输出 AppNS start / heartbeat。

### 5. SPI4 DMA 采集链路

在 AppNS 中加入 SPI4 主机接收 AD7606 采集卡数据帧，使用 PE8 `AD_IRQ` 作为数据就绪握手，PB0 `AD_CS` 由软件控制。最初使用阻塞式 `HAL_SPI_TransmitReceive()` 验证帧头和 CRC，确认可以稳定收到：

```text
magic=0xAD76
frame_type=0x01
total_len=8240
payload_len=8212
CRC_OK
```

随后切换为 SPI4 + GPDMA1 双通道状态机：

- `AD_IRQ` 上升沿置 pending 标志。
- 主循环在 SPI 空闲时拉低 `AD_CS`，先 DMA 读取 24 字节帧头。
- 帧头合法后继续 DMA 读取剩余 payload + CRC。
- payload 完成后拉高 `AD_CS`，主循环解析帧、校验 CRC、统计质量。

DMA 初期曾偶发坏头：

```text
SPI4 DMA bad header magic_le=0x5AED bytes=ED 5A 02 02 60 40 28 40 ...
```

该坏头不是随机噪声：`0x4060 / 0x4028` 分别接近期望总长/载荷长度的 2 倍，说明更像是主机在采集卡端 SPI DMA 或发送缓冲尚未稳定时开始读。最终在 `AD_IRQ` 后、拉低 `AD_CS` 前加入 1 ms settle 时间：

```c
#define AD_SPI_IRQ_TO_CS_SETTLE_MS 1U
```

加入该延时后，长时间统计中 `dma_err / bad_hdr / crc_bad / raw_gap` 均保持为 0，帧率约 `50 frame/s`，吞吐约 `412000 B/s`。

## 常用检查清单

如果代码改了但板子现象没变，先检查：

- 是否重新 `Build: All`。
- 是否重新 `Sign: All`。
- 是否烧录的是新的 `*-trusted.bin`。
- `.vscode/settings.json` 中工程名和路径是否仍然指向当前工程。
- 外部 Flash 地址是否仍是 `0x70000000 / 0x70100000 / 0x70180000`。

如果 LED 不亮，先检查：

- 开发板是否切到 External Flash boot 模式。
- 三段 trusted bin 是否都下载和 verify 成功。
- PO1 是否配置为 NonSecure GPIO。
- Secure 是否进入 fault。
- NonSecure 是否进入 `Error_Handler`。

如果 USART 没输出或乱码，先检查：

- 串口工具是否为 `115200 8N1`。
- TX/RX 是否接反。
- USART3 是否使用 `PCLK1`。
- USART3 / GPIOD 是否在 RIF 中配置为 NonSecure。

如果 SPI4 采集异常，先检查：

- AD7606 采集卡是否已经启动并持续拉起 `AD_IRQ`。
- PE8 `AD_IRQ` 是否配置为 NonSecure EXTI rising edge。
- PB0 `AD_CS` 是否配置为 NonSecure GPIO output，并且空闲为高。
- PE12 / PE13 / PE14 是否为 SPI4 SCK / MISO / MOSI，且 RIF/pin attribute 为 NonSecure。
- SPI4 / GPDMA1 Channel 10 / 11 / EXTI Line 8 是否在 Secure RIF 中放给 NonSecure。
- `AD_SPI_IRQ_TO_CS_SETTLE_MS` 是否保留为当前验证过的 `1U`。
- 串口质量统计里 `crc_bad / dma_err / bad_hdr / bad_len / raw_gap` 是否持续为 0。

## 文档

更详细的调试流水账见：

```text
docs/STM32N6_VSCODE_MAKEFILE_BOOTCHAIN_DEBUG_NOTES.md
```

后续新增外设或业务逻辑时，建议每次只引入一个变量，并保留 LED / UART 这两个最小观测信号，避免 BootChain、TrustZone 和外设配置问题混在一起。
