# Tiny1C 红外调试计划

## 1. 文档信息

| 项目 | 内容 |
| --- | --- |
| 工程 | `variants/fsbl_appli_all` |
| 目标平台 | STM32N657 + Tiny1C + AD7606 + 千兆以太网 + NPU |
| 文档版本 | V1.0 |
| 创建日期 | 2026-07-17 |
| N6 地址 | `192.168.6.50` |
| PC 地址 | `192.168.6.10` |
| 当前运行目标 | Tiny1C 图像 10 fps、温度 5 fps，AD7606 连续采集，AI 25 次/s |

## 2. 调试目标

1. 确认 Tiny1C 在 25 MHz SPI3 + HPDMA1 下能够长期稳定采集。
2. 定位 AD7606 并行运行时红外图像或温度帧散点增加的根因。
3. 验证 50 MHz SPI3 是否能够在当前 PCB、供电和 DMA 架构下稳定工作。
4. 建立可量化的帧完整性检测、错误统计和自动恢复机制。
5. 为后续 25 fps 红外采集、AI 使用和以太网连续上传确定可行配置。

## 3. 已知事实与当前基线

### 3.1 Tiny1C 协议边界

| 项目 | 数值或说明 |
| --- | --- |
| 分辨率 | 256 x 192 |
| 探测器最高帧率 | 25 Hz |
| 单幅 Y14 数据量 | 98,304 bytes |
| 图像 + 温度组合帧 | 256 x 384，196,608 bytes |
| 新帧前导 | 480 bytes dummy + 32 bytes header |
| 继续读命令 | `0x55` |
| 当前实测首帧命令 | 图像 `0xAA`，温度 `0xCC` |
| I2C | 400 kHz，7 位地址 `0x3C` |
| VDD50 最大允许噪声 | 200 uV，1 Hz 至 50 kHz |
| VDD33 最大允许噪声 | 50 mV，1 Hz 至 50 kHz |

产品手册时序图中的新帧命令为 `0xE1`，当前工程和独立工程实际使用 `0xAA/0xCC` 并能得到有效图像。没有对应固件版本的完整协议前，不修改当前有效命令，只记录并分析 32 字节帧头。

### 3.2 当前硬件与软件配置

| 资源 | 当前配置 |
| --- | --- |
| Tiny1C SPI | SPI3，默认 25 MHz，Mode 3，8 bit，MSB first |
| Tiny1C DMA | HPDMA1 Channel 0/1，RX/TX |
| AD7606 SPI | SPI4 + GPDMA1 Channel 10/11 |
| Tiny1C GPIO | PC10/PC11/PC12 当前为 Very High speed |
| Tiny1C 帧缓冲 | 单个 98,304-byte `tiny1c_frame` |
| 后台调度 | 图像 10 fps、温度 5 fps |
| 网络 | TCP 命令端口 5000，UDP 回显端口 5005 |

### 3.3 版本边界

| 版本 | 状态 | 说明 |
| --- | --- | --- |
| Git 基线 `570bee5` | 已验证 | SPI3 已迁移到 HPDMA1；25 MHz 可运行；50 MHz 仍有少量干扰 |
| 当前本地工作区 | 性能回退已复现，修复待验证 | 增加 SPI3/SPI4 DMA cache 维护、32-byte 对齐及 Tiny1C DMA bounce buffer；A-B-A 已确认网络回退来自当前构建，现增加 NetX packet/payload 显式 32-byte 对齐 |

所有测试必须记录 Git commit、工作区是否有修改、固件生成时间以及是否完整断电重启。

## 4. 指标定义

| 指标 | 含义 | 关注点 |
| --- | --- | --- |
| `CRC OK` | PC 接收数据与板端 CRC 一致 | 只能证明以太网传输后数据未变化，不能证明 SPI 原始数据正确 |
| `err` | Tiny1C 采集失败次数 | 应保持为 0 |
| `late` | 调度未按期完成次数 | 长时间运行时应低且不持续快速增加 |
| `Temp raw jumps total` | 相邻像素差超过阈值的数量 | 在均匀目标下应接近 0 |
| `max_delta` | 最大相邻像素差 | 用于识别位错、突发干扰和局部坏点 |
| `repaired` | PC 端中值修复像素数 | 仅用于诊断，不作为固件正确性的替代 |
| `CRC32` 变化 | 原始帧是否随时间变化 | 多帧长期完全相同可能是固定帧或模块冻结 |
| `avg_ms/max_ms` | 单帧采集耗时 | 用于判断目标帧率是否有足够余量 |
| `dma_err/hal` | DMA/HAL 错误 | 应为 0 |

### 4.1 测试场景要求

- 链路测试使用静止、温度均匀的目标覆盖全部视场。
- 画质测试使用包含明显冷热区域和边缘的固定场景。
- 每组 A/B 测试中不得移动模块、线缆或目标。
- 模块完整断电后，至少等待 30 秒再开始链路测试。
- 涉及标定或画质比较时，建议预热至少 5 分钟。
- 保存原始 `.raw`、头文件 `.txt`、未滤波 BMP 和滤波 BMP。

## 5. 调试阶段与测试表

### 阶段 A：当前实验固件基本回归

目的：确认 cache/bounce-buffer 修改没有破坏 25 MHz 基线。

| 编号 | 操作 | 预期结果 | 状态 | 结果记录 |
| --- | --- | --- | --- | --- |
| A1 | 完整断电、烧录、重新上电 | 串口启动完整，无 HardFault | 基本通过 | 板端持续可通信，未附本轮串口启动日志 |
| A2 | `ping 192.168.6.50` | 0% 丢包 | 通过 | 4/4 回复，延迟小于 1 ms |
| A3 | 连续两次 `IRSTAT`，间隔 10 秒 | `err=0`，图像约 10 fps，温度约 5 fps | 性能未通过 | `err=0`；累计图像约 6.6 fps，温度约 4.6 fps；单帧约 80 ms，`late=406` |
| A4 | 连续抓取 5 幅温度帧 | 全部 CRC OK，无固定占位帧 | 通过 | 实际抓取 10 帧；CRC 全部正确且各不相同；每帧 jumps 为 1 至 44 |
| A5 | 抓取图像、温度、AD7606 各一帧 | 三类数据均可用 | 通过 | 三类数据 CRC 均正确，图像肉眼正常，AD7606 CH4 为 6862 至 6868 |
| A6 | 查询 `AISTAT` | `ready=1 fault=0 run_err=0` | 通过 | `runs=7030`、`run_err=0`、当前/平均 8 ms，最大 238 ms |
| A7 | UDP `--no-fill` 测试 | 无丢包，吞吐无明显回退 | 部分通过 | 0 丢包，但板端 224.90 Mbps，相对约 279.50 Mbps 基线下降约 19.5% |

建议命令：

```powershell
ping 192.168.6.50

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 -Command IRSTAT
Start-Sleep -Seconds 10
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 -Command IRSTAT

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\ir_capture_idle_test.ps1 `
  -Iterations 5 -IdleMs 0

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_binary_capture.ps1 -Command IRGETIMG -TimeoutMs 90000
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_binary_capture.ps1 -Command IRGETTEMP -TimeoutMs 90000
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_binary_capture.ps1 -Command ADGET

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 -Command AISTAT

