# A12 · PS 侧服务：采集核心、解包、定点 FFT 与 TCP 前端

覆盖文件（`sw/`）：

| 文件 | 行数 | 角色 |
|---|---|---|
| `ps_service/include/pd_hw_map.h` | 89 | **唯一的硬件契约来源**（AXI-Lite 偏移、DMA 寄存器、PS DDR 分配） |
| `ps_service/include/pd_acquisition.h` | 73 | 采集核心公共 API 与归档元数据结构 |
| `ps_service/src/pd_acquisition_core.c` | 372 | **采集核心**：DMA S2MM、事件归档、快照归档、槽管理、停机排空、故障恢复 |
| `ps_service/src/pd_snapshot_unpack.c` | 82 | **原始快照解包**（6 字节/采样点 → 四通道） |
| `ps_service/src/pd_spectrum.c` | 236 | **定点 1024 点 FFT** + 去直流 + Hann 窗 + 峰值搜索 + 端到端自检 |
| `ps_service/src/main.c` | 132 | UART 命令行前端（非阻塞轮询 + 行缓冲解析） |
| `ps_service/tcp/pd_tcp_service.c` | 647 | **TCP 服务**（lwIP RAW 模式，端口 6001） |
| `ps_service/tcp/main.c` | 45 | TCP 应用入口（lwIP 初始化） |
| `sw/pd_snapshot_poll.c` | 315 | **上板冒烟测试**（自动快照 / 四槽轮转 / 槽满拒绝） |

> 旧版整体式服务与调试探针（`pd_acquisition_service.c`、`pd_capture_service.c`、`pd_acquisition_tcp_service_main.c`、`dma_s2mm_probe*.c`、`pd_filter_apply.c` 等）见 `A15_PS侧_旧版采集服务与调试探针逐行注释.md`。

---

## 0. 架构：裸机单线程的"协作式并发"

```
main()
 ├─ pd_acq_init()                    ← 初始化 AXI DMA（Simple 模式，关中断）
 ├─ [TCP 版] lwIP 初始化 + 监听 6001
 └─ for (;;) {
        uart_poll() / pd_tcp_service_poll()   ← 服务命令 + lwIP 定时器 + 收包
        pd_acq_poll()                          ← 采集状态机"推进一格"
    }
```

⭐ **三条关键设计**：
1. **`pd_acq_poll()` 是非阻塞的状态机推进函数** —— 不阻塞等待任何硬件事件（DMA 完成 / 快照就绪），"能推进就推进"。**这是唯一能让"采集"与"网络"并存的办法**（没有 RTOS）。
2. **`pd_acq_poll_hook_t` 回调**（`pd_acquisition.h:50-51`）：在采集核心的**长等待循环**里反复调用，用于继续服务网络。
   - 否则等待 DMA 的 2000 万次轮询期间，**lwIP 定时器停摆 → 连接超时、收包积压**。
   - `tcp/main.c:41`：`pd_acq_set_poll_hook(pd_tcp_service_poll);` ✓
3. **四态粘滞状态机**：
   ```
   IDLE → RUNNING → STOPPING → IDLE
            └──────────────┴──► FAULT ──(pd_acq_recover)──► IDLE
   ```
   `STOPPING` 是"优雅停止"（排空在途数据），`FAULT` 是"粘滞故障"（`pd_acq_poll` 直接返回失败，必须显式 `RECOVER`）。

⭐ **分层纪律**（`pd_acquisition.h:4-6`）：**UART/TCP 前端必须走公共 API，不得直接读写 AXI 寄存器或改 `g_pd_acq`。**
- 现状：**写**确实全走 API；**读**存在妥协（`pd_tcp_service.c:139-147` 直接读 `g_pd_acq.*` 显示状态）✓ 风险低。

---

## 1. `pd_hw_map.h` —— 硬件契约的唯一来源

### 1.1 基址的"猜宏名"与跨 Vitis 版本兼容

```
13 #if defined(XPAR_PD_DDR_0_BASEADDR)              // 新版 Vitis：宏名 = BD 实例名大写
14 # define PD_DDR_BASE ((UINTPTR)XPAR_PD_DDR_0_BASEADDR)
15 #elif defined(XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR) // 或按 module_ref 全名
16 # define PD_DDR_BASE ((UINTPTR)XPAR_PD_DDR_BD_ADAPTER_0_BASEADDR)
17 #else
18 # error "Cannot find pd_ddr AXI-Lite base address in xparameters.h"
19 #endif
21 #if defined(XPAR_XAXIDMA_0_BASEADDR)              // 新版：LookupConfig 的参数是"基址"
22 # define PD_DMA_BASE       ((UINTPTR)XPAR_XAXIDMA_0_BASEADDR)
23 # define PD_DMA_LOOKUP_ARG XPAR_XAXIDMA_0_BASEADDR
24 #elif defined(XPAR_AXIDMA_0_BASEADDR) && defined(XPAR_AXIDMA_0_DEVICE_ID)
25 # define PD_DMA_BASE       ((UINTPTR)XPAR_AXIDMA_0_BASEADDR)
26 # define PD_DMA_LOOKUP_ARG XPAR_AXIDMA_0_DEVICE_ID   // 旧版：参数是"设备 ID"
27 #else
28 # error "No AXI-DMA base macro: regenerate the Vitis platform from the matching XSA."
29 #endif
```
⭐ **两个要点**：
- `XPAR_*` 宏是 Vitis **从 `.xsa` 自动生成**的，宏名由 **BD 里的实例名**决定 → 所以这里"猜两个候选名"，都不匹配就 `#error` ✓；
- ⭐ **`PD_DMA_LOOKUP_ARG` 是"适配新旧两套 Vitis"的关键**：新版 `XAxiDma_LookupConfig(基址)`，旧版 `XAxiDma_LookupConfig(DeviceId)` → **参数类型不同，所以必须把"查找参数"单独定义**。
- ⭐ **`#error` 优于运行期错**：它在**编译期**就暴露"平台与 bit 不同源"这个根因（`xparameters.h` 与 `.xsa`/bit 必须同源）。

### 1.2 寄存器偏移（与 RTL 逐一核对）

| C 宏（`pd_hw_map.h`） | 值 | RTL（`pd_ddr_axil.v`） | 语义 |
|---|---|---|---|
| `PD_DDR_CTRL` | `0x000` | `A_CTRL = 12'h00` | `[0]acq_en [1]sw_rst(W1P) [2]snap_start(W1P)` |
| `PD_DDR_STATUS` | `0x004` | `A_STATUS = 12'h04` | 见 §1.3 |
| `PD_FREEZE_CTRL` | `0x034` | `A_FZ_CTRL = 12'h34` | `[0]trig [1]resume`，只读 `[8]freeze_done` |
| `PD_SLOT_CTRL` | `0x04C` | `A_SLOT_CTRL = 12'h4C` | `[0]auto_snap_en [7:4]lock [11:8]release [12]status_clear` |
| `PD_SLOT_STATUS` | `0x050` | `A_SLOT_STATUS = 12'h50` | `[3:0]valid [7:4]busy [11:8]locked [12]full [13]cfg_err [14]req_ovf [15]cmd_err [16]pending [18:17]last_slot [19]ready` |
| `PD_SLOT_SEQ` | `0x054` | `A_SLOT_SEQ` | 自动快照成功序号 |
| `PD_SLOT_DROPS` | `0x058` | `A_SLOT_DROPS` | 请求被拒/溢出累计 |
| `PD_SNAP_TRIG_CTRL` | `0x05C` | `A_SNAP_TRIG_CTRL` | `[0]en [4:1]mask [8]drop_clear`，只读 `[16]armed [17]drop` |
| `PD_SLOT_BASE_OFF(n)` | `0x060 + n*0x10` | `A_SLOT0_BASE = 12'h60` | 四槽描述符，步长 `0x10` |
| `PD_SLOT_LEN_OFF(n)` | `0x064 + n*0x10` | `A_SLOT0_LEN = 12'h64` | — |
| `PD_SLOT_SEQ_OFF(n)` | `0x068 + n*0x10` | `A_SLOT0_SEQ = 12'h68` | — |
| `PD_SLOT_FLAGS_OFF(n)` | `0x06C + n*0x10` | `A_SLOT0_FLAGS = 12'h6C` | `[2]locked [1]busy [0]valid` |

⭐ **全部一致** ✓（把 `0x06C` 等十六进制与 `12'h6C` 对照即可）。

### 1.3 状态位掩码（含一个命名陷阱）

