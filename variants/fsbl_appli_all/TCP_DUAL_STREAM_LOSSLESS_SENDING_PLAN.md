# STM32N6 双数据流 TCP 无损发送方案与可行性复核

适用工程：

```text
E:\WORK\STM32N6\N6CODE\STM32N6_AI\variants\fsbl_appli_all
```

复核日期：2026-07-19

---

## 1. 复核结论

经过对当前工程、NetX Duo TCP 发送实现、Ethernet 驱动和链接内存占用的复核，结论如下。

### 1.1 总体方向可行

以下方向是可行的：

1. 采集与网络发送解耦；
2. AD/AI 和红外使用独立数据缓存；
3. TCP 发送使用有限等待或非阻塞方式；
4. TCP 队列满时由事件通知发送线程继续工作；
5. 每次发送尽量填满一个 MSS；
6. 通过发送配额避免一条流长期挤占另一条流；
7. 无损数据使用应用层环形缓存承受 TCP 反压。

### 1.2 原方案中需要纠正的内容

原方案中的以下内容不应直接实施：

1. Windows 接收端设置 `TCP_NODELAY` 不能直接关闭 Windows 自己的 Delayed ACK；
2. 当前 NetX Duo TCP 发送源码没有发现 Nagle 聚合逻辑，不能直接认定是 Nagle/Delayed ACK 互锁；
3. 将完整大消息构造成 packet chain 并只调用一次 `nx_tcp_socket_send()`，不会减少最终 TCP segment 数量或发送队列占用；
4. 将 `NX_TCP_MAXIMUM_TX_QUEUE` 直接提升到 64/128，并为每条流配置 96 个 packet，当前内部 SRAM 无法承受；
5. 统一为一个发送线程并不是第一阶段最稳妥的做法，因为一个 socket 阻塞仍可能拖住另一条流；
6. 在没有 Wireshark 和 NetX 状态证据前，不能只根据“约 200 ms 停顿”认定 Windows Delayed ACK 是唯一根因。

### 1.3 当前工作树与问题描述不完全一致

当前工程中的：

```text
Appli/NetXDuo/App/app_stream_telemetry.c
Appli/NetXDuo/App/app_ir_stream.c
```

目前实际使用的是 UDP 广播，而不是 TCP：

```c
NX_UDP_SOCKET
nx_udp_socket_send()
```

因此本文中的 TCP 改造方案适用于：

- 后续将 5100/5101 改回 TCP；
- 或用户当前正在测试但尚未同步到此工作树的 TCP 版本。

实施前必须先确认最终需要修改的是哪一版 TCP 代码。

---

## 2. 对“20 个 packet、每消息 7 个包、200 ms 停顿”的判断

已知条件：

```text
NX_TCP_MAXIMUM_TX_QUEUE = 20
每条消息约 7 个 TCP segment
nx_tcp_socket_send() 最长等待约 200 ms
Windows 端可能使用 Delayed ACK
```

发送队列的占用可以表示为：

```text
消息 1：7 个 segment
消息 2：7 个 segment
消息 3：继续加入时达到 20 个 segment
```

这确实会使调用线程在 TCP 发送队列满时等待 ACK。

但“20 和 7 完美撞上 Windows 200 ms”仍需要抓包确认，原因是：

1. 连续发送满 MSS segment 时，接收方通常会持续产生 ACK；
2. 20 是偶数，如果接收方每两个 segment 确认一次，理论上不应固定留下一个孤立 segment；
3. 200 ms 也可能只是设备端设置的发送等待超时；
4. PC 接收线程停止读取、接收窗口缩小、packet pool 耗尽或 Ethernet TX descriptor 紧张也可能产生相似现象。

因此第一步不是直接扩大所有队列，而是确认：

```text
阻塞返回状态是什么？
阻塞时 TCP TX queue depth 是多少？
阻塞时 TCP advertised window 是多少？
Windows ACK 实际间隔是多少？
PC 接收线程是否持续 recv()？
```

---

## 3. 当前 NetX TCP 行为复核

### 3.1 TCP 发送队列确实按 segment 计数

NetX 在：

```text
Middlewares/ST/netxduo/common/src/nx_tcp_socket_send_internal.c
```