python .\tools\net_throughput_test.py --mode udp --no-fill --udp-ms 5000
```

阶段 A 通过条件：无 DMA/HAL 错误，连续温度帧不是固定占位帧，AD7606 和 AI 保持正常，网络无丢包。

#### 阶段 A 执行记录：2026-07-17

总体结论：**功能完整性通过，实时性能未通过。** 当前 cache/bounce-buffer 实验版没有出现 CRC 错误、DMA/HAL 错误、固定帧或肉眼可见的随机黑点，但红外采集和网络吞吐均有明显性能回退，暂不适合作为正式基线提交。

| 测试项 | 结果 |
| --- | --- |
| Ping | 0% 丢包，延迟小于 1 ms |
| IR 累计帧率 | 图像约 6.60 至 6.62 fps，温度约 4.58 至 4.60 fps |
| IR 最近 10 秒增量 | 图像约 7.23 fps，温度约 4.98 fps |
| IR 采集状态 | `err=0`，`last_ms=80`，`max_ms=5084`，`late=406` |
| 后台运行温度帧 | JumpAvg 22.8，范围 1 至 44；RepairAvg 2.0 |
| IR 后台暂停温度帧 | JumpAvg 34.4，范围 7 至 44；RepairAvg 3.2 |
| 温度数据范围 | 原始约 18280 至 19704，非固定占位数据 |
| AD7606 | CRC 正确，512 点 x 8 通道，CH4 平均 6865 |
| AI | `ready=1`、`fault=0`、`run_err=0`，平均 8 ms |
| UDP | 224.90 Mbps，100% 接收，0 丢包 |

结果解释：

1. 10 幅温度帧 CRC 全部不同，排除了本轮测试中的固定帧和模块冻结。
2. 未滤波 BMP 肉眼连续，没有随机黑点、错行或条带；每帧最多修复 4 个像素。
3. `IR_PAUSED_IDLE` 只暂停 Tiny1C 后台调度，并没有暂停 AD7606。暂停后 JumpAvg 没有下降，说明 Tiny1C 后台采集调度不是当前少量跳变的主要来源。
4. 当前结果不能代替阶段 B 的 `ADPAUSE/ADRESUME` 对照。
5. 红外单帧耗时由此前约 45 ms 上升到约 80 ms，与图像帧率降至约 6.6 fps 相符。优先检查每个 4096-byte 分块上的 cache clean/invalidate 和 bounce-buffer memcpy 开销。
6. UDP 相对历史约 279.50 Mbps 下降约 19.5%，需要区分是 SPI3/SPI4 cache 维护开销、红外线程负载还是本次网络波动。

阶段 A 后续补充测试：

| 编号 | 测试条件 | 目的 | 状态 |
| --- | --- | --- | --- |
| A8 | `IRPAUSE` 后执行 UDP `--no-fill` | 判断 Tiny1C 后台负载对网络吞吐的影响 | 已执行：225.11 Mbps，0 丢包 |
| A9 | `ADPAUSE` 后执行 UDP `--no-fill` | 判断 SPI4/cache 维护对网络吞吐的影响 | 已执行：224.94 Mbps，0 丢包 |
| A10 | Tiny1C 与 AD7606 同时暂停后执行 UDP | 获取当前固件的纯网络上限 | 已执行：224.94 Mbps，0 丢包 |
| A11 | 重复三次正常负载 TCP/UDP 测试 | 排除单次网络波动 | 已执行：TCP 平均 179.56 Mbps，UDP 平均 224.79 Mbps |

#### 阶段 A 补充执行记录：A8 至 A10

| 条件 | UDP 板端速率 | 接收率 | 丢包 |
| --- | ---: | ---: | ---: |
| Tiny1C 与 AD7606 正常运行 | 224.90 Mbps | 100% | 0 |
| 仅暂停 Tiny1C 后台采集 | 225.11 Mbps | 100% | 0 |
| 仅暂停 AD7606 | 224.94 Mbps | 100% | 0 |
| Tiny1C 与 AD7606 同时暂停 | 224.94 Mbps | 100% | 0 |

四次结果范围只有 224.90 至 225.11 Mbps，最大差值 0.21 Mbps，小于 0.1%。因此：

1. 可以排除运行中的 SPI3、SPI4、红外后台线程和 AD7606 DMA 负载是本轮 UDP 回退的主要原因。
2. 暂停 AD7606 后 AI 不再获得新输入，但 UDP 速率没有提高，因此 NPU 推理负载也不是主要原因。
3. 当前约 225 Mbps 更像当前固件构建、网络数据路径或外部链路环境形成的稳定上限。
4. NetX 状态正常：`pool_total=40`、`pool_free=32`、`pool_empty=0`、`byte_avail=20064`，没有内存池耗尽迹象。
5. 当前网络配置文件与 Git 基线相比没有实质参数变化，不能仅凭本轮数据归因到 NetX 配置。
6. 下一步先完整断电并重复三组 TCP/UDP `--no-fill`；如仍稳定在约 225 Mbps，再用 Git `570bee5` 固件做同环境 A/B 对照。

#### 阶段 A 补充执行记录：A11

| 轮次 | TCP PC | TCP 板端 | UDP PC | UDP 板端 | UDP 丢包 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 179.28 Mbps | 179.80 Mbps | 223.94 Mbps | 224.63 Mbps | 0 |
| 2 | 179.20 Mbps | 179.68 Mbps | 224.36 Mbps | 224.94 Mbps | 0 |
| 3 | 178.58 Mbps | 179.20 Mbps | 224.08 Mbps | 224.80 Mbps | 0 |
| 平均 | 179.02 Mbps | 179.56 Mbps | 224.13 Mbps | 224.79 Mbps | 0 |

A11 结论：

1. TCP 板端三次范围为 179.20 至 179.80 Mbps，UDP 板端范围为 224.63 至 224.94 Mbps，结果高度稳定。
2. UDP 无 gap、无 bad magic、接收率 100%，链路可靠性正常，只是吞吐上限降低。
3. 测试前后 NetX 状态一致：`pool_total=40`、`pool_free=32`、`pool_empty=0`、`byte_avail=20064`，排除持续内存泄漏和包池耗尽。
4. 相对历史板端约 226.15 Mbps TCP、282.53 Mbps UDP，当前 TCP 下降约 20.6%，UDP 下降约 20.4%。两种协议按相同比例回退，说明更可能是共享的 CPU、内存/cache 或网络发送路径发生变化。
5. 当前网络配置文件与 `570bee5` 相比没有实质参数差异。下一步应在相同 PC、网卡、线缆和交换机条件下烧录 `570bee5` 固件做 A/B；不再通过重复当前固件测试来定位。

#### Git `570bee5` 对照固件结果

| 轮次 | TCP PC | TCP 板端 | UDP PC | UDP 板端 | UDP 丢包 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 220.31 Mbps | 220.57 Mbps | 279.05 Mbps | 279.50 Mbps | 0 |
| 2 | 219.95 Mbps | 220.39 Mbps | 278.78 Mbps | 279.29 Mbps | 0 |
| 3 | 218.57 Mbps | 218.77 Mbps | 278.62 Mbps | 279.19 Mbps | 0 |
| 平均 | 219.61 Mbps | 219.91 Mbps | 278.82 Mbps | 279.33 Mbps | 0 |

与当前实验固件相比，`570bee5` 的 TCP 板端速率提高约 22.5%，UDP 板端速率提高约 24.3%；换算为当前实验固件相对基线，TCP 下降约 18.3%，UDP 下降约 19.5%。外部网络链路能够达到约 279 Mbps，已排除 PC、网卡、线缆和交换机形成 225 Mbps 固定上限。

该测试还没有完成最终归因，因为烧录 `570bee5` 同时复位了板端。必须完成 A-B-A 复测：

1. A：当前实验固件在长时间运行后约为 TCP 179.56 Mbps、UDP 224.79 Mbps。
2. B：重新烧录 `570bee5` 后约为 TCP 219.91 Mbps、UDP 279.33 Mbps。
3. A2：重新烧录当前实验固件并在同样的启动等待时间后重复三次测试。

若 A2 再次约为 TCP 180 Mbps、UDP 225 Mbps，可确认回退来自当前 cache/bounce 实验构建。若 A2 恢复到约 TCP 220 Mbps、UDP 279 Mbps，则需要调查复位前的长期运行状态、错误恢复或外设状态积累。

#### A-B-A 最终结果：当前实验固件回切 A2

| 轮次 | TCP PC | TCP 板端 | UDP PC | UDP 板端 | UDP 丢包 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 178.70 Mbps | 179.20 Mbps | 224.10 Mbps | 224.91 Mbps | 0 |
| 2 | 179.22 Mbps | 179.44 Mbps | 224.20 Mbps | 224.82 Mbps | 0 |
| 3 | 178.69 Mbps | 179.32 Mbps | 224.14 Mbps | 224.73 Mbps | 0 |
| 平均 | 178.87 Mbps | 179.32 Mbps | 224.15 Mbps | 224.82 Mbps | 0 |

A-B-A 结论：**网络吞吐回退由当前实验构建稳定复现，已排除复位状态和外部网络环境。** 相对 `570bee5`，当前实验固件 TCP 板端下降约 18.5%，UDP 板端下降约 19.5%。三轮 UDP 均无丢包，说明问题是吞吐上限下降，不是链路可靠性下降。

压力测试结束时，`IRSTAT` 为图像 4.41 fps、温度 2.91 fps、`err=0`、`last_ms=72`、`max_ms=6584`；该累计帧率包含连续 TCP/UDP 满负载阶段，只用于说明调度压力，不能替代空闲状态帧率验收。`AISTAT` 保持 `fault=0`、`run_err=0`，但最大推理间隔达到 1508 ms，同样反映满负载下的长尾调度延迟。

下一项单变量验证：将 NetX `NX_PACKET_ALIGNMENT` 设为 32 字节，显式对齐 packet pool，并按 64 字节对齐后的 `NX_PACKET` 头重新计算 40 包内存。烧录后先用 `STAT` 确认 `pool_off=0`、`data_off=0`，再重复三组 TCP/UDP `--no-fill`。若吞吐恢复，则可确认此前静态 DMA buffer 对齐改变 `.bss` 布局，间接改变 NetX packet payload 的 cache-line 对齐。

首次对齐验证固件没有真正启用该宏：`nx_user.h` 中的配置仍位于 CubeMX 示例注释块内。板端 `STAT` 显示 `pool_off=0x10`、`data_off=0x0C`，三轮平均 TCP 板端约 181.01 Mbps、UDP 板端约 225.74 Mbps，与未对齐实验版基本一致，因此该轮不能用于判断 32-byte packet 对齐效果。反汇编同时确认该固件仍申请 63843 字节、按 60 字节包头和 4 字节对齐编译。

已将 `NX_PACKET_ALIGNMENT 32U` 移出注释并重新编译。修正版反汇编确认 packet pool 申请 64031 字节、有效区 64000 字节，调用前执行 `+31` 和清除低 5 位；NetX 的 `nx_packet_pool_create` 也按 32 字节执行取整。修正版二进制 SHA-256 为 `F75059602018E23067F1C8A12925E9200782871DE32745D7F5A9B3D0162828AC`，等待板端复测。

板端复测确认 32-byte 对齐已经生效：`pool_addr=0x341C4740`、`pool_off=0`、首包 `data_addr=0x341C4780`、`data_off=0`，packet 数仍为 40，包池无耗尽。但是连续九轮平均 TCP 板端约 179.65 Mbps、UDP 板端约 225.70 Mbps，与未对齐实验版相同。因此 **NetX packet/payload 对齐不是本次约 20% 吞吐回退的原因**，该实验配置应撤销。

第一次三轮满负载后，AI 出现 `run_err=1`、`err_ms=5009`、`rt=1`，随后两组三轮测试结束时 `runs=922`、`last_seq` 和输出均未再变化，说明 AI worker 在约 5 秒运行时超时后没有自动恢复。网络链路仍保持 100% 接收率和零丢包。这一问题与吞吐回退分开记录，后续需要补充 AI 超时后的运行时复位和恢复流程。

下一版单变量控制固件恢复原始 NetX 配置，并通过编译期开关同时关闭 AD7606 与 Tiny1C 新增的 DMA cache 维护、32-byte 静态 buffer 对齐和 Tiny1C bounce-buffer，其他 HPDMA、采集线程、AI、命令和网络配置保持不变。若吞吐恢复到 `570bee5` 水平，再分别单独启用两组逻辑定位责任项。

### 阶段 B：25 MHz 下 AD7606 并行干扰 A/B 测试

目的：判断 25 MHz 下残余散点是否与 AD7606 并行运行直接相关。

| 编号 | AD7606 | Tiny1C 后台 | 温度帧数 | 记录内容 | 状态 |
| --- | --- | --- | --- | --- | --- |
| B1 | 运行 | 运行 | 30 | 每帧 jumps、max_delta、repaired、CRC | 通过 |
| B2 | 暂停 | 运行 | 30 | 同 B1 | 通过 |
| B3 | 运行 | 暂停后按需抓取 | 30 | 同 B1 | 待执行 |
| B4 | 暂停 | 暂停后按需抓取 | 30 | 同 B1 | 待执行 |

#### 阶段 B 首轮结果：Tiny1C cache/bounce 单独启用版本

烧录文件：`fsbl_appli_all_Appli_cache01_tiny_only-trusted.bin`。测试时间为 2026-07-17，使用 `ir_ad_contention_test.ps1` 分别采集 AD7606 运行和暂停状态下的 10 帧温度数据。

| 模式 | 帧数 | CRC 错误 | jumps 平均/最大 | max_delta 范围 | repaired 平均/最大 |
| --- | ---: | ---: | ---: | ---: | ---: |
| AD7606 运行 | 10 | 0 | 0 / 0 | 408 至 440 | 0 / 0 |
| AD7606 暂停 | 10 | 0 | 0 / 0 | 408 至 420 | 0 / 0 |

两组全部使用小端完整温度行解析，`nonzero=49128`，未出现错行、字节序混合或固定占位帧。抽查两组未滤波图未见随机黑点、条带或帧结构异常。该结果说明在本次短时样本中，AD7606 并行运行没有引入可测量的温度像素跳变，Tiny1C 对齐 bounce/cache 方案具备继续验证的条件。

该首轮结果仅作为 10+10 帧短时预验证，后续继续执行原计划的 30 帧、多轮重启和长时间连续运行验收。还需增加图像帧孤立黑点统计，避免仅凭温度帧阈值漏掉低幅度图像异常。

#### 阶段 B 30 帧验收结果

随后按相同条件完成 AD7606 运行和暂停状态各 30 帧测试。60 帧全部 CRC 正确，全部解析为 `le_rows=192 be_rows=0`，所有帧 `repaired=0`。

| 模式 | 帧数 | CRC 错误 | jumps 平均/最大 | 异常帧数 | repaired 平均/最大 |
| --- | ---: | ---: | ---: | ---: | ---: |
| AD7606 运行 | 30 | 0 | 0.1 / 2 | 1 | 0 / 0 |
| AD7606 暂停 | 30 | 0 | 0 / 0 | 0 | 0 / 0 |

AD7606 运行组第 26 帧出现 2 次超过 512 的相邻像素差，最大值为 660。原始坐标分别为 `(245,158)` 水平方向和 `(0,166)` 垂直方向，均位于真实场景或画面边缘，不是同一孤立像素；未滤波图中也未见随机黑点、错行或条带。因此该帧不判定为链路异常。

阶段 B 的 B1/B2 判定为通过：在本轮 30+30 帧测试中，没有证据表明 AD7606 并行运行会破坏 Tiny1C 温度帧。B3/B4 仍保留，用于区分后台采集线程调度与按需抓取路径；图像帧孤立黑点统计和多次冷启动长时间测试仍需继续。

第二次独立执行 30+30 帧复测，两组均为 `JumpAvg=0`、`JumpMax=0`、`RepairAvg=0`、`RepairMax=0`。AD7606 运行组每帧 `max_delta` 为 420 至 440，暂停组为 420 至 452，未观察到状态相关差异。温度帧 B1/B2 因此完成重复验证。

#### 图像帧黑点量化 A/B

`tcp_binary_capture.ps1` 已增加 8-bit 图像空间噪声统计，默认阈值为 48 灰度级，输出相邻像素跳变总数、孤立暗点/亮点数、最大偏差及其坐标。历史帧离线校准结果：

| 历史样本 | spatial jumps | dark | bright | 判定 |
| --- | ---: | ---: | ---: | --- |
| 2026-07-17 干净帧 | 549 | 27 | 11 | 正常场景边缘 |
| 2026-07-15 高密度散点帧 | 57277 | 18502 | 12669 | 明显故障 |

两个样本相差两个数量级，阈值 48 可用于识别此前的高密度黑点故障。板端 A/B 使用：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\ir_ad_image_contention_test.ps1 `
  -Iterations 10 `
  -Threshold 48 `
  -SettleMs 2000