```
53 #define PD_DDR_COPY_BUSY         (1U << 1)     // DDR_STATUS[1]
54 #define PD_DDR_RING_ERR          (1U << 5)     // DDR_STATUS[5]
55 #define PD_DDR_COPY_ERR          (1U << 8)     // DDR_STATUS[8]
56 #define PD_DDR_CFG_ERR           (1U << 9)     // DDR_STATUS[9]
...
64 #define PD_SLOT_READY            (1U << 19)    // SLOT_STATUS[19]
65 #define PD_TRIG_ENABLED          (1U << 0)
66 #define PD_TRIG_MASK_ALL         (0xFU << 1)
67 #define PD_TRIG_ARMED            (1U << 16)
```

⚠️⭐ **命名陷阱（必须记住）**：`PD_DDR_RING_ERR`（`1U << 5`）这个名字**暗示"环形写错误"**，而 RTL 侧的表达式是（`pd_ddr_wr_top.v:743`）：
```
i_err = ring_err | ring_ovf | copy_err | snapshot_cfg_err
```
→ **它是"四路错误的或"，不是单纯的 `ring_err`**，而 `copy_err`/`snapshot_cfg_err` 另有 `[8]`/`[9]` 两个专门的位。
→ ⭐ **所以 `[5] = 1` 且 `[8] = [9] = 0` 时，只可能是 `ring_err` 或 `ring_ovf`，两者无法区分**（要区分必须引出 `dbg_ddr[9]`，见 `B09` Y-2）。
→ ⭐ **PS 侧代码对此的处理是"正确且成熟"的**：`archive_new_snapshot()` **不用 `[5]` 判"拷贝是否失败"**，而是用语义无歧义的 `[8]`/`[9]`（见 §3.3）✓。

### 1.4 PS DDR 内存分配与"不得重叠"铁律

| 区域 | 地址 | 大小 | 所有者 |
|---|---|---|---|
| PL 环形区 | `0x1000_2000` ~ `0x1800_0000` | 128 MiB−8 KiB | PL（`pd_ddr_ring_wr`） |
| PL legacy 快照区 | `0x1800_0000` ~ `0x187F_E000` | 8 MiB−8 KiB | PL（legacy 路径） |
| **PL 四槽快照区** | `0x2000_1000` ~ `0x2300_1000` | 48 MiB | PL（`pd_ddr_slot_mgr`） |
| **PS 快照归档** | `0x2400_0000` | 4 × 12 MiB = 48 MiB | PS（`PD_SNAP_ARCHIVE_*`） |
| **PS 事件归档** | `0x2700_0000` | 16 × 64 KiB = 1 MiB | PS（`PD_EVENT_ARCHIVE_*`） |
| **DMA 接收缓冲** | `PS_DDR_BASE + 0x0100_0000` | 65,528 B | PS（S2MM 目标） |

⭐ **文件头第 77 行的纪律**："**不要与 PL 环形区或硬件快照槽重叠。**"
- ⭐ **为什么这是硬约束？** 如果 PS 把归档区放在 PL 环形区里，那么：
  - PL 写入会**覆盖** PS 的归档数据；
  - 或者 PS 的归档写入会**破坏** PL 的环形缓冲 → 快照内容错乱。
  - **两种后果都是静默的**（数据看起来正常，只是不对）。
- ⭐ `PD_SNAP_SLOT_LOW`/`HIGH`（`0x2000_1000`/`0x2300_1000`）正是 **PL 槽区的边界**，被 `archive_new_snapshot()` 用作 "描述符合法性检查" ✓（见 §3.3）。

⚠️ **`PD_RX_BUFFER_BYTES = 65528` 的由来**：`65536 − 8`。因为 `XAxiDma_SimpleTransfer` 的长度有上限（65536），取 65536 会越界，所以减一个 beat（8 字节）✓。

---

## 2. 归档元数据结构（`pd_acquisition.h`）

```
 7 typedef struct { u32 sequence, ddr_addr, bytes, peak_words, cycle_words; } pd_event_record_t;
15 typedef struct { u32 sequence, source_slot, source_addr, archive_addr, bytes,
                     hw_slot_sequence, flags; } pd_snapshot_record_t;
```
⭐ **两条记录的关键字段**：
| 字段 | 事件记录 | 快照记录 | 说明 |
|---|---|---|---|
| `sequence` | ✓ | ✓ | 全局自增序号（支持"按序号查询"，PS 不必自己算 `seq % COUNT`） |
| `ddr_addr` / `archive_addr` | `ddr_addr` | `archive_addr` | **在 PS DDR 里的归档地址**（不是 PL 的地址） |
| `source_slot` / `source_addr` | — | ✓ | ⭐ **来自哪个 PL 硬件槽 + 槽的物理地址**（可追溯） |
| `hw_slot_sequence` | — | ✓ | ⭐ **PL 侧该槽的 `slot_seq`**（**交叉核对**："我搬的这份对应 PL 第几次快照"） |

⭐ **共享状态（第 25–41 行）里两个"诚实计数器"**：`event_overwrites` / `snapshot_overwrites`。
- 归档容量有限（事件 16 槽、快照 4 槽），PS 若处理不及就会被覆盖；
- ⭐ **用计数器公开这个事实，而不是假装无损** ✓（与 `sw/ps_service/README.md:38` "覆盖是预期行为，不作为失败条件"一致）。

---

## 3. `pd_acquisition_core.c` —— 采集核心

### 3.1 三个"轮询上限"（裸机必备的防御）

| 常量 | 值 | 用途 |
|---|---|---|
| `PD_DMA_POLL_LIMIT` | 20,000,000 | 等 DMA 完成 |
| `PD_SHUTDOWN_POLL_LIMIT` | 1,000,000 | 等停机排空 / 复位完成 |
| `PD_SLOT_READY_POLL_LIMIT` | 1,000,000 | 等槽描述符自洽的**重试**上限 |

⭐ **为什么必须有上限？** 裸机没有看门狗；`for(;;)` 挂住 = **网口彻底失联，只能按复位键**。设上限后至少能进 `FAULT` 并报错 ✓。

⭐ **`PD_SLOT_READY_POLL_LIMIT` 的注释（`:19-24`）解释了一个真实现象**：
> `SLOT_STATUS` 是通过 AXI-Lite 采样的，而 PL 侧在**同时更新**它 → 可能观察到"`READY` 已置位但选中槽的 `VALID` 还没置位"的中间态。
> **所以这是"重试上限"，不是新的硬件超时；持久的（durable）不匹配仍然是故障。**

→ ⭐ **这是一条重要的一般化结论：跨 PL/PS 边界的"多信号一致性读"不能假设原子，必须"重试 + 最终校验"。**

### 3.2 `fail()` —— 故障现场快照

```
45 static int fail(const char *why)
46 {
47     s_last_error = why;                                  // ① 记录"为什么"
48     g_pd_acq.last_ddr_status  = ddr_read(PD_DDR_STATUS);  // ② 立刻抓三个状态寄存器
49     g_pd_acq.last_slot_status = ddr_read(PD_SLOT_STATUS);
50     g_pd_acq.last_dma_status  = dma_read(PD_S2MM_DMASR_OFFSET);
51     s_state = PD_ACQ_FAULT;                              // ③ 置粘滞故障态
52     xil_printf("PD_ACQ_FAULT: %s ddr=%08x slot=%08x dma=%08x\r\n", ...);  // ④ 打印
55     return XST_FAILURE;
56 }
```
⭐ **四件事缺一不可**，尤其 **② 的时机**：**故障之后状态可能变化**（DMA 被复位、槽被释放），**事后再读就看不到现场了** ✓。
⭐ **它不重置硬件、不重试、不掩盖** —— 只是"记录现场 + 进入明确故障态 + 要求显式恢复"。**这是"错误必须被解释"的直接体现。**

### 3.3 `configure_capture()` —— 启动配置序列（**权威样板，务必背下来**）

| 步骤 | 写什么 | 意图 |
|---|---|---|
| 1 | `DDR_CTRL = 0` | 关采集（同时清 `ring_wr` 的 `o_err`/`o_ovf`） |
| 2 | `FREEZE_CTRL = 2` | 发 `resume`（清冻结状态、清 `snap_copy.o_err`、取消槽预留） |
| 3 | **轮询等到 `DDR_STATUS[0]`（acq_en）与 `[5]`（err）都为 0** | ⭐ **确认"清干净了"**；超时 → `fail("stale ring status did not clear")` |
| 4 | `SLOT_CTRL = (1<<12)` | `status_clear`：清槽的 5 个粘滞标志 + 清 `snapshot_ready` |
| 5 | `SNAP_TRIG_CTRL = (1<<8) \| MASK_ALL \| ENABLED` | 清 drop 计数 + 预设 mask/enable |
| 6 | `DDR_CTRL = 1` | 开采集 |
| 7 | `SLOT_CTRL = 1` | `auto_snap_en = 1`（四槽自动模式） |
| 8 | `SNAP_TRIG_CTRL = MASK_ALL \| ENABLED` | 再写一次（不带 drop_clear）确认使能 |
| 9 | ⭐ **回读校验 `(ENABLED \| ARMED)` 都为 1** | **必须"已武装"才算成功** |

