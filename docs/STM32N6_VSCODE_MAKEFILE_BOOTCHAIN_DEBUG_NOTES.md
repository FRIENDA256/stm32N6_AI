# STM32N6 VSCode Makefile 三段 BootChain 调试记录

本文记录本工程从 CubeMX 生成的最小 Makefile 工程，到 VSCode 下完整编译、签名、烧录 FSBL / Secure / NonSecure 三段镜像，并最终跳入 NonSecure 主循环运行的调试流程和注意点。

当前已验证基线：外部 Flash 启动后，FSBL 装载 Secure 和 NonSecure 镜像，Secure 完成 SAU/RIF 配置并跳转 NonSecure，`AppliNonSecure/Core/Src/main.c` 主循环中 PO1 每 500 ms 翻转。

## 1. 初始工程范围

CubeMX 初始配置保持尽量小：

- 调试口：SWD / ST-LINK。
- LED：PO1，GPIO output。
- 外部 Flash 相关 XSPI/QSPI 引脚：使用 STM32N6570-DK 官方同款外部 Flash。
- 其它外设先不启用，避免 boot chain 尚未稳定时引入干扰。

本工程最终拆成三段：

- `FSBL`：第一阶段 bootloader，初始化外部 Flash 并执行 LRUN 装载。
- `AppliSecure`：Secure 应用，配置 TrustZone / SAU / RIF，然后跳转 NonSecure。
- `AppliNonSecure`：NonSecure 应用，当前用 PO1 500 ms 翻转验证主循环已经运行。

## 2. 关键地址布局

外部 Flash 写入地址与官方 DK 布局保持一致：

```text
FSBL trusted image            0x70000000
AppliSecure trusted image     0x70100000
AppliNonSecure trusted image  0x70180000
```

内部 RAM 链接地址：

```text
FSBL linker RAM origin        0x34180400
AppliSecure linker RAM origin 0x34000400
AppliNonSecure linker origin  0x24100400
```

LRUN 装载配置在 `FSBL/Core/Inc/stm32_extmem_conf.h`：

```c
#define EXTMEM_LRUN_DESTINATION_ADDRESS     0x34000000
#define EXTMEM_LRUN_SOURCE_ADDRESS          0x00100000
#define EXTMEM_LRUN_SOURCE_SIZE             0x00010000
#define EXTMEM_LRUN_TZ_ENABLE_NS
#define EXTMEM_LRUN_DESTINATION_ADDRESS_NS  0x34100000
#define EXTMEM_LRUN_SOURCE_ADDRESS_NS       0x180000
#define EXTMEM_HEADER_OFFSET                0x400
```

这里的 source address 是相对外部 Flash 映射基址的偏移，所以：

- `0x00100000` 对应外部 Flash 绝对地址 `0x70100000`。
- `0x00180000` 对应外部 Flash 绝对地址 `0x70180000`。

`EXTMEM_HEADER_OFFSET = 0x400` 非常关键：SigningTool 使用 header v2.3 并 `-align` 后，真实 vector table 位于 trusted bin 的 `0x400` 偏移处。Secure 和 NonSecure 链接脚本也都从 `...0400` 开始，必须和这个偏移一致。

## 3. VSCode 任务和构建顺序

推荐使用 `.vscode/tasks.json` 中的任务：

- `Build: All`
- `Sign: All`
- `Flash: BootChain EXT (FSBL->Secure->NonSecure)`

构建顺序必须是：

```text
AppliSecure -> FSBL -> AppliNonSecure
```

原因：`AppliNonSecure` 链接时依赖 `Makefile/AppliSecure/build/secure_nsclib.o`。如果先构建 NonSecure，可能链接到旧的或不存在的 NSC import library。

也可以手动执行：

```powershell
make -C Makefile/AppliSecure
make -C Makefile/FSBL
make -C Makefile/AppliNonSecure
```

## 4. 签名流程

三段镜像都需要用 STM32 SigningTool 生成 trusted bin，外部 Flash 启动时烧录 trusted bin，不要烧录 raw bin。

典型参数：

```powershell
STM32_SigningTool_CLI.exe `
  -bin <input.bin> `
  -nk `
  -of 0x80000000 `
  -t fsbl `
  -o <output-trusted.bin> `
  -hv 2.3 `
  -dump <output-trusted.bin> `
  -align `
  -s
```

注意点：

- `-hv 2.3` 和 `-align` 会把 payload 对齐到 `0x400`。
- 输出日志里的 entry point 应落在对应链接地址范围内。
- 修改代码后一定要重新签名，否则烧录任务可能仍然写入旧 trusted bin。

## 5. 烧录流程

外部 Flash 烧录使用 STM32CubeProgrammer CLI 和官方 external loader：

```text
ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr
```

当前 VSCode 任务实际流程：

```powershell
STM32_Programmer_CLI.exe `
  -c port=SWD mode=HOTPLUG freq=3300 `
  -el MX66UW1G45G_STM32N6570-DK.stldr `
  -w TEST_CMAKE_FSBL-trusted.bin 0x70000000 -v `
  -w TEST_CMAKE_AppliSecure-trusted.bin 0x70100000 -v `
  -w TEST_CMAKE_AppliNonSecure-trusted.bin 0x70180000 -v `
  -rst