```

首轮先执行 10+10 帧。若两组均保持在干净帧数量级且图像无随机散点，再扩展为 30+30 帧和多次冷启动验证。

首轮板端 10+10 帧结果：

| 模式 | 帧数 | CRC 错误 | jumps 平均/范围 | dark 平均/范围 | bright 平均/范围 |
| --- | ---: | ---: | ---: | ---: | ---: |
| AD7606 运行 | 10 | 0 | 847.2 / 830 至 880 | 65.3 / 59 至 75 | 32.9 / 31 至 35 |
| AD7606 暂停 | 10 | 0 | 811.3 / 731 至 864 | 58.1 / 42 至 72 | 33.8 / 26 至 39 |

两组所有帧均比历史高密度散点故障低约两个数量级，未出现单帧突增。最强暗点和亮点坐标几乎始终固定在 `(233,183)` 与 `(233,177)`，对应场景边缘而非随机像素。抽查两组极值帧均未见随机黑点、错行或条带。

AD7606 运行组相对暂停组 `jumps` 高约 4.4%，`dark` 高约 12.4%，但测试期间人物位置和距离发生明显变化，该差值包含场景运动，不能归因于 AD7606。首轮图像 A/B 判定通过。下一轮 30+30 帧应保持镜头和被摄场景完全静止，以便进一步比较统计分布。

固定场景下完成 30+30 帧复测：

| 模式 | 帧数 | CRC 错误 | jumps 平均/范围 | dark 平均/范围 | bright 平均/范围 |
| --- | ---: | ---: | ---: | ---: | ---: |
| AD7606 运行 | 30 | 0 | 721.1 / 631 至 845 | 68.4 / 59 至 83 | 37.9 / 31 至 44 |
| AD7606 暂停 | 30 | 0 | 768.3 / 702 至 813 | 68.9 / 60 至 76 | 39.3 / 35 至 44 |

AD7606 运行组的 `jumps` 反而比暂停组低约 6.1%，`dark` 低约 0.7%，`bright` 低约 3.6%；差异均未表现为 AD7606 运行时的系统性劣化。全部 60 帧均处于正常场景数量级，无单帧突增，主要极值坐标固定在场景边缘。图像帧 AD 运行/暂停 A/B 判定通过。

结合两轮温度 30+30 帧和图像 30+30 帧结果，当前没有证据表明 AD7606 并行运行会破坏 Tiny1C 数据。下一阶段进入多次冷启动与长时间连续运行验证。

AD7606 控制命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 -Command ADPAUSE

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 -Command ADRESUME
```