⭐⭐ **第 9 步是整个序列里最关键的一步**，原因是（`pd_snapshot_trigger.v:34`）：
```
o_armed = i_enable && !i_slot_full && !waiting_resume
```
- 所以**即使 `enable = 1`，如果"槽全满"或"上一次事务没 resume"，`armed` 就是 0** → **自动快照永远不会触发**（静默失效）；
- ⭐ **回读校验把"静默失效"变成"明确的启动失败"** ✓ **这是判据设计的范例。**
- ⭐ **它同时解释了 `pd_acq_recover()` 为什么必须"逐槽锁定并释放"**（`:336-346`）：**不清空旧槽就无法重新武装** → 逻辑闭环。

### 3.4 `archive_event_packet()` —— 事件归档

```
81     u32 i, peaks = 0U, cycles = 0U, words = bytes / 8U;
87     if (!bytes || bytes > PD_RX_BUFFER_BYTES || (bytes & 7U)) return fail("bad DMA byte count");
88     for (i = 0U; i < words; ++i) {
89         u32 type = (u32)(rx[i] >> 56);       // 取事件包最高 8 位 = type
90         if (type == 0U) ++peaks; else if (type == 1U) ++cycles;
92         else return fail("unknown event word type");
93     }
94     if ((u32)(rx[words - 1U] >> 56) != 1U) return fail("AXIS packet did not end in cycle word");
96     memcpy((void *)dst, (const void *)PD_RX_BUFFER_BASE, bytes);
97     Xil_DCacheFlushRange(dst, bytes);
```
⭐ **逐条要点**：
- **第 85 行 `volatile u64 *rx`**：把 DMA 缓冲按 64 位字访问（与事件包宽度一致）；`volatile` 保证"每次都真的从内存读"（DMA 写的内容编译器不知道）✓；
- **第 87 行三重校验**：① 长度非 0；② **不超过缓冲大小**（防越界读，因为 `bytes` 来自 DMA 的 `LENGTH` 寄存器，出问题可能是垃圾值）；③ **8 字节对齐**（要按 `u64` 访问）；
- **第 89 行 `rx[i] >> 56`**：取最高 8 位 = `type` ✓ 与事件包 `[63:56]` 一致；
- ⭐ **第 94 行"最后一个字必须是统计包（type=1）"**：**因为 AXI DMA 的传输以 TLAST 结束，而 TLAST 只在周期统计包上**（`pd_feature_core.v:844`）→ **最后一个 beat 必然是统计包** ✓ **这条判据同时验证了数据完整性。**
  - ⚠️ **它不能验证"四通道的帧都完整"**（四通道事件交织，最后一个统计包可能属于任意通道）—— 见 `A06` §2.5。
- **第 96–97 行**：`memcpy` 到归档区 + **`Xil_DCacheFlushRange`**（把 CPU cache 里的数据写回内存）✓。

### 3.5 `archive_new_snapshot()` —— 快照归档（**本文件最关键**）

```
110    u32 status = ddr_read(PD_SLOT_STATUS);
111    u32 seq = ddr_read(PD_SLOT_SEQ);
117    if (status & (PD_SLOT_CFG_ERR | PD_SLOT_REQ_OVERFLOW | PD_SLOT_CMD_ERR)) {
118        ++g_pd_acq.slot_errors;
119        return fail("slot manager error");
120    }
121    if (g_pd_acq.last_ddr_status & (PD_DDR_COPY_ERR | PD_DDR_CFG_ERR))
122        return fail("DDR copy error");
123    if (!(status & PD_SLOT_READY) || seq == s_last_slot_sequence) return XST_SUCCESS;
```
⭐⭐ **第 117–123 行的三组判断，顺序与用位都极其讲究**：

| 组 | 判据 | 用什么位 | 为什么 |
|---|---|---|---|
| ① 槽管理器错误 | `CFG_ERR \| REQ_OVERFLOW \| CMD_ERR` | **槽自己的位（`[13][14][15]`）** | 与 `DDR_STATUS` 无关 |
| ② 拷贝/配置错误 | `DDR_STATUS[8] \| [9]` | ⭐ **`PD_DDR_COPY_ERR`/`PD_DDR_CFG_ERR`** | ⭐ **刻意避开 `[5]`！** 因为 `[5]` 混进了 `ring_ovf`（一次性启动瞬态）→ 用它判"快照失败"会**每次启动都误报** |
| ③ 是否需处理 | `!(READY) \|\| seq == s_last_slot_sequence` | `SLOT_STATUS[19]` + 序号 | 无 READY 或序号未变 → 什么都不做 |

⭐ **第 ③ 组的重要性**：`s_last_slot_sequence` **只在成功归档后才更新**（`:161`）→ **处理失败会自动重试** ✓。

```
125    slot = (status >> PD_SLOT_LAST_SHIFT) & 0x3U;      // PD_SLOT_LAST_SHIFT = 17
126    if (!(status & (1U << slot))) {
127        /* Do not consume seq: retry until this slot descriptor is coherent. */
128        if (++s_slot_ready_polls < PD_SLOT_READY_POLL_LIMIT) return XST_SUCCESS;
129        return fail("READY without valid slot timeout");
130    }
131    s_slot_ready_polls = 0U;
132    ddr_write(PD_SLOT_CTRL, 1U << (4U + slot));                     // lock
133    if (!(ddr_read(PD_SLOT_STATUS) & (1U << (8U + slot)))) return fail("slot lock failed");
```
- ⭐ **第 126–130 行实现"重试语义"**：读到"READY 已置位但该槽 VALID 还没置位"的中间态时 → **不消耗 `seq`**（不记录）→ 下次轮询重试；**只有连续失败超限才 `fail()`** ✓；
- ⭐ **第 132–133 行"加锁 + 回读确认"**：AXI-Lite 写是 **posted（发出即返回，不等生效）** → **写完必须回读确认** ✓ 这是跨 PL/PS 边界的必要纪律。

```
135    base  = ddr_read(PD_SLOT_BASE_OFF(slot));
136    bytes = ddr_read(PD_SLOT_LEN_OFF(slot));
137    slot_seq = ddr_read(PD_SLOT_SEQ_OFF(slot));
138    flags = ddr_read(PD_SLOT_FLAGS_OFF(slot));
139    if ((base & 7U) || (bytes & 7U) || !bytes || bytes > PD_SNAP_ARCHIVE_STRIDE ||
140        base < PD_SNAP_SLOT_LOW || base + bytes > PD_SNAP_SLOT_HIGH)
141        return fail("invalid snapshot descriptor");
```
⭐ **五项合法性校验**（**这是"防止坏描述符破坏归档区"的必要防御**）：
| 检查 | 目的 |
|---|---|
| `base & 7` / `bytes & 7` | 8 字节对齐（因为要按 `u64` 访问） |
| `!bytes` | 长度非 0 |
| `bytes > 12 MiB` | **不超过归档槽容量**（否则会溢出到相邻槽） |
| `base < LOW \|\| base + bytes > HIGH` | ⭐ **必须落在 PL 槽区内**（否则可能读到 IO 区导致总线错误） |

```
145    Xil_DCacheInvalidateRange((UINTPTR)base, bytes);      // ① 丢弃 CPU 旧缓存
146    memcpy((void *)dst, (const void *)(UINTPTR)base, bytes); // ② 拷贝
147    Xil_DCacheFlushRange(dst, bytes);                      // ③ 写回内存
158    ddr_write(PD_SLOT_CTRL, 1U << (8U + slot));            // release
159    if (ddr_read(PD_SLOT_STATUS) & (1U << (8U + slot))) return fail("slot release failed");
160    ddr_write(PD_FREEZE_CTRL, 2U);                         // ★ resume（关键！）
161    s_last_slot_sequence = seq;
```
⭐⭐ **第 145–147 行是"PS 侧 cache 管理的标准三步"**：

| 步 | 函数 | 为什么 |
|---|---|---|
| ① **invalidate 源** | `Xil_DCacheInvalidateRange(base, bytes)` | ⭐ **PL 通过 DataMover 把数据写进了这块 DDR，但 CPU cache 里可能有旧值** → 必须**丢弃 cache**，让后续读取从内存拿新数据 |
| ② **memcpy** | — | 从 PL 槽拷到 PS 归档区 |
| ③ **flush 目标** | `Xil_DCacheFlushRange(dst, bytes)` | 把 CPU cache 里的新数据**写回内存**（保证后续读到的/其他 master 读到的是新数据） |