中检查：

```c
socket_ptr->nx_tcp_socket_transmit_sent_count
    <
socket_ptr->nx_tcp_socket_transmit_queue_maximum
```

每个发送并进入未确认队列的 TCP segment 都会增加：

```c
nx_tcp_socket_transmit_sent_count
```

当队列达到上限时：

- 有等待时间：调用线程挂起；
- 无等待时间：返回 `NX_TX_QUEUE_DEPTH`；
- 对端窗口为零或不足：可能返回 `NX_WINDOW_OVERFLOW`。

### 3.2 大 packet chain 仍会被分段

当传入数据大于 MSS 时，NetX 会在内部：

1. 按 MSS 分段；
2. 从源 packet pool 分配新的 segment；
3. 将数据复制到新的 packet；
4. 将每个 segment 放入 TCP 未确认队列。

因此：

```text
一次 nx_tcp_socket_send(8 KB)
```

最终仍然可能生成约 6 个 TCP segment，并占用约 6 个 TCP 队列项。

它只能减少应用函数调用次数，不能解决队列深度问题，还可能增加：

- 临时 packet 需求；
- 数据复制；
- 大调用中途等待和部分发送处理的复杂度。

### 3.3 NetX 当前发送路径会设置 PSH

当前 NetX TCP 发送代码为数据 segment 设置：

```c
NX_TCP_ACK_BIT | NX_TCP_PSH_BIT
```

工程源码中没有发现独立的 Nagle 开关或 Nagle 聚合状态。

所以本工程不应把解决重点放在“关闭设备端 Nagle”上。

---

## 4. Windows 上位机应做的事情

### 4.1 TCP_NODELAY 的正确定位

Windows socket 上设置 `TCP_NODELAY` 只影响该 socket 的主动发送行为。

如果 PC 主要负责接收 STM32 数据，设置：

```text
PC socket TCP_NODELAY = true
```

不会直接关闭 Windows 对 STM32 数据的 Delayed ACK。

可以保留该设置以优化：

- PC 向 STM32 发送控制命令；
- 双向请求/响应；
- 心跳和确认消息。

但它不能作为解决 STM32 到 PC 方向 200 ms 停顿的第一措施。

### 4.2 PC 接收线程必须持续读取

Windows 端应该采用：

```text
TCP recv thread
    ↓
有界接收队列
    ├── 协议解析
    ├── CRC
    ├── 文件记录
    └── UI/图像处理
```

TCP 接收线程中禁止直接执行：

- 图像转换；
- 绘图；
- 阻塞磁盘写入；
- 等待另一条数据流；
- 长时间数据处理。

否则 PC 接收窗口可能缩小，最终把反压传递到 STM32。

### 4.3 Delayed ACK 修改只用于诊断

可以临时调整 Windows ACK 行为，用于确认 200 ms 停顿是否确实由 Delayed ACK 触发。

但产品方案不应依赖：

- 修改 Windows 注册表；
- 指定网卡驱动行为；
- 要求用户关闭系统级 Delayed ACK。

嵌入式发送端应该能够在接收方采用正常 TCP ACK 策略时稳定工作。

---

## 5. 推荐的第一阶段方案

第一阶段不要立即引入统一发送线程，也不要将整个消息一次交给 NetX。

推荐保留两个独立发送线程：

```text
AD/AI TCP sender
IR TCP sender
```

每个线程：

1. 只操作自己的 socket；
2. 从自己的应用层数据队列读取数据；
3. 每次构造一个接近 MSS 的 `NX_PACKET`；
4. 使用 `NX_NO_WAIT` 或很短的等待时间发送；
5. 队列满时等待 NetX queue-depth 回调唤醒；
6. 每次最多连续发送有限数量的 segment。

### 5.1 为什么保留两个发送线程

相比统一发送线程，两个线程的优势是：

- 一个 socket 出现零窗口时，不会直接阻塞另一个 socket；
- 断线重连状态相互独立；
- 修改现有代码较少；
- 更容易分别统计和定位问题。

公平性通过以下方式保证：

```text
两个发送线程使用相同优先级
每次最多连续发送 4 个 segment
达到 burst 上限后主动让出 CPU
```