分析方法：

1. 分别计算每组 `jumps` 和 `repaired` 的平均值、P95 和最大值。
2. 统计异常像素坐标是否在多帧中固定。
3. 如果异常坐标随机且 AD7606 运行时显著增加，优先检查总线、cache、供电和地弹。
4. 如果异常坐标长期固定且与 AD7606 状态无关，再进入盲元诊断。

阶段 B 通过条件：AD7606 运行组相对暂停组不出现数量级增长，且无肉眼可见随机黑点或条带。

### 阶段 C：50 MHz HPDMA 稳定性测试

目的：判断 50 MHz 能否作为正式运行配置，而不只是完成 DMA 传输。

先测试 AD7606 运行状态：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 `
  -Command "IRSPI50DMA 100" `
  -TimeoutMs 60000
```

再测试 AD7606 暂停状态：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 -Command ADPAUSE

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 `
  -Command "IRSPI50DMA 100" `
  -TimeoutMs 60000

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 -Command ADRESUME
```

| 编号 | AD7606 | `ok/err` | `avg_ms/max_ms` | `jumps/jump_max` | `delta` | 结论 |
| --- | --- | --- | --- | --- | --- | --- |
| C1 | 运行 | | | | | |
| C2 | 暂停 | | | | | |
| C3 | 运行，重复测试 | | | | | |
| C4 | 暂停，重复测试 | | | | | |

50 MHz 暂定通过条件：

- 每组 `ok=100`、`err=0`、`hal=0`。
- `max_ms` 不超过 35 ms，且没有周期性长尾。
- AD7606 运行组相对暂停组的跳变平均值增加不超过 20%。
- 原始帧中无可见随机散点、错行、条带或字节错位。
- 连续重复 3 组后指标无持续恶化。

任何一项不满足时，50 MHz 仅保留为实验配置，正式运行继续使用 25 MHz。

### 阶段 D：cache 与 DMA 一致性验证

目的：确认异常不是 cache line 对齐、脏数据回写或 DMA 目标地址重叠造成。

| 编号 | 测试版本 | Tiny1C DMA 策略 | AD7606 cache 维护 | 结果 |
| --- | --- | --- | --- | --- |
| D1 | Git `570bee5` | 原始直接接收 | 无新增维护 | |
| D2 | 第一版实验 | 直接接收 + cache 维护 | 有 | 已观察到 50 MHz 跳变增多 |
| D3 | 当前实验 | 对齐 bounce buffer + memcpy | 有 | 待验证 |

检查项目：

- DMA TX/RX 缓冲地址和长度均满足 32-byte cache line 要求。
- DMA 前 clean TX，DMA 前 clean+invalidate RX，DMA 后 invalidate RX。
- 不对未对齐的帧中间地址直接执行 cache line invalidate。
- ISR 中只做必要状态处理，不执行大块复制或耗时日志。
- SPI3 和 SPI4 DMA 完成回调不会误处理对方的句柄。

### 阶段 E：SPI 信号完整性测试

目的：确认 50 MHz 异常是否来自时钟、MISO、CS 的振铃或时序裕量不足。

| 测试项 | 25 MHz | 50 MHz | 判定 |
| --- | --- | --- | --- |
| SCK 高低电平与占空比 | | | |
| SCK 过冲/下冲 | | | |
| SCK 振铃持续时间 | | | |
| MISO 建立/保持时间 | | | |
| CS 拉低至首个时钟间隔 | | | |
| CS 块间波形 | | | |
| AD7606 运行时波形变化 | | | |

GPIO speed A/B 测试顺序：`Low -> High -> Very High`。独立工程曾使用 Low speed，而当前工程使用 Very High。应以示波器波形和错误率决定配置，不以档位越高越好作为判断依据。

如 50 MHz 存在明显振铃，可评估：

- SCK 和 MOSI 源端串联 22 至 47 ohm 电阻。
- 缩短排线、增加连续地回流、避免与 AD7606 时钟平行布线。
- 降低 GPIO slew rate。
- 检查 CS 上升/下降沿及外部上拉。

### 阶段 F：供电与地完整性测试

目的：验证 AD7606 运行是否通过供电或地耦合影响 Tiny1C 模拟输出。

| 测量点 | AD7606 暂停 | AD7606 运行 | 手册限值 | 结论 |
| --- | --- | --- | --- | --- |
| Tiny1C VDD50 纹波 | | | 200 uV，1 Hz 至 50 kHz | |
| Tiny1C VDD33 纹波 | | | 50 mV，1 Hz 至 50 kHz | |
| VDD33 快门瞬态跌落 | | | 3.15 V 至 3.45 V | |
| Tiny1C 地与 N6 地电位差 | | | 越低越好 | |
| SPI3 工作时 VDD50 频谱 | | | 不出现与 SPI4 同步的明显峰值 | |

优先在 Tiny1C 供电引脚附近测量，不使用远端电源入口作为唯一测量点。若发现耦合，评估独立 LDO、磁珠、局部去耦和星形回流。

### 阶段 G：帧头、坏帧检测与恢复

目的：让系统能够识别错误帧，而不是依赖 PC 端中值滤波隐藏问题。

计划修改：

1. 保存新帧前导中的 32-byte header，不再全部丢弃。
2. 增加帧头十六进制调试命令和多帧对比。
3. 根据实测变化确认帧类型、帧号、状态位或长度字段。
4. 增加重复 CRC 计数，识别固定帧和模块冻结。
5. 增加连续采集错误阈值和分级恢复。

建议恢复顺序：

1. 当前帧失败，释放 CS 并清理 SPI/DMA 状态。
2. 连续 2 帧失败，重新初始化 SPI3 DMA 并重启 preview。
3. 连续多次失败或 CRC 长期固定，执行 Tiny1C `RESET_N` 硬件复位。
4. 硬件复位仍失败时，切换 Tiny1C 电源并重新初始化。