⚠️⭐⭐ **漏做 ① 的后果是本工程最可怕的静默错误**：CPU 会读到**上一次的旧快照**，而**数据看起来完全正常**（有值、长度对、格式对）→ **分析结论全错但没有任何告警**。
→ ⭐ 这也解释了为什么 `pd_tcp_service.c:253`（`ANALYZE SNAP`）与 `:398`（`FFT SNAP`）**都在分析前再 invalidate 一次** ✓ 一致的做法。

⭐ **第 160 行 `ddr_write(PD_FREEZE_CTRL, 2U)`（resume）是"每个快照事务的结束动作"**，它一次完成**三件事**：
1. **让 `pd_snapshot_trigger` 重新武装**（`waiting_resume = 0`）→ 下一次事件可以再触发；
2. **清 `pd_ddr_ring_wr` 的 `o_err`/`o_ovf`**；
3. **清 `pd_ddr_snap_copy` 的 `o_err`**。
→ ⭐ **所以自动模式下"每快照一次就 resume 一次"是正常节奏**，不是只有人工恢复才发。

### 3.6 DMA 提交与完成判断（**含一个已知陷阱**）

```
169    Xil_DCacheFlushRange(PD_RX_BUFFER_BASE, PD_RX_BUFFER_BYTES);
170    Xil_DCacheInvalidateRange(PD_RX_BUFFER_BASE, PD_RX_BUFFER_BYTES);
171    XAxiDma_IntrAckIrq(&s_dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
172    if (XAxiDma_SimpleTransfer(&s_dma, PD_RX_BUFFER_BASE, PD_RX_BUFFER_BYTES,
173                               XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS)
174        return fail("DMA submit failed");
```
- ⭐ **第 169–170 行"先 flush 再 invalidate"**：顺序正确（若先 invalidate，cache 里的脏数据会被丢弃 → 数据丢失）✓；
- ⭐ **第 171 行 `XAxiDma_IntrAckIrq(...)` 不可省**：虽然 `pd_acq_init` 里关掉了中断，但**中断标志位仍会被硬件置位**；不清会导致某些实现下**下一次传输被拒** ✓ 隐蔽但必要。

```
259    if (XAxiDma_Busy(&s_dma, XAXIDMA_DEVICE_TO_DMA)) {
260        if (++s_dma_polls >= PD_DMA_POLL_LIMIT) { ++g_pd_acq.dma_errors; return fail("DMA completion timeout"); }
264        return XST_SUCCESS;
265    }
266    dmasr = dma_read(PD_S2MM_DMASR_OFFSET);
267    bytes = dma_read(PD_S2MM_LENGTH_OFFSET);
270    if ((dmasr & (PD_DMASR_HALTED | PD_DMASR_ERROR_MASK)) || !(dmasr & PD_DMASR_IDLE)) {
271        ++g_pd_acq.dma_errors;
272        return fail("DMA completion failed");
273    }
274    Xil_DCacheInvalidateRange(PD_RX_BUFFER_BASE, bytes);
```
⚠️⭐⭐ **第 259 行的已知陷阱（`B09` 已记录）**：
- `XAxiDma_Busy()` **只看 DMA 的 `Idle` 位**；
- 若 DMA 因错误进入 **`Halted`** 状态，`Idle` 位是 0 → **`XAxiDma_Busy()` 永久返回"忙"** → **轮询白转到超时**，真正的错误（`HALTED` + `DMAIntErr`）被"超时"淹没；
- **本函数的现状是"会白等 2000 万次，但最终仍能正确报错"**（因为第 266–273 行会读 `DMASR` 做完整校验）✓ **功能正确、效率差**；
- ⭐ **改进方向（`sw/dma_s2mm_probe_v3.c` 已实现）**：**在轮询循环内部也读 `DMASR`，一发现 `HALTED` 或错误位就立刻退出并报错。**

⭐ **第 270 行的三重校验**（这才是"DMA 是否成功"的正确判据）：
1. `!HALTED`（bit0）；
2. `!(ERROR_MASK = 0x4070)`（覆盖 `DMAIntErr[4]`、`SLVERR[5]`、`DecErr[6]`、`IOC_Irq[12]`、`Dly_Irq[13]`、`Err_Irq[14]`）；
3. `IDLE`（bit1）为 1。
→ **三条全过才算完成** ✓

⭐ **第 267 行 `bytes = LENGTH 寄存器`**：S2MM 的 `LENGTH` 在传输完成后**变成实际传输的字节数** → ⭐ **这就是"变长包"的机制**（不需要预先知道包长）✓。

⭐ **`pd_acq_poll()` 的动作顺序（第 243–279 行）**：
```
246  s_poll_hook()            ← 先服务网络（即使本拍不推进采集）
247  IDLE     → 直接返回
248  FAULT    → 直接返回失败（粘滞）
249  STOPPING → 若有在途 DMA 则硬复位；然后 stop_and_drain()
257  archive_new_snapshot()  ← ★ 先归档快照
258  没有在途 DMA → submit_dma()
259  在途 → 等（带超时）
266  完成 → 校验 DMASR → invalidate → 归档事件 → 计数 → 判断 packet_limit
```
⭐ **为什么"先归档快照，再管 DMA"？** 因为快照的"就绪"**异步于** DMA（两条独立硬件通路）。若先管 DMA，可能因为"DMA 每次都很快完成"而**永远轮不到归档**（饥饿）✓ **先归档保证快照不被饿死。**

⚠️ **`pd_acq_poll` 第 252 行 `while (!XAxiDma_ResetIsDone(&s_dma)) { }` 是一个"无上限忙等"** —— 比 `PD_SHUTDOWN_POLL_LIMIT` 更危险（**没有超时**）。记入 `B09` R-6。

### 3.7 停机排空与故障恢复

```
189 static int stop_and_drain(void)
192     ddr_write(PD_SNAP_TRIG_CTRL, 0U);   // 关事件触发
193     ddr_write(PD_DDR_CTRL, 0U);         // 关采集
194     ddr_write(PD_FREEZE_CTRL, 2U);      // resume
195     for (i = 0U; i < PD_SHUTDOWN_POLL_LIMIT; ++i) {
196         if (archive_new_snapshot() != XST_SUCCESS) return XST_FAILURE;
197         if (!(ddr_read(PD_DDR_STATUS) & PD_DDR_COPY_BUSY) &&
198             !(ddr_read(PD_SLOT_STATUS) & (PD_SLOT_BUSY_MASK | PD_SLOT_VALID_MASK))) {
199             s_state = PD_ACQ_IDLE; return XST_SUCCESS;
200         }
202         if (s_poll_hook != NULL) s_poll_hook();
203     }
204     return fail("capture did not quiesce");
```
⭐⭐ **"优雅停机"的全部逻辑**：
- 三步关断（关触发 → 关采集 → resume）；
- ⭐ **在监控循环里持续调用 `archive_new_snapshot()`** —— **把还在飞、已就绪的快照捞干净**；
- **退出条件两条同时成立**：`copy_busy == 0` **且** `BUSY_MASK`、`VALID_MASK` **都为空** → ⭐ **注意 `VALID_MASK` 也被检查：所以停机是"无损"的**（所有已产生的快照都会被归档）✓；
- ⚠️ **风险**：如果某个槽的描述符非法（`invalid snapshot descriptor`），`archive_new_snapshot` 会 `fail()` → **停机永远无法完成**，只能靠 `RECOVER`。记入 `B09`。

```
314 int pd_acq_recover(u32 *discarded_slots)
318     if (discarded_slots == NULL || s_state != PD_ACQ_FAULT) return XST_FAILURE;  // 只对 FAULT 有效
322     ddr_write(PD_SNAP_TRIG_CTRL, 0U);   // 关触发
323     ddr_write(PD_DDR_CTRL, 0U);         // 关采集
324     ddr_write(PD_SLOT_CTRL, 0U);        // 关 auto_snap_en（不再分配新槽）
325     ddr_write(PD_FREEZE_CTRL, 2U);      // resume
327     等 copy_busy==0 且 BUSY_MASK==0（★ 不等 VALID_MASK —— 那些正是要丢弃对象）
335     valid = ... & PD_SLOT_VALID_MASK;
336     for (slot = 0; slot < 4; ++slot) {
337         if (!(valid & (1U << slot))) continue;
338         ddr_write(PD_SLOT_CTRL, 1U << (4U + slot));    // ★ 先 lock
341         ddr_write(PD_SLOT_CTRL, 1U << (8U + slot));    // ★ 再 release
345         ++(*discarded_slots);
346     }
347     ddr_write(PD_SLOT_CTRL, 1U << 12);  // 清槽粘滞状态
348     reset_dma();
360     s_state = PD_ACQ_IDLE;
```
⭐⭐ **`pd_acq_recover()` 揭示了两条关键机制**：
1. ⭐ **"必须先用 lock 把 `VALID` 变成 `LOCKED`，才能 release"** —— 因为 `pd_ddr_slot_mgr.v:137/141` 的规则是"**只有 VALID 能加锁、只有 LOCKED 能释放**"（见 `A09` §3.5）。**想清空一个有效槽，必须走"锁 → 放"两步。**
2. ⭐ **清空硬件槽是"重新武装触发器"的前置条件** —— 因为 `o_armed = enable && !slot_full && !waiting_resume`。**不释放旧槽 → `slot_full = 1` → `configure_capture()` 第 9 步的回读校验会失败** → 逻辑闭环 ✓。