示例：

```c
#define APP_STREAM_TX_BURST_SEGMENTS 4U
```

发送 4 个 segment 后：

```c
tx_thread_relinquish();
```

或者重新等待事件。

---

## 6. 使用 NetX 队列深度通知替代 200 ms 阻塞

当前 `nx_user.h` 已启用：

```c
#define NX_ENABLE_TCP_QUEUE_DEPTH_UPDATE_NOTIFY
```

可以为每个 stream socket 注册：

```c
nx_tcp_socket_queue_depth_notify_set(
    socket,
    App_TcpQueueDepthNotify);
```

回调中只做事件通知：

```c
static VOID App_TcpQueueDepthNotify(NX_TCP_SOCKET *socket)
{
    tx_event_flags_set(
        &AppTxEvents,
        APP_TX_EVENT_QUEUE_AVAILABLE,
        TX_OR);
}
```

回调中禁止：

- 分配大内存；
- 发送下一个 packet；
- 打印大量日志；
- 进行阻塞等待。

发送线程的推荐流程：

```c
for (;;)
{
    if (没有待发送数据)
    {
        等待“新数据”事件;
    }

    burst = 0;

    while ((存在待发送数据) &&
           (burst < APP_STREAM_TX_BURST_SEGMENTS))
    {
        构造一个 MSS 大小的 NX_PACKET;

        status = nx_tcp_socket_send(
            socket,
            &packet,
            NX_NO_WAIT);

        if (status == NX_SUCCESS)
        {
            提交应用层消费进度;
            burst++;
        }
        else if ((status == NX_TX_QUEUE_DEPTH) ||
                 (status == NX_WINDOW_OVERFLOW))
        {
            保留未发送数据;
            释放或复用尚未提交的 packet;
            等待 queue-depth/window 事件;
            break;
        }
        else
        {
            进入断线或错误恢复;
            break;
        }
    }

    tx_thread_relinquish();
}
```

这样不会让发送线程每次固定阻塞 200 ms，也不会使一个 socket 的等待直接拖住另一条流。

---

## 7. 推荐的数据分段方式

### 7.1 每次发送一个 MSS 左右的 packet

推荐每个 `NX_PACKET` 直接包含一个 TCP segment 的应用数据：

```text
第一个 segment：
    64-byte MMS2 header
    + 第一段 payload

后续 segment：
    纯 payload
```

推荐应用数据长度根据 socket MSS 动态获取：

```c
ULONG peer_mss;
nx_tcp_socket_mss_get(socket, &peer_mss);
```

每段应用数据长度不要使用硬编码 1400 作为最终值，应预留 NetX 所需头部空间并结合 packet pool payload size 计算。

### 7.2 不建议完整消息 packet chain 一次发送

不推荐：

```text
构造 8 KB/98 KB packet chain
一次调用 nx_tcp_socket_send()
```

原因：

- NetX 仍会按 MSS 分段；
- 仍然占用多个 TCP TX queue 项；
- 可能产生额外 packet 分配和复制；
- 队列中途满时，处理“已发送部分”和“未发送部分”更复杂；
- 一个大调用可能长时间持有发送线程。

---

## 8. TCP 队列和 packet pool 的可行配置

### 8.1 当前内部 SRAM 余量

根据当前链接结果：

```text
RAM 起点：0x34080000
RAM 终点：0x34200000
链接及保留区结束：约 0x341F2098
剩余空间：约 57 KB
```

当前两个 stream packet pool 均为 4 个 packet，每个 pool 大约占 6384 字节。

如果两个 pool 都从 4 增加到 16：

```text
两个 16-packet pool 总计：约 51 KB
相比当前增加：约 38 KB
预计仍剩余：约 18 KB
```

该配置内存上可行，但余量已经不大，必须重新编译并检查 map。

### 8.2 推荐第一版参数

建议每条 TCP stream 使用独立 packet pool：

```c
#define APP_TELEMETRY_PACKET_COUNT 16U
#define APP_IR_PACKET_COUNT        16U
```

每个 stream socket 的发送队列建议先配置为：

```text
12～14 个 packet
```

推荐初值：

