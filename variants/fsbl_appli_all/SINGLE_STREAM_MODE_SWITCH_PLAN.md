# 单流切换方案 (Single-Stream Mode Switch)

## 目标
一次只输出一路数据流，通过 TCP 指令切换，彻底避免 IP 线程争抢。
- **AD+AI 模式**（默认上电）：AD7606 波形 + AI 结果，目标 ~44fps
- **IR 模式**：Tiny1C 温度帧，目标 **20fps**

两种模式下 AI 推理都持续运行（IR 模式下只是不发送 bundle）。

---

## CPU 预算验证

| 模式 | IR采集 | 发送(IP线程) | AI推理 | 合计 | 结论 |
|------|--------|-------------|--------|------|------|
| AD+AI | 6fps @优先级13 (~2%) | 44fps×7=308次/s (~22%) | 15% | ~40% | 全速无压力 |
| IR | 20fps @优先级7 (~2%) | 20fps×71=1420次/s (~71%) | 15% | ~88% | 紧但可达20fps |

关键：单流模式下**包池不再被两路竞争**，消除了导致 0.8fps 崩溃的根本原因（包池耗尽）。

---

## 实现

### 新增文件

**`Appli/NetXDuo/App/app_stream_mode.h` / `.c`** — 模式状态机

```c
typedef enum {
  APP_STREAM_MODE_ADAI = 0,   // 默认
  APP_STREAM_MODE_IR   = 1,
  APP_STREAM_MODE_SWITCHING = 2
} App_StreamMode_t;

UINT App_StreamMode_Init(void);
App_StreamMode_t App_StreamMode_Get(void);
UINT App_StreamMode_Set(App_StreamMode_t);
uint8_t App_StreamMode_TryAcquireSend(App_StreamMode_t);
void App_StreamMode_ReleaseSend(void);
```

当前实现还增加了内部 `APP_STREAM_MODE_SWITCHING` 状态和发送租约：
发送线程只有取得当前模式的租约后才能复制/发送一条 MMS2 消息；切换命令先
阻止新租约并等待旧消息发送完成，再修改 IR 优先级和 AI bundle consumer。
这样可以避免切换时一条流尚未结束、另一条流已经开始发送。

板端实测发现，IR 采集优先级 7 会使优先级 17 的 IR 发送线程长期饥饿，
出现单帧 CRC/发送耗时数秒至数十秒。当前实现改为：

```text
IR capture: priority 13
IR sender idle: priority 17
IR sender active: priority 12
```

切换发生时，分片循环检测 `SWITCHING` 并主动结束当前未完成消息。

`App_StreamMode_Set` 的副作用：
- **→ ADAI**：`App_IRCapture_SetActive(0)` + `App_AI_SetBundleConsumerActive(1)`
- **→ IR**  ：`App_IRCapture_SetActive(1)` + `App_AI_SetBundleConsumerActive(0)`

> consumer=0 时 AI 仍跑推理（AISTAT 的 top/out 照常更新），只是不入队，避免队列溢出。

### 修改文件

**`app_ir_capture.c`**
- 编译期默认优先级保持 `13U`；当前 16 KB SPI 分块配置已能达到目标采集率，
  不再把 IR 采集提升到优先级 7，避免饿死 IR 网络发送线程
- 新增 `App_IRCapture_SetActive(uint8_t)` — 内部调 `tx_thread_priority_change`
- `PRIORITY_IDLE=13`、`PRIORITY_ACTIVE=13`；IR 模式的发送线程动态提升为 12

**`app_stream_telemetry.c`** — 发送循环加模式门
```c
for (;;) {
  if (App_StreamMode_Get() != APP_STREAM_MODE_ADAI) { tx_thread_sleep(5U); continue; }
  /* 现有发送逻辑不变 */
  tx_thread_sleep(1U);
}
```
（移除线程内的 `App_AI_SetBundleConsumerActive(1)`，改由模式模块统一管理）

**`app_ir_stream.c`** — 发送循环加模式门 + 帧率改 20fps
```c
#define APP_IR_STREAM_MAX_FPS  20U   // 从 2U 改为 20U
...
for (;;) {
  if (App_StreamMode_Get() != APP_STREAM_MODE_IR) { tx_thread_sleep(5U); continue; }
  /* 现有发送逻辑不变 */
}
```

**`app_tcp_command.c`** — 新增指令
- `MODE`        → 返回当前模式 (`OK MODE=ADAI` / `OK MODE=IR`)
- `MODE ADAI`   → 切到 AD+AI，返回 `OK MODE=ADAI`
- `MODE IR`     → 切到 IR，返回 `OK MODE=IR`
- HELP 列表追加 `MODE`

**`app_netxduo.c`** 或 `app_threadx.c`
- 所有子系统启动后调用 `App_StreamMode_Init()`（设默认 ADAI 并应用副作用）

**`Makefile/Appli/Makefile`** — 添加 `app_stream_mode.c`

---

## 切换时序

```
上电 → App_StreamMode_Init() → ADAI 模式
  IR采集@13(6fps保温), 遥测发送AD+AI@44fps, IR流idle

PC发送 "MODE IR" → App_StreamMode_Set(IR)
  IR采集升到@7(冲20fps), AI consumer=0(不入队), 遥测idle, IR流发送@20fps

PC发送 "MODE ADAI" → App_StreamMode_Set(ADAI)
  IR采集降回@13, AI consumer=1, 遥测恢复发送, IR流idle
```

切换即时生效（IR 采集始终保温运行，无 5 秒 warmup 延迟）。

---

## 验证步骤
1. 编译 → 签名 → 烧录
2. 默认上电：viewer 应显示 AD+AI ~44fps，AI 推理 3ms，波形+分类正常
3. `echo "MODE IR" | nc 192.168.6.50 5000`（或等效）→ IR 端口 5101 出 ~20fps
4. `MODE ADAI` → 切回，AD+AI 恢复 44fps
5. 查 `IRSTAT`/`AISTAT` 确认无队列溢出、无 late

TCP 控制端口的实际命令为：

```text
MODE
MODE IR
MODE ADAI
```

预期响应分别为：

```text
OK MODE=ADAI
OK MODE=IR
OK MODE=ADAI
```

烧录后也可以直接运行自动往返检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\single_stream_mode_switch_test.ps1 `
  -SettleSeconds 5
```

---

## 与 Jumbo Frame 的关系
本方案不依赖 Jumbo Frame。若后续要在**单模式内**进一步提速（如 IR@30fps 或双流并发），再加 Jumbo Frame（MTU 4KB，数据报数减少 6×）。当前单流 20fps 目标无需它。