⭐ **语义边界（`README.md:30` 明确写出）**：
> `RECOVER` **只丢弃"PL 硬件槽里还没归档的快照"**，**不丢弃 PS DDR 里已归档的事件与快照**；**不是无损恢复**，`discarded_slots` 明确报告丢了几个。

⭐ **`pd_acq_get_event_by_sequence()` 的"三重校验"（`:293-297`）**：
1. `sequence >= event_sequence`（未来的序号）→ 拒绝；
2. `event_sequence − sequence > COUNT`（**已被环形覆盖**）→ 拒绝；
3. `event[index].sequence != sequence`（**最后一道防线**：确认槽里真的是这个序号）→ 拒绝。
→ ⭐ **这是"环形归档按序号查询"的标准实现**，"二次确认"处理了"检查条件后、真正读之前被覆盖"的竞态 ✓。

---

## 4. `pd_snapshot_unpack.c` —— 6 字节解码（**PL 布局的精确落地**）

```
 6 static void decode_sample(const u8 *p, u16 sample[PD_SNAPSHOT_CHANNELS])
 8     /* p[0] is bit 7:0 of the 48-bit sample word in DDR memory. */
 9     sample[0] = (u16)p[0] | ((u16)(p[1] & 0x0FU) << 8);     // ch0 = [11:0]
10     sample[1] = ((u16)p[1] >> 4) | ((u16)p[2] << 4);        // ch1 = [23:12]
11     sample[2] = (u16)p[3] | ((u16)(p[4] & 0x0FU) << 8);     // ch2 = [35:24]
12     sample[3] = ((u16)p[4] >> 4) | ((u16)p[5] << 4);        // ch3 = [47:36]
13 }
```
⭐⭐ **这 6 行必须逐位验证**。依据是 `pd_pack48.v:53-57` 的拼接 `{ch3[47:36], ch2[35:24], ch1[23:12], ch0[11:0]}` + `pd_pack192.v:6` 声明的小端。

**48 位字在内存里的字节序（小端）**：
```
字节 0 = 字位 [7:0]      字节 3 = 字位 [31:24]
字节 1 = 字位 [15:8]     字节 4 = 字位 [39:32]
字节 2 = 字位 [23:16]    字节 5 = 字位 [47:40]
```

| 通道 | 位段 | 跨哪些字节 | 解码式 | 代码 |
|---|---|---|---|---|
| **ch0** | `[11:0]` | 字节 0 全部 + 字节 1 的**低 4 位** | `p[0] \| ((p[1] & 0x0F) << 8)` | `:9` ✓ |
| **ch1** | `[23:12]` | 字节 1 的**高 4 位** + 字节 2 全部 | `(p[1] >> 4) \| (p[2] << 4)` | `:10` ✓ |
| **ch2** | `[35:24]` | 字节 3 全部 + 字节 4 的**低 4 位** | `p[3] \| ((p[4] & 0x0F) << 8)` | `:11` ✓ |
| **ch3** | `[47:36]` | 字节 4 的**高 4 位** + 字节 5 全部 | `(p[4] >> 4) \| (p[5] << 4)` | `:12` ✓ |

⭐ **全部吻合** —— 这是"PL 位布局 → PS 解码"一致性的最强证据。
⭐ **注意字节 1 被 ch0（低 4 位）与 ch1（高 4 位）共用**，字节 4 被 ch2/ch3 共用 → **这就是"12 位通道紧邻排列"导致的字节跨界**，也是"每 6 字节一个采样时刻"紧凑布局的代价。

⭐ **三个 API 的分工**：
| API | 用途 | 内存代价 |
|---|---|---|
| `pd_snapshot_unpack` | 解包**整段**快照成 4 条通道数组 | 4 × 520,000 × 2 B = **4.16 MB**（所以实际未用） |
| `pd_snapshot_read_sample` | **只读一个采样时刻**的四通道 | 0（栈上 4 个 u16）→ ⭐ **FFT 走这条路**（1024 点 × 4 通道 = 4096 次调用） |
| `pd_snapshot_measure` | 一次遍历算 min/max/mean | 0 → ⭐ **`ANALYZE SNAP` 命令走这条路** |

⭐ **`validate_raw()` 用"必须是 24 字节的整数倍"作判据**（而不是 6 的倍数）→ **更严格**（同时能发现"块不完整"）✓。

---

## 5. `pd_spectrum.c` —— 定点 1024 点 FFT

### 5.1 三个"省资源/提精度"的技巧

⭐⭐ **技巧 1：只存 1/4 周期的正弦表（257 项）+ 象限映射**（第 12–50 行）
```
12 static const s16 s_sin_quarter_q15[257] = { 0, 201, ..., 32767 };   // sin(pi*n/512), n=0..256
39 static s32 sin_turn_1024(u32 turn)
43     turn &= (PD_FFT_POINTS - 1U);   // 取模 1024（位与，因为 1024 是 2 的幂）
44     quadrant = turn >> 8;           // 0..3
45     position = turn & 0xFFU;        // 0..255
46     if (quadrant == 0U) return  s_sin_quarter_q15[position];
47     if (quadrant == 1U) return  s_sin_quarter_q15[256U - position];
48     if (quadrant == 2U) return -s_sin_quarter_q15[position];
49     return                     -s_sin_quarter_q15[256U - position];
```
**推导验证（象限 1）**：`turn = 256 + p` → 角度 `= π/2 + πp/512`；`sin(π/2 + θ) = cos θ = sin(π/2 − θ) = sin(π(256−p)/512)` ✓ 正好是 `tab[256−p]` ✓。
→ ⭐ **收益**：表从 1024 项降到 257 项（省 1.5 KB）✓

⭐⭐ **技巧 2：Hann 窗复用同一张表**（第 52–56 行）
```
54     /* Periodic Hann: 0.5 * (1 - cos(2*pi*n/N)); appropriate for an N-point FFT. */
55     return (32767 - sin_turn_1024(sample + 256U)) >> 1;
```
**推导**：`cos θ = sin(θ + π/2)` → `sin_turn_1024(sample + 256)` 正好是 `cos(2πn/N)` → 代入 `w[n] = 0.5(1 − cos)` ✓。
→ ⭐ **不需要第二张表。**
→ ⭐ **另一个专业细节**：第 54 行强调是 **periodic** Hann（`2πn/N`）而不是 symmetric（`2πn/(N−1)`）—— **对 N 点 FFT 应该用 periodic 版本**（symmetric 会引入额外窗泄漏）✓。

⭐⭐ **技巧 3：`q15_mul` 用"对称舍入"**（第 32–37 行）
```
34     s64 product = (s64)a * b;
35     if (product >= 0) return (s32)((product + (1LL << 14)) >> 15);
36     return -(s32)(((-product) + (1LL << 14)) >> 15);   // 先取绝对值、加半、右移、再取负
```
- ⭐ **等价于"round half away from zero"（真对称四舍五入）**，比 RTL 侧 `pd_iir_biquad.v:48` 的"加半再截断"更严谨；
- ⭐ **为什么这里要更严谨？** 因为 **FFT 有 10 级蝶形，每级都有乘法与舍入 → 误差会累积**；对称舍入能**消除系统性偏置** ✓ **这是一个经过思考的选择。**

### 5.2 FFT 主体（第 70–95 行）

⭐ **标准的三重循环 radix-2 DIT**：
```
75 for (length = 2; length <= PD_FFT_POINTS; length <<= 1)   // 10 级
76     half = length >> 1;
77     step = PD_FFT_POINTS / length;                        // 旋转因子步长
78     for (group = 0; group < N; group += length)
79         for (k = 0; k < half; ++k) {
80             phase = k * step;
81             wr =  sin_turn_1024(phase + 256U);            // = cos(θ)
82             wi = -sin_turn_1024(phase);                   // = −sin(θ)
83             vr = q15_mul(A.re, wr) - q15_mul(A.im, wi);   // A·W 的实部
84             vi = q15_mul(A.re, wi) + q15_mul(A.im, wr);   // A·W 的虚部
88             data[g+k].re        = (ur + vr) >> 1;         // ★ 每级右移 1 位
89             data[g+k].im        = (ui + vi) >> 1;
90             data[g+k+half].re   = (ur - vr) >> 1;
91             data[g+k+half].im   = (ui - vi) >> 1;
```
⭐ **`step = 1024/length` 的推导**：第 `length` 级要用 `W_length^k = e^{-j2πk/length}`，而 `sin_turn_1024(phase)` 给出 `2π·phase/1024` → 令两者相等 → `phase = k × 1024/length = k × step` ✓（`length` 恒为 2 的幂 → 整除，无精度损失）。