```c
nx_tcp_socket_transmit_configure(
    socket,
    14U,
    NX_TCP_TRANSMIT_TIMER_RATE,
    NX_TCP_MAXIMUM_RETRIES,
    NX_TCP_RETRY_SHIFT);
```

设置原则：

```text
socket TX queue maximum
    <
该 socket packet pool packet 数量
```

必须保留至少 2 个 packet 用于：

- 构造下一个 segment；
- 错误恢复；
- 短时驱动占用。

### 8.3 为什么不推荐 64/128

当前 SRAM 下：

```text
每条流 64 个 packet
每条流 96 个 packet
```

均不可行。

即使放入 PSRAM，Ethernet DMA、Cache 一致性和 NetX packet pool 的访问性能也必须单独验证，不能直接作为第一阶段方案。

---

## 9. Ethernet TX descriptor 的判断

当前 HAL 默认配置：

```c
ETH_TX_DESC_CNT = 4
ETH_RX_DESC_CNT = 4
ETH_DMA_TX_CH_CNT = 2
ETH_DMA_RX_CH_CNT = 2
```

Ethernet 驱动使用中断发送：

```c
HAL_ETH_Transmit_IT()
```

一个 Ethernet frame 如果只由一个 NX packet 组成，通常只需要一个 TX descriptor。

因此第一阶段不建议先修改 descriptor 数量。

只有在以下证据出现时才增加：

- `HAL_ETH_Transmit_IT()` 返回忙或错误；
- Ethernet driver error 计数增加；
- TX descriptor 长期无法回收；
- TCP 队列有数据但 MAC 发送长期停顿。

如果后续增加 descriptor 数量，需要同时检查固定描述符地址和链接布局，避免 RX/TX 描述符区域重叠。

---

## 10. 应用层无损缓存

TCP 只保证连接正常期间的可靠有序传输，不能自动保证采集源永远不丢数据。

真正的无损要求：

```text
生产速度短时间大于发送速度时：
数据先进入应用层缓存

连接断开时：
明确决定继续缓存、暂停采集或报告失败

缓存满时：
禁止静默覆盖未发送数据
```

### 10.1 AD/AI

建议：

```text
AD7606 DMA
    ↓
固定大小 block ring
    ↓
AI 消费游标
    ↓
TCP 消费游标
```

AI 和 TCP 应使用独立消费游标，避免网络反压阻塞 AI。

### 10.2 红外

提供两种模式：

```text
LIVE：
    队列满时丢旧帧，保留最新帧

LOSSLESS：
    不覆盖未发送帧，缓存满时降低帧率或暂停采集
```

### 10.3 PSRAM 的使用条件

PSRAM 环形缓存方向是合理的，但当前工程中 PSRAM 仍需要完成：

- 容量和地址验证；
- Cacheable/non-cacheable 策略；
- DMA 并发访问测试；
- Ethernet、NPU、Tiny1C 并发压力测试；
- 断线积压恢复测试。

所以 PSRAM 是第二阶段无损能力，不是解决当前 200 ms 停顿的第一步。

---

## 11. 必须先增加的诊断信息

每个 TCP stream 记录：

```text
send_call_count
send_success_count
send_queue_depth_count
send_window_overflow_count
send_other_error_count
send_wait_total_ms
send_wait_max_ms
tcp_tx_queue_depth
tcp_tx_window
tcp_rx_window
tcp_retransmit_packets
app_queue_depth
app_queue_high_watermark
source_sequence_gap
```

发送前后调用：

```c
nx_tcp_socket_info_get()
```

重点获取：

```text
tcp_transmit_queue_depth
tcp_transmit_window
tcp_receive_window
tcp_retransmit_packets
```

Windows Wireshark 同时检查：

```text
ACK 间隔
ACK 确认的 segment 数量
ZeroWindow
Window Update
TCP Retransmission
TCP Dup ACK
设备停顿时最后一个 segment 的长度
```

只有看到：

```text
设备只剩一个未确认 segment
Windows 约 200 ms 后才 ACK
ACK 后设备立即恢复
```

才能确认 Delayed ACK 是主要根因。

---

## 12. 推荐实施顺序

### 阶段 0：确认真实根因