```

调试期间用 Development mode + HOTPLUG 连接烧录。烧录完成后切到 External Flash boot，再 reset 或掉电重启验证外部 Flash 启动链路。具体 BOOT 开关/引脚状态以开发板丝印和官方手册为准；流程上要区分“烧录模式”和“外部 Flash 启动验证模式”。

不要用 OpenOCD 的 `program <bin> 0x70000000 verify` 直接写外部 Flash。本项目调试时这个方式报过：

```text
flash
...
Error: ** Programming Failed **
```

原因是当前 OpenOCD target 配置没有注册外部 Flash bank。外部 Flash 写入应走 CubeProgrammer + `.stldr` external loader。

## 6. Secure 跳 NonSecure 的关键配置

### 6.1 NonSecure vector table

Secure 侧使用：

```c
#define VECT_TAB_NS_OFFSET  0x00400
#define VTOR_TABLE_NS_START_ADDR   (SRAM2_AXI_BASE_NS | VECT_TAB_NS_OFFSET)
#define VTOR_TABLE_NS_START_ADDR_S (SRAM2_AXI_BASE_S  | VECT_TAB_NS_OFFSET)
```

流程：

1. `SCB_NS->VTOR = 0x24100400`。
2. 从 `0x24100400` 读取 NonSecure MSP。
3. 从 `0x24100404` 读取 NonSecure Reset_Handler。
4. 校验 MSP 范围、Reset_Handler Thumb bit 和地址范围。
5. `__TZ_set_MSP_NS(ns_msp)`。
6. 通过 `cmse_nonsecure_call` 跳转 NonSecure Reset_Handler。

调试中还保留了从 secure alias `0x34100400` 预读 vector 的 fallback，用于 RISAF 配置前后访问属性变化时排障。

### 6.2 SAU 配置

这次最关键的问题点是 SAU。CubeMX 生成的 Secure partition 初始状态中，SAU 可能是关闭的，或者没有把 NonSecure SRAM 配出来。这会导致 Secure 调用 NonSecure Reset_Handler 时进入 SecureFault。

当前 `AppliSecure/Core/Inc/partition_stm32n657xx.h` 需要满足：

```c
#define SAU_INIT_CTRL          1
#define SAU_INIT_CTRL_ENABLE   1
#define SAU_INIT_CTRL_ALLNS    0
```

Region 0：NSC veneer 区：

```c
#define SAU_INIT_REGION0    1
#define SAU_INIT_START0     ((uint32_t) &_sNSCVeneer)
#define SAU_INIT_END0       ((uint32_t) &_eNSCVeneer)
#define SAU_INIT_NSC0       1
```

Region 1：NonSecure SRAM：

```c
#define SAU_INIT_REGION1    1
#define SAU_INIT_START1     0x24100000
#define SAU_INIT_END1       0x241FFFFF
#define SAU_INIT_NSC1       0
```

Region 2：NonSecure 外设窗口：

```c
#define SAU_INIT_REGION2    1
#define SAU_INIT_START2     0x40000000
#define SAU_INIT_END2       0x4FFFFFFF
#define SAU_INIT_NSC2       0
```

### 6.3 NSC veneer 符号

Secure linker script 必须给 SAU region 0 提供正确的 NSC veneer 起止地址：

```ld
.gnu.sgstubs ALIGN(32) :
{
  . = ALIGN(32);
  KEEP(*(.gnu.sgstubs*))
  . = ALIGN(32);
} >RAM
_sNSCVeneer = ADDR(.gnu.sgstubs);
_eNSCVeneer = ADDR(.gnu.sgstubs) + SIZEOF(.gnu.sgstubs) - 1;
```

调试时曾出现 `_eNSCVeneer < _sNSCVeneer`，导致 SAU NSC 区间异常。修复后用 `nm` 验证过：

```text
_sNSCVeneer = 0x34001520
_eNSCVeneer = 0x3400153f
```

如果后续增删 NSC 函数，建议重新检查 map/nm，确认 `_eNSCVeneer` 大于 `_sNSCVeneer` 且 32 字节对齐逻辑合理。

### 6.4 RIF / RISAF / GPIO 安全属性

Secure 侧 `SystemIsolation_Config()` 中当前策略：

- XSPI2 / XSPIM：Secure。
- USART3 / GPIOD / GPIOO：NonSecure。
- RISAF3 region 1：NonSecure，覆盖 `0x0000-0xFFFFF`。
- RISAF2 / RISAF7：Secure。
- PD8 / PD9 / PO1 pin attribute：NonSecure。

PO1 要在 NonSecure 中直接控制，必须同时满足：

- GPIOO 外设 RIF 设置为 NonSecure。
- PO1 pin attribute 设置为 `GPIO_PIN_NSEC`。
- SAU region 2 把外设地址窗口设为 NonSecure。

## 7. LED 诊断约定

当前 LED 诊断用于早期 boot chain 排障。因为 UART 在 SecureFault/早期异常路径里可能因为时钟、RIF 或阻塞问题导致看不到输出，所以首选 LED。

### 7.1 正常路径

`AppliNonSecure/Core/Src/main.c` 主循环：

```c
HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_GPIO_PIN);
HAL_Delay(500U);
```

现象：PO1 每 500 ms 翻转，说明已经进入 NonSecure 主循环。

### 7.2 非正常路径

- FSBL `Error_Handler`：PO1 熄灭并停住。
- NonSecure `Error_Handler`：PO1 熄灭并停住。
- NonSecure fault handler：连续闪烁。
- Secure 普通 fault：N 次短闪 + 长停顿。
- SecureFault：长闪作为 marker，后跟 N 次短闪。

SecureFault 编码：

```text
长 + 1短  INVEP
长 + 2短  INVIS
长 + 3短  INVER
长 + 4短  AUVIOL
长 + 5短  INVTRAN
长 + 6短  LSPERR
长 + 7短  LSERR
长 + 8短  UNKNOWN
```

本次关键排障现象是“长 + 1短 + 长时间熄灭”，即 `INVEP`。根因是 Secure 到 NonSecure 的入口点安全属性不正确，最终通过启用 SAU、配置 NonSecure SRAM、修正 NSC veneer 符号解决。

## 8. 本次排障时间线摘要

1. 从 CubeMX 最小工程生成 Makefile 工程。
2. 参考官方 `Template_Isolation_LRUN` 拆出 FSBL / AppliSecure / AppliNonSecure。
3. 先尝试 OpenOCD `program ... 0x70000000` 写外部 Flash，失败，改用 CubeProgrammer external loader。
4. 外部 Flash 可以成功 erase / download / verify 三段 trusted bin。
5. External Flash boot 后无串口、LED 不按预期运行，开始用 PO1 做状态编码。
6. 先确认 FSBL / Secure / NonSecure 的 Error/Fault 路径，避免“常亮”和“正常运行”混淆。
7. 用 SecureFault LED 编码定位到 `INVEP`。
8. 检查 NonSecure vector：`0x24100400` MSP 和 `0x24100404` Reset_Handler 均有效。
9. 检查 Secure `NonSecure_Init()`：`blxns` 调用逻辑正常。
10. 对比官方 partition，发现 SAU 未启用/NonSecure SRAM 未配置。
11. 启用 SAU region 0/1/2，并修正 `.gnu.sgstubs` 起止符号。
12. 重建、重新签名、烧录，SecureFault 消失。
13. 在 NonSecure 主循环中加入 500 ms LED 翻转，确认最终已经进入 NonSecure 正常运行。

## 9. 常见坑和检查清单

### 9.1 代码改了但板子现象没变

检查：

- 是否重新 `Build: All`。
- 是否重新 `Sign: All`。
- 烧录的是 `*-trusted.bin` 还是旧文件。
- VSCode task 中路径是否仍指向当前工程。
- `make` 输出如果是 `Nothing to be done`，确认源文件时间戳和目标文件是否真的更新。

### 9.2 LED 不亮

按顺序判断：

1. 是否处于 External Flash boot 模式并复位/掉电重启。
2. FSBL trusted bin 是否在 `0x70000000`。
3. Secure trusted bin 是否在 `0x70100000`。
4. NonSecure trusted bin 是否在 `0x70180000`。
5. Secure 是否进入 fault 编码。
6. PO1 是否已经被配置为 NonSecure。
7. NonSecure 是否进入 `Error_Handler` 熄灭停住。

### 9.3 SecureFault INVEP

重点检查：

- SAU 是否 enable。
- `0x24100000-0x241FFFFF` 是否配置为 NonSecure。
- Reset_Handler 地址是否在 `0x24100400-0x241FFFFF`。
- Reset_Handler bit0 是否为 1。
- `_sNSCVeneer` / `_eNSCVeneer` 是否正确，尤其 `_eNSCVeneer` 不能小于 `_sNSCVeneer`。

### 9.4 UART 没输出

早期 boot / fault 路径不建议优先依赖 UART：

- UART 时钟和 pin mux 可能还没配置。
- RIF/SAU 可能不允许 NonSecure 访问。
- 阻塞式发送可能改变 LED 时序甚至让异常表现变慢。

建议先用 LED 编码定位阶段，再在明确进入 NonSecure 后开启 UART。

### 9.5 OpenOCD 烧外部 Flash 失败

若看到 `flash bank` / `Programming Failed`，不要在 OpenOCD 上继续绕。当前工作流用 CubeProgrammer external loader 烧外部 Flash。

## 10. 当前可用的基线

本地 git 已有基线提交和 tag：

```text
commit 8d7a98a Initial STM32N6 bootchain baseline
tag    baseline-bootchain-working
```

该基线验证结果：

- 三段 Makefile 工程可编译。
- 三段 trusted bin 可烧录到外部 Flash。
- External Flash boot 可以进入 NonSecure。
- PO1 每 500 ms 翻转。

后续增加外设或应用逻辑时，建议每加一类能力就验证一次 boot chain，不要一次性引入多个变量。