当前 `.ioc` 中没有明确的 Tiny1C `RESET_N` 控制引脚。后续 PCB 或引脚资源允许时，应将 RESET_N 或 Tiny1C 独立电源使能连接到 MCU。

### 阶段 H：缓冲与连续采集架构

目的：避免采集、AI 和网络发送同时访问同一帧缓冲。

计划结构：

```text
Tiny1C DMA producer
        |
        v
FREE -> FILLING -> READY -> IN_USE -> FREE
                    |          |
                    |          +-- Ethernet publisher
                    +------------- AI / temperature processor
```

最低要求：

- 使用双缓冲；网络和 AI 同时工作时建议三缓冲。
- 帧描述符包含类型、序号、时间戳、长度、CRC、错误标志和消费者引用计数。
- 采集线程不得等待慢速网络客户端。
- 消费者落后时按策略丢弃旧帧，保留最新完整帧。
- 图像和温度需要融合时，保存共同采集周期或配对序号。

### 阶段 I：25 fps 能力评估

理论数据量：

| 模式 | 原始带宽 | 25 MHz SPI 利用率 | 50 MHz SPI 利用率 |
| --- | ---: | ---: | ---: |
| 256 x 192 单帧，25 fps | 19.66 Mbps | 78.6% | 39.3% |
| 图像 + 温度组合帧，25 fps | 39.32 Mbps | 不可行 | 78.6% |

以上未包含 512-byte 首帧前导、继续读 dummy、CS 间隔、HAL/DMA 启停和线程调度开销。

建议分级目标：

| 等级 | 配置 | 验收目标 |
| --- | --- | --- |
| I1 | 25 MHz，图像 10 fps + 温度 5 fps | 当前正式基线，长期稳定 |
| I2 | 50 MHz，图像 25 fps | 连续 30 分钟无错误和可见散点 |
| I3 | 50 MHz，图像 25 fps + 温度低频 | AI 和网络并行时仍稳定 |
| I4 | 图像 + 温度均 25 fps | 优先评估 DVP；SPI 只作为备选 |

### 阶段 J：长期稳定性验收

| 时长 | 工作负载 | 验收内容 | 状态 |
| --- | --- | --- | --- |
| 30 分钟 | Tiny1C + AD7606 + AI | 无采集错误、无固定帧、AI 无 fault | 待执行 |
| 2 小时 | 加入周期性图像/温度网络读取 | 网络无断连，帧 CRC 正常 | 待执行 |
| 8 小时 | 完整目标负载 | 无内存池耗尽、无 DMA 停止、无异常复位 | 待执行 |

每 10 分钟记录：`IRSTAT`、`AISTAT`、NetX `STAT`、最新帧 CRC、DMA 错误计数和最大采集耗时。

## 6. 盲元与传输错误的区分

| 特征 | 固定盲元 | SPI/供电干扰 |
| --- | --- | --- |
| 像素坐标 | 多帧长期固定 | 随帧变化或形成随机条带 |
| 与 AD7606 状态关系 | 通常无关 | 运行时可能显著增加 |
| 与 SPI 频率关系 | 通常无关 | 高频时可能增加 |
| 处理方式 | 官方盲元表、3 x 3 中值检测 | 修复传输、信号或供电根因 |

只有在均匀目标下采集 30 帧以上，完成时域平均后仍能确认固定坐标，才进入官方盲元标定流程。不得将随机传输错误写入模块盲元表。

## 7. 测试记录模板

### 7.1 固件信息

```text
日期：
测试人：
Git commit：
工作区是否有修改：
固件生成时间：
是否完整断电：
Tiny1C SPI：25 MHz / 50 MHz
GPIO speed：Low / High / Very High
AD7606：运行 / 暂停
AI：运行 / 暂停
网络负载：无 / TCP / UDP / 连续上传
目标场景：均匀目标 / 固定冷热场景
预热时间：
```

### 7.2 结果记录

```text
IRSTAT：
AISTAT：
NetX STAT：
采集帧数：
CRC 错误：
DMA/HAL 错误：
Jump 平均/P95/最大：
Max delta：
Repaired 平均/P95/最大：
平均/最大采集耗时：
固定 CRC 次数：
肉眼异常：
示波器截图路径：
原始帧目录：
结论：通过 / 不通过 / 需要复测
备注：
```

## 8. 当前推荐执行顺序

1. 执行阶段 A，验证当前 bounce-buffer 实验版。
2. 执行阶段 B，获得 25 MHz 下严格的 AD7606 运行/暂停对照。
3. 执行阶段 C，重新评估 50 MHz，不能只看 `ok=100`。
4. 如果 50 MHz 仍有差异，同时执行阶段 E 和阶段 F。
5. 在链路稳定后实施阶段 G 和阶段 H。
6. 最后执行 25 fps 和长期稳定性验收。

在阶段 A 至 F 得到明确结论前，正式运行配置保持 25 MHz、图像 10 fps、温度 5 fps。

## 9. 2026-07-17 NetX 对齐与压力测试结论

### 9.1 NetX 32-byte 对齐实验

实验固件已经确认 NetX packet pool 和首个 packet data 指针均按 32 字节对齐：

```text
pool_addr=0x341C4740 pool_off=0x00000000
data_addr=0x341C4780 data_off=0x00000000
pool_total=0x00000028 pool_free=0x00000020 pool_empty=0
```

在该配置下连续执行 9 轮 `--mode both --no-fill`，汇总结果为：

| 指标 | 结果 |
| --- | ---: |
| TCP board rate 平均值 | 约 179.65 Mbps |
| UDP board rate 平均值 | 约 225.70 Mbps |
| UDP 丢包 | 0 |

对照提交 `570bee5` 的典型结果约为 TCP 219--220 Mbps、UDP 279 Mbps。即使地址已经正确对齐，吞吐仍保持在约 180/226 Mbps，因此可以排除“NetX packet pool 未按 cache line 对齐”是本轮吞吐回退的主因。临时的 NetX 对齐改动已经从源码中撤除。

### 9.2 AI 压力故障

第一组网络压力测试后观测到：

```text
run_err=1 err_ms=5009 rt=1 max_ms=1501
```

后续两组测试中 `runs`、`last_seq` 和输出值完全不再变化，说明一次约 5 秒的 NPU runtime 超时后，AI worker 没有继续推理。该问题与网络吞吐回退分开处理：先定位 cache/buffer 改动的影响，再补充 AI runtime 超时后的受控恢复和故障升级机制。

## 10. Cache 路径单变量固件

为避免多个改动同时存在，新增两个编译期开关：

```c
AD7606_SPI4_DMA_CACHE_MAINTENANCE
TINY1C_STM32_DMA_CACHE_MAINTENANCE
```

源码默认均为 `0`，主构建保持基线行为。已经生成以下三个诊断固件。普通 `.bin` 仅是签名前的 payload，外部 Flash 启动测试必须烧录表中的 `-trusted.bin`：

| 可烧录固件 | AD7606 cache | Tiny1C bounce/cache | SHA-256 |
| --- | ---: | ---: | --- |
| `fsbl_appli_all_Appli_cache00_control-trusted.bin` | 0 | 0 | `7096CC5F83BB2CCCB0E0C95B417C978BE6082B52494BF92FA41753EA824F6886` |
| `fsbl_appli_all_Appli_cache10_ad_only-trusted.bin` | 1 | 0 | `7316AA6A4C7D94F502228A8B0B195DCB3FF439B4699F1DE314C86C1814295436` |
| `fsbl_appli_all_Appli_cache01_tiny_only-trusted.bin` | 0 | 1 | `F3930317F5A7D80A08A65A2229E4A1E509A9BC86004B4A4234FFE0B3DB7ABF66` |

`cache00` 的原始 payload 与独立工作树中的 `570bee5` 固件逐字节一致，可作为确定的基线。主构建输出当前也是 `cache00`。

三个 trusted 镜像均使用以下参数生成：

```text
-hv 2.3 -align -nk -of 0x80000000 -t fsbl
```

签名工具解析出的入口地址分别为 `0x34022045`、`0x340220D5` 和 `0x3402213D`，与对应原始 bin 向量表中的 Reset Handler 完全一致。