⭐⭐ **第 87–91 行"每级右移 1 位"是极重要的设计决策**，注释（第 87 行）说明了两点：
1. **防溢出**：蝶形的加减让数值**每级最多增大 2 倍**，10 级就是 1024 倍 → **不缩放会在几级内就溢出 Q15**；
2. **归一化**：每级除 2 → 10 级共除 `2¹⁰ = N` → **最终结果正好是"归一化 DFT"（DFT/N）**。
- ⚠️ **代价**：这是"block floating point 的最粗粒度版本"；对"能量集中在少数 bin"的信号会**损失精度**（小信号在前几级就被衰减到 0）。
- ⚠️ **更精细的做法**：检测是否会溢出，**只在必要时**除 2 并记录缩放因子。**本工程选了最简单方案** → 换来确定性的输出范围 ✓ 对"**找峰值 bin**"这个任务够用。
- ⭐ **README 第 73 行诚实说明了精度边界**："输出可用于**同一采样率、同一配置下的相对幅值比较**" ✓。

⭐ **`isqrt_u64()`（第 97–112 行）**：**只用加减与移位的整数平方根**（digit-by-digit），不用浮点、不用除法 → ⭐ **在无 FPU 的 Cortex-A9 上极快** ✓。

⭐ **幅度换算 `amplitude = sqrt(power) × 4`（第 155 行）里的 ×4 从哪来？**
```
① "每级除 2" ⇒ 结果是 DFT/N；
② 实正弦单边谱：|X[k]|/N = A/2 ⇒ 需 ×2 才得到峰值 A；
③ Hann 窗相干增益 = 0.5 ⇒ 再 ×2 补偿；
⇒ 合计 ×4
```
✓ **这不是"魔数"，而是有推导的。**

### 5.3 最强偏移点搜索（`FFT AUTO SNAP` 的核心）

```
184    for (i = 0; i < total_samples; ++i) {
188        delta = sample[ch] >= 2048U ? sample[ch] - 2048U : 2048U - sample[ch];  // |code-2048|
190        if (delta > best_delta) { 记录 i / channel / raw_code }
199    window->start_sample = peak_sample > 512 ? peak_sample - 512 : 0;   // 以它为中心
201    if (start_sample > total_samples - PD_FFT_POINTS) start_sample = total_samples - PD_FFT_POINTS;
```
⭐⭐ **为什么需要它？**（`README.md:90` 说得很好）
> **"这对触发事件不位于快照第一个 1024 点内的情况尤为必要。"**

- **问题**：`FFT SNAP 0` 默认从 `start_sample = 0` 取 1024 点。但**放电脉冲在快照里的位置是随机的**（取决于"事件发生在周期的哪个相位"+"快照从哪个周期边界起算"）→ **若脉冲落在第 300,000 点，从 0 开始的窗口里根本看不到它** → FFT 结果是"背景噪声的频谱"，**完全没有意义**。
- **解法**：先扫全段找"**相对 0x800（2048）偏离最大**"的原始样本，再**以它为中心**取 1024 点 ✓。
- 第 199–202 行：向前退 512 点 + **夹到合法范围** ✓ 边界处理正确。
- ⭐ **代价**：一次全段扫描（520,000 × 4 = 208 万次比较）→ 慢，但这是"一次性命令"，可接受。

### 5.4 `pd_spectrum_self_test()` —— **本工程 PS 侧最漂亮的验证设计**

```
208    static u8 raw[PD_FFT_POINTS * PD_SNAPSHOT_BYTES_PER_SAMPLE];       // 6144 B（静态，避免占栈）
209    static const u32 expected_bin[4] = {37U, 83U, 151U, 255U};
214    for (i = 0; i < 1024; ++i)
215        for (channel = 0; channel < 4; ++channel) {
216            phase = (i * expected_bin[channel]) & 1023U;
217            value[channel] = (u16)(2048 + q15_mul(1024, sin_turn_1024(phase)));   // 幅度 1024
219        raw[i*6+0] = (u8)value[0];
220        raw[i*6+1] = (u8)(((value[0] >> 8) & 0x0FU) | ((value[1] & 0x0FU) << 4)); // ★ 打包！
222        raw[i*6+2] = (u8)(value[1] >> 4);
223        raw[i*6+3] = (u8)value[2];
224        raw[i*6+4] = (u8)(((value[2] >> 8) & 0x0FU) | ((value[3] & 0x0FU) << 4));
225        raw[i*6+5] = (u8)(value[3] >> 4);
226    if (pd_spectrum_analyze(raw, sizeof(raw), 0U, PD_SPECTRUM_DEFAULT_FS_HZ, result) != XST_SUCCESS)
229    for (channel = 0; channel < 4; ++channel)
230        if (result->channel[channel].peak_bin != expected_bin[channel] ||
231            result->channel[channel].amplitude_code < 900U ||
232            result->channel[channel].amplitude_code > 1150U)
233            return XST_FAILURE;
```
⭐⭐ **四个层面的设计都值得学**：

1. **第 214–225 行："自己合成一份符合 PL 内存契约的原始数据"**
   - 在 i 位置放一个频率为 `expected_bin` 的正弦（四通道 37/83/151/255，**互不相同便于区分**），幅度 1024（远小于偏置 2048，不会越界）；
   - ⭐ **第 219–225 行是 `pd_snapshot_unpack.c:6-13` 的精确逆运算**（"按 6 字节打包"）✓ 完全对称。

2. ⭐⭐ **第 226 行："把合成数据喂给真实的分析函数"** —— 走的是**"打包 → 解包 → 去直流 → 加窗 → FFT → 找峰值"这同一条路径**（`README.md:88`）。
   - ⭐ **这是一次"端到端自检（end-to-end self-test）"**，它同时验证了：
     ① 打包/解包一致性（不一致 ⇒ 解出的信号错 ⇒ 峰值 bin 错）；
     ② FFT 正确性（蝶形/旋转因子错 ⇒ bin 错）；
     ③ 去直流与加窗正确性；
     ④ 峰值搜索正确性。
   - ⭐ **一条自检覆盖四个环节，成本极低（6144 字节数据 + 一次分析），收益极高。**

3. ⭐ **第 230–232 行的"判据组合非常专业"**：
   - **`peak_bin` 必须精确相等**（强判据，对"算法错"敏感）；
   - **`amplitude_code` 只要落在 `[900, 1150]`**（宽松判据，对"定点缩放误差"宽容）。
   → **"精确判据 + 宽容判据"的组合，既不会漏报算法错误，也不会因定点误差误报** ✓ 值得抄。

4. ⭐⭐⭐ **纪律："只有返回 `FFT_SELFTEST_PASS` 才进入真实快照分析"**（`README.md:88`）
   - **"先验证工具，再测量"**：如果 FFT 本身有问题，任何对真实数据的分析结论都不可信；
   - ⭐ 这与 `~/.workbuddy/MEMORY.md` 里那条"**判据必须先用独立模型验算再上板**"是同源思想 —— 只是这里的"独立模型"被**内建到固件里**了 ✓ **非常值得学习。**

---

## 6. `pd_tcp_service.c` —— TCP 前端（647 行）

### 6.1 lwIP RAW 模式（无 OS）的三件套

```
641 void pd_tcp_service_poll(void)
642 {
643     if (TcpFastTmrFlag) { tcp_fasttmr(); TcpFastTmrFlag = 0; }   // 每 250 ms
644     if (TcpSlowTmrFlag) { tcp_slowtmr(); TcpSlowTmrFlag = 0; }   // 每 500 ms
645     xemacif_input(&echo_netif);                                  // ★ 把收到的包喂给 lwIP
646     transfer_pump();                                             // 推进大数据传输
647 }
```
⭐ **RAW 模式（无 OS）必须自己周期性调用这三件事**：
| 调用 | 作用 | 不调的后果 |
|---|---|---|
| `tcp_fasttmr()` | 延迟 ACK / 重传等快定时 | 连接可能超时断开 |
| `tcp_slowtmr()` | TCP 状态机推进 / 超时回收 | 连接无法正常关闭 |
| `xemacif_input()` | ⭐ **从网卡把收到的包送进协议栈** | **收不到任何数据（命令无响应）** |