1. 确认当前测试固件究竟是 TCP 还是 UDP；
2. 记录每次 `nx_tcp_socket_send()` 的返回值和耗时；
3. 记录 TCP queue depth 和 advertised window；
4. 使用 Wireshark 抓包；
5. 确认 PC 接收线程没有被 UI 或磁盘阻塞。

### 阶段 1：最小代码改造

1. 保留两个独立发送线程；
2. 两个线程设为相同优先级；
3. 每次最多发送 4 个 MSS segment；
4. 使用 `NX_NO_WAIT`；
5. 使用 queue-depth notify 唤醒；
6. Header 与第一段 Payload 合并；
7. 每流 packet pool 从 4 增加到 16；
8. 每 socket TX queue 设置为 12～14。

### 阶段 2：应用层反压

1. AD/AI 增加有界 block ring；
2. 红外增加 LIVE/LOSSLESS 模式；
3. 网络发送只移动 TCP 消费游标；
4. 网络断开不阻塞 DMA 和 AI；
5. 缓存满时执行明确策略。

### 阶段 3：PSRAM 无损窗口

1. 完成 PSRAM 和 Cache 验证；
2. 将大数据 ring 移入 PSRAM；
3. 测试断线积压和恢复；
4. 进行 30 分钟、8 小时和 24 小时稳定性测试。

---

## 13. 验收标准

### 13.1 TCP 发送

```text
周期性 200 ms 停顿消失或原因明确
NX_TX_QUEUE_DEPTH 不导致采集线程阻塞
TCP retransmission 正常局域网下为 0 或接近 0
控制通道在双流满载时仍可响应
```

### 13.2 公平性

```text
AD/AI 和红外均持续有发送机会
任意一条流不能连续长期占用发送路径
单 socket 零窗口不冻结另一条 socket
```

### 13.3 无损性

```text
CRC 错误为 0
未解释 sequence gap 为 0
缓存未发生静默覆盖
断线和缓存溢出有明确状态上报
```

### 13.4 实时性

```text
AD7606 DMA error 为 0
Tiny1C DMA error 为 0
AI 输入不因网络反压产生新增缺口
```

---

## 14. 最终推荐方案

当前最可行、风险最低的方案不是立即建立一个大而复杂的统一发送任务，而是：

```text
两个独立 TCP sender
        +
每流独立 16-packet pool
        +
每 socket 12～14 TX queue
        +
NX_NO_WAIT
        +
queue-depth notify
        +
每次最多 4 个 MSS segment
        +
应用层有界数据缓存
```

Windows 端：

```text
持续 recv()
接收与处理分离
TCP_NODELAY 只作为反向小数据优化
Delayed ACK 修改只用于问题诊断
```

该方案的特点：

- 能在当前内部 SRAM 约束下实现；
- 不要求立即完成 PSRAM；
- 不会让一个 socket 的 200 ms 等待直接拖住另一条流；
- 修改量小于统一发送调度器；
- 后续仍可平滑扩展到统一调度和 PSRAM 无损窗口。

---

## 15. 可行性评级

| 项目 | 可行性 | 说明 |
|---|---|---|
| 两条独立 TCP 流 | 高 | NetX 支持，连接状态可独立管理 |
| `NX_NO_WAIT` + queue-depth notify | 高 | 当前 `nx_user.h` 已启用通知功能 |
| 每流 16-packet pool | 中高 | 预计可用，但编译后只剩约 18 KB，必须检查 map |
| 每 socket TX queue 12～14 | 高 | 与 16-packet 独立 pool 匹配 |
| Header 与第一段 Payload 合并 | 高 | 修改小，可减少小 segment |
| 完整消息 packet chain 一次发送 | 低 | 不减少 segment 和 TX queue 占用，并增加复制风险 |
| 单一统一发送线程 | 中 | 非阻塞实现可行，但第一阶段复杂度较高 |
| 每流 64/96 packet | 低 | 当前内部 SRAM 不足 |
| PSRAM 无损环形缓存 | 中 | 方向正确，但必须先完成 Cache/DMA 并发验证 |
| 依赖 Windows 注册表关闭 Delayed ACK | 低 | 不适合作为产品运行条件 |