### 10.1 推荐测试顺序

1. 烧录 `cache10_ad_only`，执行 3 轮吞吐测试，并在测试前后读取 `AISTAT`、`IRSTAT`。
2. 烧录 `cache01_tiny_only`，重复完全相同的测试。
3. 只有需要复核烧录流程时才再次烧录 `cache00`，因为它已与 `570bee5` 逐字节一致。

每个固件使用同一组命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 -Command AISTAT

1..3 | ForEach-Object {
    python .\tools\net_throughput_test.py --mode both --no-fill
    Start-Sleep -Seconds 2
}

Start-Sleep -Seconds 10

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 -Command IRSTAT

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\tcp_ir.ps1 -Command AISTAT
```

### 10.2 结果判读

| 现象 | 初步结论 |
| --- | --- |
| `cache10` 回退到约 180/226，`cache01` 保持约 220/279 | AD7606 cache 维护或其内存布局变化是主要触发项 |
| `cache10` 保持约 220/279，`cache01` 回退到约 180/226 | Tiny1C bounce/cache 路径是主要触发项 |
| 两个单变量版本均保持基线，仅双开启版本回退 | 两条路径叠加后的 cache 压力、BSS 布局或总线竞争导致回退 |
| 两个单变量版本均回退 | 两组改动分别都能影响关键内存布局或运行路径，需要继续细分 clean/invalidate、alignment 和 memcpy |

每轮必须同时检查 `AISTAT` 的 `runs` 是否继续增长。出现 `run_err` 后若 `runs/last_seq` 连续 10 秒不变，应记录为 AI runtime 卡死，不能只依据 `fault=0` 判断 AI 正常。

### 10.3 无启动输出事件记录

第一次烧录 `cache10` 时误将未签名的 `fsbl_appli_all_Appli_cache10_ad_only.bin` 直接写入 `0x70100000`。板卡随后没有 LED、串口或网络输出。该现象发生在应用 `main()` 之前，原因是 FSBL 需要 STM32 header v2.3 trusted 镜像，不能据此判断 AD7606 cache 代码死机。

该次结果作废。后续只使用文件名以 `-trusted.bin` 结尾的诊断镜像重新测试。

### 10.4 AD-only 与 Tiny-only 实测结果

使用 trusted 诊断镜像分别执行 3 轮 `--mode both --no-fill`：

| 版本 | TCP board 平均 | UDP board 平均 | IR image/temp | AI |
| --- | ---: | ---: | --- | --- |
| AD7606 cache only | 205.54 Mbps | 254.82 Mbps | 9.79 / 4.89 fps，`err=0` | `run_err=0`，持续运行 |
| Tiny1C bounce/cache only | 225.58 Mbps | 281.11 Mbps | 5.96 / 3.29 fps，`err=0` | `run_err=1`，`err_ms=5008` |
| 两条路径同时开启的先前实验 | 约 179.65 Mbps | 约 225.70 Mbps | 受压下降 | 曾发生超时后停止增长 |

结论：

1. Tiny1C bounce/cache 单独开启时 Ethernet 保持基线，不是网络吞吐回退的直接原因。
2. AD7606 cache 单独开启时，TCP 相对 Tiny-only 基线下降约 8.9%，UDP 下降约 9.4%。
3. 两条路径同时开启存在额外叠加损失，不能仅用静态 BSS 对齐解释。
4. Tiny1C bounce/cache 会明显影响红外采集调度，并可能触发 NPU runtime 长尾或超时，需要与网络问题分开优化。

代码审查发现，旧 AD cache 实验在 header DMA 完成中断中对剩余约 16 KiB payload 执行 `CleanInvalidateDCache`。该操作会延长高优先级 DMA ISR，阻塞 Ethernet 和其他中断，是当前最强的吞吐回退候选原因。

### 10.5 AD cache 细分诊断镜像

新增独立的 `AD7606_SPI4_DMA_BUFFER_ALIGNMENT` 开关，并生成两个 trusted 镜像：

| 固件 | 对齐 | Cache 维护 | Cache prepare 上下文 | SHA-256 |
| --- | ---: | ---: | --- | --- |
| `fsbl_appli_all_Appli_cache_align_only-trusted.bin` | 1 | 0 | 无 | `F7D2DEE6F5EEF312B7B686DF1B9E7B8D39344EFCDA15DB9DB3326E903273229F` |
| `fsbl_appli_all_Appli_cache_ad_thread_prepare-trusted.bin` | 1 | 1 | 整帧 prepare 移到线程；ISR 仅处理 header cache line | `C89757FC5E3A6D6EA4A5763A4681854DF2E15F57BCFF88009FF47BC5E4FFBD9D` |

推荐先测试 `cache_ad_thread_prepare`。如果吞吐恢复到约 225/281 Mbps，说明 ISR 内的大块 cache 维护是主要根因；如果仍为约 205/255 Mbps，再测试 `cache_align_only`，区分 cache 维护总开销与内存布局变化。

## 11. 25 fps 温度链路实验版本

在 25 MHz、图像 10 fps、温度 5 fps 配置下，固定场景的图像与温度 A/B 对照已经表明：AD7606 运行和暂停时均可获得稳定数据，CRC 正确，温度大跳变和修复点均为 0，图像噪点指标也没有出现与 AD7606 状态相关的显著差异。因此下一阶段不再继续停留在低速链路，而是验证 50 MHz 下的持续温度采集能力。

Tiny1-C 说明书将探测器帧频限定为不高于 25 Hz。当前 STM32 VoSPI 驱动使用 `0xAA` 和 `0xCC` 分别触发一帧图像和一帧温度读取，所以“图像 25 fps + 温度 5 fps”会产生每秒 30 次完整帧传输，不作为正式实验配置。厂家 USB SDK 的 `256x384` 联合模式能够在一个 UVC 原始帧中同时携带图像和温度，但该路径与当前 VoSPI 分流命令不同，后续需要单独验证协议和帧长度，不能直接套用。

本阶段改为后台只连续读取 `0xCC` 温度帧 25 fps。图像后台采集关闭，仍可通过 `IRGETIMG` 暂停后台后按需抓取一帧。这样既满足连续温度数据需求，也保留图像诊断能力。

### 11.1 实验配置

| 项目 | 配置 |
| --- | ---: |
| Tiny1C 模块输出模式 | 25 fps |
| SPI3 kernel clock | 200 MHz |
| SPI3 prescaler | 4 |
| SPI3 SCK | 50 MHz |
| SPI3 DMA | HPDMA1 CH0 RX / CH1 TX |
| HPDMA 通道权重 | High |
| HPDMA IRQ 优先级 | 8 |
| 图像后台采集目标 | 0 fps，按需抓取 |
| 温度采集目标 | 25 fps |
| Tiny1C DMA cache 路径 | 开启对齐 scratch/bounce buffer |
| AD7606 DMA cache 路径 | 保持关闭，不引入额外变量 |

单帧为 98,304 bytes。温度 25 fps 的有效载荷数据率约为 19.66 Mbps，占 50 MHz 时钟的 39.3%。此前 50 MHz HPDMA 实测单帧耗时约 25--32 ms，因此总采集时间预算约为 625--800 ms/s，较 30 次完整帧读取保留了更合理的调度余量。

### 11.2 固件

```text
Makefile/Appli/build/fsbl_appli_all_Appli_tinytemp25fps_rxhigh_50mhz-trusted.bin
SHA-256: E13C0F2FCC577AC06E756727F6457FAC7EB9EDFE2557888CEBC8CE55E045425E
```

该镜像使用 header v2.3、`-align -nk -of 0x80000000 -t fsbl` 生成，签名工具解析出的入口地址为 `0x340222CD`。必须烧录 `-trusted.bin`，不能直接烧录未签名的 payload。

### 11.3 首轮板端结果与修正

首轮温度单流固件的后台线程优先级为 15，断电启动后的两次 60 s 测试均稳定在约 14.7 fps，`temp_ms` 为 54--57 ms。其配置、采集错误和截止期计数均正常，说明不是 SPI 传输失败，而是后台线程被优先级 14 的 AD7606 线程持续抢占。此前 `IRSPI50DMA` 在优先级 13 的 TCP 命令线程中执行时，完整温度帧约为 30--32 ms，证明同一 SPI/HPDMA 路径具备达到 25 fps 的时间预算。

因此将红外后台线程优先级由 15 提升到 13。线程在每个 4096-byte DMA 块上等待信号量，NetX 核心、UDP echo 和链路线程仍保持更高优先级，AD7606 线程也可在 HPDMA 等待间隙继续运行。

首轮脚本还曾在 `ADSTAT latest=0` 的瞬时发布窗口读到 `seq=0`，使无符号序列差值下溢。脚本现会重试至 `latest=1`，并使用温度平均采集时延验收本轮性能，不再用包含历史瞬时抖动的全局 `max_ms` 直接判失败。

优先级 13 固件（SHA-256 `736D05F0706737B314E4307B1DA9BEE722410D4C5C7359E439CFD8CEB784CDD1`）断电测试后，两段 60 s 测量的板端实际时间分别为 61.041 s 和 61.001 s，温度计数增量分别为 1525 和 1524，即两次实际帧率均约为 24.98 fps。`temp_ms=37`、`max_ms=39--40`、AD7606 序号和 AI 推理持续增长，说明线程优先级修正有效。

两段测量中 `capture_error_count` 均增加 1，表现为约每 60 s 一次的低频失败。为避免掩盖问题，暂不放宽错误判据，也不立即加入自动重试。诊断固件在 `IRSTAT` 中增加 `dma_start`、`dma_wait`、`dma_irq`、`blk`、`reason`、`hal`、`spierr`、`err_at` 和 `err_len`，用于区分 DMA 启动失败、等待超时、DMA IRQ 错误和阻塞回退错误。脚本帧率也改为使用两次板端 `elapsed_ms` 的差值计算，不再用主机固定睡眠时长估算。

诊断固件进行 120.566 s 测试后得到 24.98 fps，帧级错误增加 2；同期 `dma_irq` 从 6 增加到 10，而 `dma_start`、`dma_wait` 和 `blk` 均保持 0。`reason=5`、`hal=1`、`spierr=0x00000004` 在 STM32N6 HAL 中对应 `HAL_SPI_ERROR_OVR`。一次 OVR 会异步中止 RX 和 TX 两条 DMA 通道，因此 `dma_irq` 增量是帧级错误增量的两倍。

为优先排空 SPI3 RX FIFO，将 HPDMA1 Channel 0（SPI3_RX）从 `DMA_LOW_PRIORITY_HIGH_WEIGHT` 提升为 `DMA_HIGH_PRIORITY`，Channel 1（SPI3_TX）保持 High weight。该改动同时写入 `spi.c` 和 `.ioc`，其余 SPI、线程、AD7606、AI 与网络配置不变。

### 11.4 第二轮自动验收

烧录并完全断电重启后，串口应显示：

```text
Tiny1C SPI3 SCK Hz: 0x02FAF080
IR capture thread start target=image0fps,temp25fps spi=50000000Hz
IR capture ready image=0fps temp=25fps spi=50000000Hz
```

执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\ir_25fps_stability_test.ps1 `
  -DurationSeconds 60