⭐ **第 643–644 行用"中断置标志 + 主循环检查清零"** → ⭐ **"中断只置标志、主循环做冗长工作"的标准做法** ✓。

⭐ **单客户端设计**（`:615-618`）：
```
if (err != ERR_OK || s_client != NULL) { tcp_close(pcb); return ERR_ABRT; }
```
→ **第二个连接会被直接关闭** ✓ 简化并发管理（对"调参 + 下载"这种单用户场景足够）。

### 6.2 `reply()` 与 CRC32

```
48 static int reply(const char *text)
53     len = (u16_t)strlen(text);
55     if (tcp_sndbuf(s_client) < len) return -1;      // 发送缓冲不够 → 放弃
56     err = tcp_write(s_client, text, len, TCP_WRITE_FLAG_COPY);
58     (void)tcp_output(s_client);
```
⭐ **`TCP_WRITE_FLAG_COPY` 不能省**：`text` 大多是**栈上局部数组**（如 `reply_status()` 里的 `char out[256]`）→ **函数返回后栈失效**；不 COPY 则 lwIP 之后发送时读到垃圾 ✓。
⚠️ **第 55 行的"缓冲不够就放弃"是一个丢包取舍**，且**所有调用点都是 `(void)reply(...)`（不检查返回值）** → **丢包静默**。记入 `B09`。

⭐ **`crc32()`（第 62–71 行）**：标准 CRC-32（多项式 `0xEDB88320` = 反射后的 `0x04C11DB7`），`init = 0xFFFFFFFF`、`final = ~crc` ✓ 与 zlib/Python 的 `zlib.crc32` **一致** → **便于用 PC 侧脚本独立校验** ✓ 这是一个很好的"跨平台可验证"设计。

### 6.3 命令集（`execute_command`，第 498–570 行）

| 命令 | 作用 |
|---|---|
| `HELP` | 列出所有命令 |
| `START [n]` | 启动采集（`n` = 包数，0 = 连续；不带参数用 `SET LIMIT` 存的默认值） |
| `STOP` | 请求停止（进入 `STOPPING`，优雅排空） |
| `STATUS` | 状态（含三个硬件状态寄存器 + drops + err） |
| `CONFIG` | API 版本 / 默认包数 / 归档容量 / `GET` 上限 |
| `SET LIMIT n` | 设默认包数（仅空闲时） |
| `SET FFTFS hz` | 设 FFT 采样率（仅空闲时） |
| `RECOVER` | 仅 `FAULT` 态有效，清理未归档槽并复位 DMA |
| `CATALOG` | 返回归档保留序号窗口 `[first, next)` |
| `EVENT n` / `EVENT SEQ n` | 按下标 / 按序号查事件元数据 |
| `SNAP n` / `SNAP SEQ n` | 按下标 / 按序号查快照元数据 |
| `ANALYZE SNAP n\|SEQ s` | ⭐ 快照 `min/max/mean`（**格式一致性门槛**） |
| `FFT CONFIG` / `FFT SELFTEST` | FFT 配置 / 端到端自检 |
| `FFT [AUTO] SNAP n\|SEQ s [start]` | 对快照做 FFT（`AUTO` = 自动选窗口） |
| `GET EVENT\|SNAP n\|SEQ s offset bytes` | ⭐ 分段下载（先回 `DATA V2` 头 + CRC32，再发原始数据） |
| `CLEAR` | 清归档元数据（仅空闲时） |

⭐ **三条设计要点**：
1. ⭐ **所有"重操作"（`ANALYZE`/`FFT`/`GET`）都要求 `state == IDLE`**（第 276/338/420 行）→ **避免与采集争用 DDR 带宽与 CPU** ✓；
2. ⭐ **`GET` 用"先回元数据头（含 CRC32）、再流式发数据"的两段式**（第 487–495 行）→ PC 可以先拿到长度与校验和，**再决定怎么收** ✓；
3. ⭐ **`DATA V2` 里的 `crc32` 让 PC 能独立验证完整性** ✓ 与"判据可验证"的原则一致。

### 6.4 大数据传输的分片泵（`transfer_pump`）

```
112 static void transfer_pump(void)
117     chunk = s_transfer.remaining > PD_TCP_SEND_BYTES ? PD_TCP_SEND_BYTES : remaining;
119     if (tcp_sndbuf(s_client) < chunk) return;         // 缓冲不够 → 本拍不发（下次再试）
120     Xil_DCacheInvalidateRange(s_transfer.addr + s_transfer.offset, chunk);  // ★ 发前 invalidate
121     err = tcp_write(s_client, (const void *)(s_transfer.addr + s_transfer.offset),
122                     chunk, TCP_WRITE_FLAG_COPY);
123     if (err == ERR_MEM) return;                        // 内存不足 → 下次再试
129     s_transfer.offset += chunk;
130     s_transfer.remaining -= chunk;
131     if (s_transfer.remaining == 0U) s_transfer.active = 0U;
132     (void)tcp_output(s_client);
```
⭐⭐ **这是一个"非阻塞分片发送泵"**，三个关键点：
1. **每次只发 1024 字节**（`PD_TCP_SEND_BYTES`）→ 不会长时间占用 CPU ✓；
2. ⭐ **`ERR_MEM` 时安静返回**（不报错、不清状态）→ **下次调用继续发** ✓ **正确处理了"发送缓冲暂时满"这个正常情况**（与"真错误"区分开）；
3. ⭐ **发前 `Xil_DCacheInvalidateRange`**（第 120 行）→ **保证 TCP 栈读到的是内存里的最新数据**（因为归档区可能被 CPU 写过）✓ **cache 管理的第三处正确用法。**
- ⭐ **`s_transfer.active` 在命令解析里被检查**（`:501` `if (s_transfer.active) return;`）→ **传输期间不接受新命令** ✓ "**Never interleave text and raw payload**"（注释原话）—— **防止二进制数据与文本命令交错导致协议错乱** ✓ 非常重要的一条规定。

### 6.5 接收回调（`receive_cb`）

```
572 static err_t receive_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
578     if (p == NULL) {                    // 对端关闭连接
579         tcp_recv(pcb, NULL);
580         if (s_client == pcb) s_client = NULL;
581         s_transfer.active = 0U;         // ★ 清传输状态
582         (void)tcp_close(pcb);
583         return ERR_OK;
584     }
585     tcp_recved(pcb, p->tot_len);        // ★ 必须调用（通知 lwIP 已处理，窗口才能前进）
586     for (i = 0U; i < p->tot_len; ++i) {
587         (void)pbuf_copy_partial(p, &c, 1U, i);
588         if (c == '\r' || c == '\n') { ... 执行命令 ... }
594         else if (c >= ' ' && c <= '~') { ... 存字符 ... }
597     pbuf_free(p);                        // ★ 必须释放，否则内存泄漏
```
⭐ **四个必须做的动作**：
| 动作 | 为什么 |
|---|---|
| `tcp_recved(pcb, p->tot_len)` | ⭐ **通知 lwIP"这些数据我已处理"** → TCP 窗口才能前进；**漏调会导致对端发不出更多数据**（表现为"命令发一半卡住"） |
| 逐字节处理 + 行缓冲 | 把"流"变成"命令行" |
| `pbuf_free(p)` | ⭐ **释放 pbuf**，否则 lwIP 内存池耗尽 |
| `p == NULL` 时清 `s_client` 与 `s_transfer` | 对端关闭时的清理 |

---

## 7. `sw/pd_snapshot_poll.c`（315 行）—— 上板冒烟测试

⭐ **这是一个"独立的最简测试程序"**（自带 `main()`），用途是**脱开 TCP，用最少的代码验证"自动快照 + 四槽轮转 + 槽满拒绝"三条核心契约**。

### 7.1 为什么用轮询不用 GIC

```
4  * This test intentionally polls pd_ddr_0 instead of installing a GIC handler.
5  * The BD already routes pd_ddr_0/irq to IRQ_F2P[2], but the generated BSP does
6  * not expose a named pd_ddr interrupt macro yet. Polling therefore verifies the
7  * complete PL/DDR/AXI-Lite contract without guessing an interrupt ID.
```
⭐ **一句"避免猜中断 ID"的实用判断**：
- BD 里确实把 `pd_ddr_0/irq` 接到了 `IRQ_F2P[2]`（`.bd` 的 `xlconcat_0/In2`）；
- 但 **BSP 没有暴露 `pd_ddr` 的中断 ID 宏** → 要装 GIC handler 就得**手工推算中断号**（脆弱且易错）；
- ⭐ **所以先用轮询把"PL/DDR/AXI-Lite 契约"验证完整，把"中断是否配错"这个变量隔离出去** ✓ **这是一个非常成熟的"控制变量"式调试策略。**

