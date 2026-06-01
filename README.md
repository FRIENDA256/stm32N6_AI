# stm32N6_AI

这是一个面向 `STM32N6570-DK` 的 VS Code + Makefile 测试工程，用来验证从 CubeMX 生成的最小配置工程，到 STM32N6 三段式外部 Flash 启动链路的完整开发流程。

当前工程已经验证：

- 外部 Flash boot 可以启动 FSBL。
- FSBL 可以从外部 Flash 加载 Secure / NonSecure 镜像。
- Secure 工程可以完成 SAU / RIF / RISAF 安全隔离配置。
- Secure 可以跳转到 NonSecure。
- NonSecure 主循环正常运行。
- PO1 LED 每 500 ms 翻转。
- USART3 通过 PD8 / PD9 输出心跳信息，波特率 `115200 8N1`。

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
- `AppliNonSecure`：当前作为功能验证区，负责 LED 闪烁和 USART3 心跳输出。

## 当前验证信号

NonSecure 主循环位于：

```text
AppliNonSecure/Core/Src/main.c
```

当前现象：

- PO1 LED 每 500 ms 翻转。
- 串口启动后输出：

```text
STM32N6_AI AppNS USART3 start
```

- 之后周期输出：

```text
STM32N6_AI AppNS heartbeat
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
- USART3：NonSecure。
- GPIOD：NonSecure，供 USART3 PD8 / PD9 使用。
- GPIOO：NonSecure，供 PO1 LED 使用。
- PO1 / PD8 / PD9 pin attribute：NonSecure。
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

## 文档

更详细的调试流水账见：

```text
docs/STM32N6_VSCODE_MAKEFILE_BOOTCHAIN_DEBUG_NOTES.md
```

后续新增外设或业务逻辑时，建议每次只引入一个变量，并保留 LED / UART 这两个最小观测信号，避免 BootChain、TrustZone 和外设配置问题混在一起。