```

脚本同时检查配置、温度实际帧率、采集错误、调度超期、平均采集耗时、AD7606 序号增长和 AI 推理增长。第二轮判据为温度不低于 23 fps、后台图像计数不增长、温度平均采集耗时不超过 40 ms、采集错误增量为 0、AI `run_err` 增量为 0。

### 11.5 兼容性验收

第一轮帧率测试通过后，在固定场景下分别执行温度和图像 A/B 测试：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\ir_ad_contention_test.ps1 -Iterations 30

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\ir_ad_image_contention_test.ps1 -Iterations 30
```

验收不能只看 CRC。还需满足：

1. AD7606 运行和暂停两组都没有持续的温度大跳变或修复点增长。
2. 图像 `Dark/Bright/TotalJumps` 分布与 25 MHz 的正常固定场景基线同量级，不能出现数量级增长。
3. `ADGET` CRC 正确，`AISTAT runs` 持续增长，`run_err` 不增加。
4. 连续运行至少 30 分钟后再次执行自动验收和 A/B 抽检。

如果温度达不到 23 fps，但单帧 `temp_ms` 小于 35 ms 且 `err=0`，优先检查模块是否真的以 25 Hz 输出以及主机是否重复读取同一帧；如果单帧经常超过 40 ms，则应先优化 SPI 分块、cache 维护和总线仲裁，不应继续提高线程优先级。若 50 MHz 再次出现温度跳变，则立即保留原始帧，并按 AD7606 运行/暂停结果区分总线竞争与 Tiny1C 自身链路裕量。

### 11.6 HPDMA RX-only 300 秒结果与双通道高优先级实验

RX CH0 为 Very High、TX CH1 为 High weight 的固件连续运行 300.206 s，温度帧增加 6475 帧，即 21.57 fps；`temp_ms=43`、`max_ms=45`。采集错误、DMA IRQ 错误、SPI OVR、AD7606 错误和 AI 运行错误均未增加。该结果证明 RX 优先排空已经消除了 OVR，但 RX/TX 仲裁不对称使 TX FIFO 供数出现间歇停顿，完整 98,304-byte 帧超过了 40 ms 周期。

下一轮只将 HPDMA1 CH1（SPI3_TX）从 High weight 提升为 Very High，使 RX/TX 保持同一仲裁等级；CH0、SPI3 50 MHz、4096-byte 分块、线程优先级、AD7606、AI 和网络配置均不改变。

```text
Makefile/Appli/build/fsbl_appli_all_Appli_tinytemp25fps_rxtxhigh_50mhz-trusted.bin
SHA-256: E08BE3F4A938645325C94F1AEADAB36B9B1637154DE6EDA198DDEC3F921F16CA
Entry point: 0x340222DD
```

先执行 60 s 快速筛选。合格条件为温度帧率不低于 23 fps、平均采集耗时不超过 40 ms、`capture_err_delta=0`，并且 `dma_irq` 和 `spierr` 不增长。通过后再执行 300 s 稳定性测试；若恢复 25 fps 但 OVR 重现，则回到 RX-only 版本并进入 SPI 分块/FIFO 优化，不再通过 DMA 优先级反复折中。

双通道 Very High 固件的首轮 61.157 s 测量通过：温度增加 1529 帧，即 25.00 fps；平均采集耗时 37 ms，区间最大值 40 ms；`capture_err_delta=0`、`late_delta=0`。AD7606 序号增加 2369，AI 推理增加 970，`run_err` 不增长。测试起点和终点均显示累计 `err=3`、`dma_irq=6`、`spierr=0x00000004`，说明这些 OVR 发生在测量窗口之前，而不是本轮 61 s 期间。下一步必须完整断电重启后执行 300 s 测试，以覆盖 Tiny1C 启动、预热和长期并行运行阶段。

冷启动验收使用严格基线参数。该参数要求 10 s 预热结束时累计 `err`、`dma_irq` 和 `spierr` 仍全部为 0，同时继续检查后续测量区间内的错误增量：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\ir_25fps_stability_test.ps1 `
  -DurationSeconds 300 `
  -RequireCleanBaseline