### 7.2 关键实现细节

| 行 | 内容 | 说明 |
|---|---|---|
| `:90-104` | `clear_ring_error_before_arm()` | 写 `DDR_CTRL=0` + `FREEZE_CTRL=2`，然后**轮询确认 `DDR_STATUS[0]` 与 `[5]` 都为 0**；超时 → `fail_stop` |
| `:106-119` | `verify_register_contract()` | ⭐ **读回 `RING_BASE`/`RING_SIZE`/`SNAP_BASE`/`SNAP_SIZE` 并与期望值比对**（`0x10002000`/`0x07FFE000`/`0x18000000`/`0x007FE000`）→ **先证明"跑的是同源的 bit"** |
| `:134-154` | `wait_snapshot_ready_after()` | 等 `SLOT_READY` 且 `SLOT_SEQ > seq_before`；期间检查两类错误 |
| `:182-223` | `verify_slot()` | 校验槽描述符（对齐、范围）、**加锁**、读 `first`/`last` 字、按 `keep_locked` 决定是否释放 |
| `:289-297` | 轮转循环 | ⭐ **`while (VALID_MASK != 0xF)`** —— "保持第一个槽锁住，再消费 3 个事件填满其余槽" |
| `:299-301` | 槽满拒绝 | 再产生一个事件，等 `SLOT_DROPS` 增长 |
| `:307-309` | 通过标志 | 打印 `SNAPSHOT_POLL_PASS slot= seq= err_first= slot_drops= trig_drops=` |

⭐⭐ **第 283–297 行的注释揭示了"四槽轮转"的正确验证方法**：
```
283    /*
284     * Keep the first verified slot LOCKED, then consume three events to fill
285     * the other slots.  The fifth event must be rejected by slot_mgr.  That
286     * is the externally visible no-overwrite contract of this smoke test.
287     */
```
- ⭐ **必须"保持锁定"**！因为 `pd_ddr_slot_mgr` 的策略是"**最低编号 FREE 优先**"（`A09` §3.3）：
  - 若不锁住 slot0，释放它之后下一次分配**又会选 slot0** → **观察不到"轮转"**；
  - 锁住 slot0 后，slot0 不再是 `FREE` → 后续快照依次落到 slot1/2/3 → **"看起来像轮转"** ✓。
- ⭐ **这条经验与本工程一次真实误判直接相关**（`~/.workbuddy/MEMORY.md` 禁错清单第 20 条）："把『最低编号 FREE 槽优先』误报成异常（`ROTATION_PARTIAL`）。**要验四槽轮转必须『加锁后保持锁定』**。"
- ⭐ **一般化教训：判据设计前必须先读被判定对象的策略。** 不读策略就设计判据 → 必然误判。

⭐ **`verify_slot()` 里 `first`/`last` 字的读取（`:210-214`）**：
```
210    Xil_DCacheInvalidateRange((UINTPTR)base, len);   // ★ 读前 invalidate
211    data = (u32 *)(UINTPTR)base;
212    first = data[0];
213    last  = data[(len / sizeof(u32)) - 1U];
```
→ ⭐ **又是 cache 管理的正确用法**（读 PL 写的数据前必须 invalidate）✓ 与 `pd_acquisition_core.c` 一致。

### 7.3 `err_first` 的处理（**对 `DDR_STATUS[5]` 二义性的实测态度**）

```
262    err_first = reg_read(DDR_STATUS) & DDR_STATUS_ERR;
269    if (err_first)
270        xil_printf("WARN: DDR_STATUS[5] set after first snapshot; checking clear semantics\r\n");
273    reg_write(FREEZE_CTRL, 2U);                    // resume
277    /* FREEZE_RESUME is also the documented acknowledge/clear for ring o_err. */
279    err_after_resume = reg_read(DDR_STATUS) & DDR_STATUS_ERR;
280    if (err_after_resume)
281        fail_stop("DDR_STATUS[5] remained set after freeze_resume");
```
⭐⭐ **这段是"如何对待一个有歧义的状态位"的范例**：
- **第一次读到 `[5] = 1` 时**：⭐ **不直接判失败，而是打印 WARN**（因为它可能是那个已知的一次性启动瞬态）；
- **发 `resume` 之后**：⭐ **必须为 0**（因为 `resume` 是它的清除条件）→ **若仍为 1 才判失败** ✓。
- ⭐ **所以这个测试的"通过"并不掩盖错误** —— 它把"歧义位"降级为"警告"，但把"清除机制是否有效"升级为"硬判据" ✓ **这是一个成熟的判据设计。**

---

## 8. 本篇易错点小结（PS 侧）

1. ⭐ **`PD_DDR_RING_ERR`（`1U<<5`）是四路错误的或**（`ring_err|ring_ovf|copy_err|snapshot_cfg_err`），**不是单纯的环形错误**；判"快照是否失败"必须用 `[8]`/`[9]`。
2. ⭐ **读 PL 通过 DMA 写进 DDR 的数据前必须 `Xil_DCacheInvalidateRange`**；不做的后果是"读到旧快照但看起来完全正常"（本工程最可怕的静默错误）。
3. ⭐ **`invalidate` 源 → `memcpy` → `flush` 目标** 是标准三步，顺序不能颠倒（先 invalidate 再 flush 会丢脏数据）。
4. ⭐ **AXI-Lite 写是 posted 的**：写完必须**回读确认**（本工程对"加锁/释放/启动校验"都这么做了）。
5. ⭐ **`configure_capture()` 的最后一步"回读 `TRIG_ENABLED|TRIG_ARMED`"不可省** —— 它把"槽满/未 resume 导致的静默失效"变成明确的启动失败。
6. ⭐ **`archive_new_snapshot()` 的"不消耗 `seq` 就重试"** 处理了"READY 与 VALID 不同拍"的中间态；只有**持久**不匹配才判故障。
7. ⭐ **`XAxiDma_Busy()` 只看 `Idle` 位** → `Halted` 时会永久返回"忙"，把真错误淹没成"超时"。正确判据是读 `DMASR` 检查 `HALTED`/`ERROR_MASK`/`IDLE` 三位。
8. ⭐ **`XAxiDma_IntrAckIrq()` 即使关了中断也要调**（清中断标志位），否则下一次传输可能被拒。
9. ⭐ **S2MM 的 `LENGTH` 寄存器在完成后变成实际传输字节数** —— 这是"变长包"的关键机制。
10. ⭐ **`s_last_slot_sequence` 只在成功归档后才更新** → 失败会自动重试。
11. ⭐ **`pd_acq_recover()` 必须"先 lock 再 release"**（因为只有 `VALID` 能加锁、只有 `LOCKED` 能释放），而清空旧槽是"重新武装触发器"的前置条件。
12. ⭐ **`pd_snapshot_unpack.c:6-13` 的 6 字节解码与 `pd_pack48.v:53-57` 严格互逆**（已逐位验证）——这是跨 PL/PS 契约落地的最好证据。
13. ⭐ **FFT 只用 1/4 正弦表 + 象限映射**；**Hann 窗复用同一张表**（用 `cos θ = sin(θ+π/2)`）；**每级右移 1 位**兼做防溢出与归一化。
14. ⭐ **`q15_mul` 用"对称舍入"**（先取绝对值再加半），比 RTL 侧的"加半再截断"更严谨 —— 因为 FFT 有 10 级，误差会累积。
15. ⭐ **`pd_spectrum_self_test()` 是"端到端自检"**：自己合成符合 PL 布局的数据，穿过同一条分析路径，用"精确 bin + 宽容幅度"双判据；**且必须先它通过才分析真实数据**。
16. ⭐ **lwIP RAW 模式必须周期调用 `tcp_fasttmr`/`tcp_slowtmr`/`xemacif_input`**；`receive_cb` 里 **`tcp_recved` 与 `pbuf_free` 不可漏**。
17. ⭐ **`tcp_write` 必须带 `TCP_WRITE_FLAG_COPY`**（因为源串多在栈上）。
18. ⭐ **大数据传输期间不接受新命令**（`if (s_transfer.active) return;`）——防止文本与二进制交错破坏协议。
19. ⭐ **验证"四槽轮转"必须"加锁后保持锁定"**，否则会因"最低编号 FREE 优先"策略而误判。
20. ⭐ **`pd_acq_poll_hook_t` 回调不可省** —— 否则采集的长等待循环会拖垮网络。

---

**PS 侧旧版服务与调试探针（含 `pd_acquisition_service.c` / `pd_capture_service.c` / `dma_s2mm_probe*.c` / `pd_filter_apply.c` 等）见 `A15_PH_旧版采集服务与调试探针逐行注释.md`。**