```

双通道 Very High 固件随后完成三轮 300 s 测量。三轮温度帧率均为 24.99 fps，平均采集耗时均为 37 ms，最大值为 38--39 ms；AD7606 和 AI 始终连续运行。测量窗口内 OVR 分别增加 3、3、2 次，对应 `dma_irq` 分别增加 6、6、4。第三轮没有重新上电，而是延续第二轮的累计计数。结果说明帧率和系统调度是稳定的，变化的只是低频 OVR 的发生时刻；当前故障率约为每 100--150 s 一次，不能继续视为随机的整机状态变化。

HAL 对 `MasterReceiverAutoSusp` 的定义明确用于在 SPI 主机接收模式下自动管理连续传输并避免 overrun。下一轮保持 50 MHz、4096-byte 分块和 RX/TX 双 Very High 不变，仅把 SPI3 `MasterReceiverAutoSusp` 从 Disable 改为 Enable。`IRSTAT` 增加 `masrx` 字段，自动测试要求 `masrx=1`。

```text
Makefile/Appli/build/fsbl_appli_all_Appli_tinytemp25fps_autosusp_50mhz-trusted.bin
SHA-256: 0161B178C6BF567FCB26BB00364D643BDE75BF44835FE88B250DCC9F504D931F
Entry point: 0x34022305
```

### 11.7 Master receiver auto-suspend 连续测试

auto-suspend 固件完成两轮不间断的 60 s 测试。两轮均保持约 24.55 fps，AD7606 序号和 AI 推理次数持续增长，`dma_irq=0`、`spierr=0`，说明 `MASRX` 已消除此前约每 97 s 出现一次的 SPI OVR。

但两轮测量窗口内均新增 1 次 `dma_wait`：`reason=4`、`hal=3`，对应 DMA 完成信号等待超时和 `HAL_TIMEOUT`。单次超时约 1000 ms，使全局 `max_ms` 达到 1038 ms。第一次发生在 536-byte 尾块，第二次发生在普通 4096-byte 块，因此不能将问题归结为尾包长度。当前结论是 auto-suspend 改变了故障类型，但尚未形成可接受的最终方案。

下一版不改变采集调度、SPI 时钟、HPDMA 优先级或 CubeMX 配置，只在 `HAL_SPI_Abort()` 前保存 SPI3 `SR/CR1/CR2/CFG1`、HAL SPI 状态、RX/TX DMA 剩余字节和 DMA 状态，并通过 `IRSTAT` 输出。稳定性脚本同时单列 `DmaWaitTimeouts` 检查。

```text
Makefile/Appli/build/fsbl_appli_all_Appli_tinytemp25fps_autosusp_diag_50mhz-trusted.bin
SHA-256: 5FB7CD1675AEDF282572715DBE19E66301A6FB1B7F6D92382C466921877543D0
Entry point: 0x34022325
```

第一版扩展诊断镜像运行 180.504 s，未出现 OVR、DMA callback 错误或 DMA wait timeout，但温度帧率只有 20.70 fps，单帧平均耗时 44 ms。检查链接布局发现，直接扩大诊断结构将 Tiny1C 的 frame/RX/TX DMA 缓冲区整体移动了 32 bytes，改变了 AXI SRAM bank 映射；因此该轮只能证明 auto-suspend 可以无错误降速运行，不能作为原布局下的性能结论。

诊断存储随后被拆成原尺寸计数结构和独立超时快照。DMA 缓冲区恢复到 `frame=0x341AA140`、`rx=0x341C2140`、`tx=0x341C3140`，超时快照位于 `0x341C4140`，不会再扰动三块 DMA 缓冲区。新版 `IRSTAT` 输出 `diag=2`，脚本拒绝旧诊断布局。

```text
Makefile/Appli/build/fsbl_appli_all_Appli_tinytemp25fps_autosusp_diag2_50mhz-trusted.bin
SHA-256: 716553D95989E412E198A0BFEB0E83AE3276B3FA2C1234A97DE392DB576BD1D3
Entry point: 0x3402238D
```

### 11.8 正式目标调整为温度 20 fps

系统不再强求 Tiny1C 温度流达到 25 fps。正式后台目标调整为 20 fps，图像仍为按需抓取。SPI3 保持 50 MHz、RX/TX HPDMA 保持 Very High、master receiver auto-suspend 保持开启。50 ms 调度周期相较 25 fps 的 40 ms 周期留出更多 AD7606、NPU 和总线仲裁余量，验收下限调整为 19 fps，平均单帧耗时上限调整为 48 ms；采集错误、DMA wait timeout、SPI OVR 和 AI 运行错误仍要求零增量。

```text
Makefile/Appli/build/fsbl_appli_all_Appli_tinytemp20fps_autosusp_diag2_50mhz-trusted.bin
SHA-256: B435B3D81A130F72C7F07364EFC16089EF23DCC6ED602060875633828EA68846
Entry point: 0x3402238D
```

验收使用 `tools/ir_20fps_stability_test.ps1`。该入口默认运行 180 s，并调用通用稳定性脚本检查 `target=0/20`、温度帧率不低于 19 fps、平均采集耗时不超过 48 ms以及全部错误计数零增量。

### 11.9 Auto-suspend 停滞现场与 EOT IRQ 恢复

20 fps、diag2 固件运行 182.026 s 时新增 2 次 DMA wait timeout。最后一次现场为 `SR=0x0E720800`、`CR1=0x00001301`、`CR2=0x00001000`、`rxrem=3698`、`txrem=3666`、`spist=5`、`rxst=2`、`txst=2`。解码结果如下：

- `SR.SUSP=1`，`CR1.CSTART=1`，说明硬件自动挂起而传输仍处于启动状态。
- `SR.CTSIZE=3698` 与 RX DMA 剩余量一致，RX 已取走全部线上接收数据。
- TX DMA 比 RX 多推进 32 bytes，正好对应预装入 TX FIFO 的数据。
- SPI 为 `HAL_SPI_STATE_BUSY_TX_RX`，两条 HPDMA 均为 `HAL_DMA_STATE_BUSY`，没有 OVR 或 DMA 硬件错误。

HAL 的阻塞收发路径检测到该状态后会清除 `SUSP`。DMA 路径原先只在 RX DMA 完成后打开 EOT 中断，因此中途 auto-suspend 没有进入 `HAL_SPI_IRQHandler()`，最终等待 1000 ms 超时。修复是在 `HAL_SPI_TransmitReceive_DMA()` 成功后、等待完成信号量前立即开启 `SPI_IT_EOT`；HAL ISR 随后可清除每次硬件 SUSP 并继续 CSTART。`IRSTAT` 增加 `susp` 计数，`diag=3` 标识该修复版本。

在烧录 auto-suspend 固件前，旧双 Very High 固件又继续运行了两个区间。累计 `err` 从 5 增至 14、再增至 20；对应运行时间分别增加约 786.974 s 和 673.865 s，即分别新增 9 次和 6 次 OVR。合计 1460.839 s 新增 15 次，平均约每 97.4 s 一次，与前三轮 300 s 测量的低频 OVR 结论一致。这两次命令使用了已经要求 `masrx` 字段的新版脚本，但旧固件不输出该字段，因此脚本在起始状态解析阶段退出，没有形成新的 300 s 测量窗口。脚本现会对这种情况明确提示需要烧录 auto-suspend 固件。

EOT IRQ 恢复版已完成编译和签名。新增诊断没有改变已验证的 DMA 缓冲区布局：`frame=0x341AA140`、`rx=0x341C2140`、`tx=0x341C3140`，超时快照位于 `0x341C4140`。`susp` 表示 HAL 中断已清除硬件自动挂起的累计次数；测试窗口内该值允许为零，但当它增加且 `err`、`dma_wait`、`dma_irq`、`spierr` 均无增量时，可直接证明恢复路径已被触发并成功继续传输。

```text
Makefile/Appli/build/fsbl_appli_all_Appli_tinytemp20fps_autosusp_irqresume_diag3_50mhz-trusted.bin
SHA-256: 999071FCF22C00796C7D75BB20EC74290C94805C06AC5B2EBAD7A07B32F47591
Entry point: 0x340223CD
Flash address: 0x70100000
```

diag3 固件完成首轮严格 300 s 验收。测量区间为 300.537 s，温度帧增加 6011 帧，实测 `20.00 fps`；平均采集耗时 43 ms、全局最大值 44 ms，`capture_err_delta=0`、`late_delta=0`、`dma_wait_delta=0`，SPI/DMA 错误保持全零。同期 AD7606 序号增加 13194，AI 推理增加 5262 次且 `run_err_delta=0`，三条链路持续并行运行。全部验收项通过。

本轮 `SUSP resume delta=0`，说明 300 s 内没有产生硬件自动挂起，因此尚未实际覆盖 EOT IRQ 恢复分支；不过与此前 182.026 s 内出现 2 次 DMA wait timeout 的结果相比，当前配置已经取得首个完整的五分钟零错误样本。后续应继续进行至少 30 min 的耐久测试，并以 `susp` 是否增长来区分“始终未挂起”和“发生挂起后成功恢复”。
