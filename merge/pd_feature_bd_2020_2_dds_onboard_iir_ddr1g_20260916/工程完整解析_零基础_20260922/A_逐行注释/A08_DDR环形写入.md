# A08 · DDR 环形写入管理器 `pd_ddr_ring_wr.v`（443 行）

> 这是本工程**最核心的业务逻辑**：它决定"原始样本以什么节拍、写到 DDR 的哪个地址、什么时候回绕、冻结快照冻结的是哪一段"。基于工具与放大器的工程（BRAM 推断、AXI 握手）相比，这一篇更偏"算法与协议"，但同样是工程里最值得吃透的部分。

---

## 0. 它要解决的四个问题

| 问题 | 解法 | 本篇章节 |
|---|---|---|
| DataMover 不会自动回绕 | 把一次逻辑突发拆成"尾段 + 头段"两条命令 | §3 |
| DataMover 可能"等米下锅"挂死总线 | **字节信用（credit）机制**：只有数据已经躺在 FIFO 里才下命令 | §2.3、§3.1 |
| 冻结快照要抓"完整一个工频周期" | **在两个周期边界各锁一次地址**，差就是长度 | §4 |
| 快照搬运期间新数据可能追上并覆盖 | 环形深度 >> 一个周期 + `o_snap_overrun` 告警 | §4.3 |

---

## 1. 文件头（第 1–63 行）—— 四段设计说明

### 1.1 为什么必须自己做环形指针（第 11–17 行）

```
14 //   AXI DataMover 只认「(起始地址, 字节数)」这一条命令，它按顺序把数据写到
15 //   DA 开始的连续物理地址上，**不会**在到达环形区末尾时自动折回基址。
16 //   因此"回绕"必须由本模块在数据链路层之上处理：把一次逻辑突发拆成
17 //   「尾段 + 头段」两条命令，第二条命令的地址重新指向 RING_BASE。
```
⭐ **这是本模块存在的根本理由**。DataMover 是一个"傻瓜搬运工"：给它 (地址, 长度)，它就把数据搬过去。它**不知道**什么是"环形缓冲区"，也不会在到达区域末尾时跳回起点。所以"环"这个语义必须由上层（本模块）实现。

### 1.2 三条地址对齐铁律（第 19–29 行）

```
22 //   ① 24B 块对齐（契约 §3.3）：RING_BASE % 24 == 0 且 RING_SIZE % 24 == 0。
23 //      这样"第 k 个 192bit 块"永远完整落在 [BASE+24k, BASE+24k+24) 内，
24 //      绝不会横跨回绕点，PS 侧按 24B 对齐读即可无错位解出 4 通道。
25 //   ② 8B AXI 对齐：DataMover 关闭 DRE 时起始地址必须对齐 AXI 数据位宽(8B)。
26 //      24 = 8x3，故满足 ① 即自动满足 ②，无需额外处理。
27 //   ③ 4KB 边界：AXI 协议禁止单次突发跨越 4KB。**这条由 DataMover 内部自动
28 //      拆包处理**（PG022 明确说明），本模块无需干预，但 RING_BASE 取
29 //      LCM(4096,24)=12288 的整数倍，可从源头减少拆分次数。
```
⭐ **第 22–24 行是"为什么要 24 B 对齐"的本质解释**：
- 一个 192 bit 块 = 24 字节。如果块**横跨回绕点**（例如起点在 `BASE+SIZE-8`），那么这条突发的一部分在尾部、一部分在头部 → **一条命令无法表达**，逻辑会复杂到爆炸。
- 只要 `BASE` 和 `SIZE` 都是 24 的整数倍，那么"第 k 个块"的地址 `BASE + 24k` **永远落在 `[BASE, BASE+SIZE)` 内且完整**（因为 `SIZE` 是 24 的倍数，所以最后一个块的起点 `BASE+SIZE-24` 也在区域里）✓。
- ⭐ **这是一个"通过约束取值的粒度来消除边界情况"的经典手法**：不去处理"跨回绕"，而是让"跨回绕"不可能发生（在块粒度上）。

- 第 25–26 行：**24 B 对齐自动蕴含 8 B 对齐**（因为 24 = 8×3）。所以关掉 DRE 的 8 B 约束被免费满足 ✓。
- 第 27–29 行：**4 KB 边界由 DataMover 自动拆包**（PG022 有说明），本模块不用管。但把基址取 4 KB 对齐的整数倍（`LCM(4096,24) = 12288`）能**减少拆分次数**。

### 1.3 字节信用机制（第 31–40 行）

```
32 // 三、字节信用（credit）机制：保证 DataMover 绝不"等米下锅"
33 // ---
34 //   avail_bytes = 已推入 AXI-Stream Data FIFO 但尚未下发命令的字节数。
35 //     生产侧：pd_pack192 每被接收 1 个 64bit beat -> +8（i_beat_en）
36 //     消费侧：本模块每发出 1 条命令        -> -seg_len
37 //   **铁律：只有当 avail_bytes >= BURST_BYTES 时才下发命令。**
38 //   即整段数据已经躺在 FIFO 里才命令 DataMover 来取，DataMover 永远不会因为
39 //   上游供不上而挂住 AXI 总线（那会拖死整条 HP 通道）。
40 //   推论：FIFO 深度必须 >= BURST_BYTES，本设计 512x8B=4096B > 1536B，余量充足。
```
⭐⭐ **这是本模块最重要的机制**，必须彻底理解：

**问题**：AXI DataMover 拿到命令后会**立刻开始从流上取数据**。如果此时 FIFO 里数据不够（上游还在慢慢送），DataMover 就会"卡在半路"——它已经占用了 AXI 总线（发出 AW 地址、等 W 数据），但数据没来，于是：
- **AXI 总线被长期占用** → 拖死整条 HP 通道 → **PS 侧的其他访问（例如 DDR 访问）也变慢**；
- 而且 DataMover 的"未完成事务"会一直挂着，可能超时/报错。

**解法（字节信用）**：维护一个计数器 `avail_bytes` = "已经推进 AXI-Stream Data FIFO、但还没有被命令搬走的字节数"。
- **生产**：`pd_pack192` 每被下游接收 1 个 64 bit beat（= 8 字节），`i_beat_en` 拉高 → `avail_bytes += 8`；
- **消费**：每下发一条命令（长度 `seg_len`）→ `avail_bytes -= seg_len`；
- ⭐ **铁律：只有 `avail_bytes >= BURST_BYTES`（1536）时才下发命令。**

**效果**：整段数据（1536 字节 = 192 个 beat）**已经躺在 FIFO 里**了，才命令 DataMover 来取。DataMover 一开始取就有数据，**永远不会因为上游供不上而挂住总线** ✓。

⭐ **推论（第 40 行）**：**FIFO 深度必须 ≥ `BURST_BYTES`**。
- 本设计 FIFO 是 `axis_dfi_64`：512 × 64 bit = **4096 字节 > 1536 字节** ✓ 余量 2.67 倍。
- ⭐ **这是一个必须记住的"参数联动"**：如果将来把 `BURST_BLOCKS` 加大（例如 256 块 = 6144 字节），**必须同时确认 FIFO 深度 ≥ 6144 字节**，否则命令永远不会下发 → **整个通路静默停摆**。

### 1.4 状态机（第 42–51 行）

```
45 //        ┌──────────┐  avail>=BURST_BYTES 且 acq_en   ┌──────────┐
46 //        │  S_IDLE  │ ──────────────────────────────> │  S_CMD   │
47 //        │ (攒数据) │                                 │(发命令)  │
48 //        └──────────┘ <────────────────────────────── └──────────┘
49 //                        cmd_tready：指针前进/回绕、扣减信用
50 //   注意：命令通道只传"地址+长度"，数据由 DataMover 直接从 Stream FIFO 取，
51 //         两者并行。当前 26MSPS 档为 156MB/s，1536B/命令约 101.6k 命令/秒。
```
- **两个状态**：`S_IDLE`（攒数据）↔ `S_CMD`（发命令）。
- ⭐ **第 50–51 行点出一个重要的结构特征**：**命令通道（`m_cmd_*`）与数据通道（AXI-Stream）是分离的**。
  - 本模块只通过**命令通道**告诉 DataMover "去地址 X 搬 1536 字节"；
  - 数据由 DataMover **直接从 Stream FIFO 取**（`s_axis_s2mm_tdata` 连的就是 `axis_dfi_64` 的输出）；
  - 两者**并行**（命令下发不需要等数据，数据也不经过本模块）。
  - ⭐ **这就是为什么需要信用机制**：因为本模块看不见数据流，只能用"beat 计数"来间接知道"数据到哪儿了"。
- **命令频率**：`156 MB/s ÷ 1536 B = 101,562 命令/秒`（注释写"约 101.6k"）✓。也就是每 9.85 µs 一条命令（@130 MHz = 1280 拍一条）。**这个数字说明命令通道压力不大**（每 1280 拍才发一条命令）。

### 1.5 冻结快照的地址锁存（第 53–62 行）

```
56 //   PS 写 FREEZE_TRIG -> 本模块置 freeze_arm
57 //   第 1 个周期边界(i_cycle_start)：锁存 起始块地址  -> freeze_wait_end
58 //   第 2 个周期边界(i_cycle_start)：锁存 结束块地址  -> freeze_done，给出
59 //        FREEZE_BASE / FREEZE_LEN（字节），并触发 pd_ddr_snap_copy 搬快照区。
60 //   **采集环路不中断**：本模块只锁存地址，不停止写指针（契约 §1 性能指标）。
61 //   **覆盖保护**：环形深度 344ms >> 1 个工频周期 20ms，快照搬完前不会被冲掉；
62 //       若写指针二次进入冻结区间，o_snap_overrun 拉高上报（契约 风险 3 兜底）。
```
⭐ **这段是本模块第二个核心机制**：
- **触发流程**：PS 写 `FREEZE_TRIG` → `freeze_arm = 1` → 等**第一个** `cycle_start` 锁起始地址 → 等**第二个** `cycle_start` 锁结束地址 → 得到 `[起始, 结束)` 这一段（**恰好一个完整工频周期**）→ 输出 `FREEZE_BASE`/`FREEZE_LEN` → **触发 `pd_ddr_snap_copy` 搬运**。
- ⭐ **"用两个周期边界来界定一个周期"是巧妙的**：它不需要知道周期的长度，只需要"起止都在周期边界上"，差自然就是一个周期 ✓。而且如果工频频率变化（50→60 Hz），这个机制**自动适应**。
- ⭐ **"采集环路不中断"**：本模块**只锁存地址，不停止写指针**。也就是说冻结期间数据仍在写（继续覆盖环形区）。**这是一个刻意的取舍**：不中断采集（满足契约 §1 的性能指标），代价是"冻结的数据可能被后续数据追上覆盖"。
- ⭐ **覆盖保护的两层**：
  1. **靠深度余量**：环形深度 344 ms >> 一个工频周期 20 ms → 快照搬完前不会被冲掉（余量 17 倍）；
  2. **靠告警**：若写指针**二次进入**冻结区间（`wr_off` 落在 `[f_start_off, f_end_off)` 内），`o_snap_overrun` 拉高上报。

---

## 2. 参数、端口与参数自检（第 66–157 行）

### 2.1 参数（第 66–74 行）

```
66 module pd_ddr_ring_wr #(
67     parameter integer AXI_ADDR_W   = 32,
68     parameter integer AXI_DATA_W   = 64,
69     parameter [31:0]  RING_BASE    = `DDR_RING_BASE,      // 32'h1000_2000
70     parameter [31:0]  RING_SIZE    = `DDR_RING_SIZE,      // 32'h07FF_E000
71     parameter integer BURST_BLOCKS = `DDR_BURST_BLOCKS,   // 64 块 = 1536 B
72     parameter integer AVAIL_W      = 20,                  // 信用计数器位宽(1MB)
73     parameter [31:0]  OVF_THRESH   = 3072                 // 高水位告警(Bytes)
74 )(
```
- `RING_BASE`/`RING_SIZE`/`BURST_BLOCKS` 都来自 `pd_ddr_defines.vh` 的宏，但**又做成了 parameter** —— ⭐ 这样做的意义是：**BD 里可以覆盖它们**（虽然实际没有覆盖）。**这是"默认值来自宏、但可被上层覆盖"的灵活设计。**
- `AVAIL_W = 20` → 信用计数器最大 `2²⁰ = 1,048,576` 字节 = **1 MB**。
  - ⭐ **为什么是 20 位？** 因为需要能表示"FIFO 满时的字节数"（4096）+ 累积误差余量。20 位（1 MB）远远够用 ✓。
  - ⚠️ **但注意"信用计数器位宽不足会怎样"**：若 `avail_bytes` 溢出回绕，会从 1 MB 掉回 0 → 突然"看起来没有数据" → **停发命令**（保守，不会出错）。所以位宽不足的后果是"性能下降"而不是"功能错误" ✓ 安全。
- `OVF_THRESH = 3072`：**高水位告警阈值**。当 `avail_bytes >= 3072`（= 2 个突发）时拉高 `o_hiwm`，提示"FIFO 快满了，需要注意带宽"。**这是"预警"而不是"错误"**。

### 2.2 端口（第 75–130 行）

```
79     input  wire                 i_acq_en,        // 采集使能（PS 经 AXI-Lite 下发）
84     input  wire                 i_beat_en,
87     input  wire                 i_up_ovf,        // 上游溢出指示
90     output wire [`DM_CMD_W-1:0] m_cmd_tdata,
91     output reg                  m_cmd_tvalid,
92     input  wire                 m_cmd_tready,
95     input  wire [7:0]           s_sts_tdata,
96     input  wire                 s_sts_tvalid,
97     output wire                 s_sts_tready,
98     input  wire                 i_s2mm_err,      // DataMover 硬错误指示
101    input  wire                 i_cycle_start,   // 工频周期边界
102    input  wire                 i_freeze_trig,   // 冻结触发 (W1P 脉冲)
103    input  wire                 i_freeze_resume, // 恢复 (W1P 脉冲)
```
端口分四组（采集控制、数据节拍、命令/状态通道、冻结控制），逐条：
- `i_acq_en`：**采集使能**。⚠️ 注意它**只门控"发命令"**（第 285 行），**不门控"信用累计"**（第 260 行）。**这个区别是"`ring_ovf` 一次性误报"的根因** —— 详见 §5.3。
- `i_beat_en`：来自 `pd_pack192` 的 `o_beat_en`，**生产侧信用**。
- `i_up_ovf`：上游溢出（异步 FIFO 满 / Data FIFO 反压导致丢样本）。
- `m_cmd_tdata/tvalid/tready`：**DataMover S2MM 命令通道**（本模块是主）。
- `s_sts_tdata/tvalid/tready`：**状态通道**（DataMover 是主，本模块是从）。注释第 220 行 `assign s_sts_tready = 1'b1;` —— **恒接收状态字，避免 IP 侧堵塞**。
- `i_cycle_start`：工频周期边界（来自 `pd_sync_pulse` 的 `cycle_start`，经 BD 连到 `pd_ddr_0/cycle_start`）。
- `i_freeze_trig`/`i_freeze_resume`：**W1P 脉冲**（冻结/恢复）。

```
106    output reg  [31:0]          o_wr_addr,       // 当前写绝对地址 = BASE + off
107    output reg  [31:0]          o_wr_off,        // 环内字节偏移
108    output reg  [31:0]          o_blk_idx,       // 当前块号 (off/24)
109    output reg  [31:0]          o_wrap_cnt,      // 回绕次数
110    output reg  [31:0]          o_cmd_cnt,       // 已下发命令数
111    output reg  [63:0]          o_wr_bytes,      // 累计写入字节数 (64bit 不溢出)
112    output wire [AVAIL_W-1:0]   o_avail_bytes,   // 当前信用
113    output reg                  o_hiwm,          // 高水位
114    output reg                  o_ovf,           // 采集侧丢数
117    output reg                  o_freeze_done,   // FREEZE_CTRL[8]
118    output reg  [31:0]          o_freeze_base,
119    output reg  [31:0]          o_freeze_len,
120    output reg                  o_snap_overrun,
123    output reg  [31:0]          o_last_btt,
124    output reg  [7:0]           o_sts_tdata,
125    output reg                  o_err,
126    output reg  [1:0]           o_state,
129    output wire                 o_write_idle
```
⭐ **这些输出是 PS 侧观测环形状态的窗口**（多数映射到 `pd_ddr_axil` 的寄存器，见 A11）：

| 输出 | 含义 | 用途 |
|---|---|---|
| `o_wr_addr` | 当前写绝对地址 | 诊断 |
| `o_wr_off` | 环内字节偏移 | **`RING_WR_PTR`（`0x10`）**，PS 用它验证"环写是否在推进" |
| `o_blk_idx` | 当前块号（`off/24`） | ⚠️ **在顶层悬空**，且因悬空会被综合优化掉 |
| `o_wrap_cnt` | 回绕次数 | **`RING_WRAP`（`0x14`）** |
| `o_cmd_cnt` | 已下发命令数 | **`CMD_CNT`（`0x38`）** |
| `o_wr_bytes` | 累计写入字节数（**64 位不溢出**） | **`WR_BYTES_LO/HI`（`0x18/0x1C`）**，与 `CMD_CNT × 1536` 对账 |
| `o_avail_bytes` | 当前信用 | **`AVAIL_BYTES`（`0x3C`）** |
| `o_hiwm` | 高水位 | `DDR_STATUS[2]` |
| `o_ovf` | 采集侧丢数（**粘滞**） | `DDR_STATUS[5]` 的贡献项之一 |
| `o_freeze_done` | 冻结完成 | `FREEZE_CTRL[8]`（只读位） |
| `o_freeze_base`/`o_freeze_len` | 冻结段的基址与长度 | `FREEZE_BASE_LO`（`0x28`）/`FREEZE_LEN`（`0x30`） |
| `o_snap_overrun` | 冻结区被追上 | `DDR_STATUS[4]` |
| `o_last_btt` | 最近一条命令的 BTT | ⚠️ **顶层悬空** |
| `o_sts_tdata` | 最近一条状态字 | ⚠️ **顶层悬空** |
| `o_err` | 错误汇总（**粘滞**） | `DDR_STATUS[5]` 的贡献项之一 |
| `o_state` | 状态机状态（2 位） | `dbg_ddr[15:14]` |
| `o_write_idle` | **已接受的写命令全部完成** | ⭐ 快照搬运必须等的条件 |

⭐ **`o_write_idle`（第 127–129 行注释）极其重要**：
```
127    // 已接受但尚未收到 DataMover 状态的写命令全部完成。
128    // 冻结快照必须等待该条件，不能在 DDR 尾部突发尚未落地时开始读回。
129    output wire                 o_write_idle
```
- **为什么必须等？** 数据从 Stream FIFO 进入 DataMover、再写到 DDR，是**异步**的（DataMover 内部有流水与写缓冲）。
- 如果 `freeze_done` 一置位就开始搬运快照（读环形区），**而最后几条写命令的数据还"DDR 在路上"** → **读回来的快照尾部是旧数据** → 快照内容错。
- ⭐ **所以 `pd_ddr_snap_copy` 的启动条件里包含 `ring_write_idle`**（见 A09/A11）。**这是"读写同一区域时必须等写落地"的典型工程考虑。**

### 2.3 参数自检（第 139–157 行）

```
139    // synthesis translate_off
140    initial begin
141        if (RING_BASE % BLK_BYTES != 0)
142            $display("[ERROR] pd_ddr_ring_wr: RING_BASE=%h 不是 %0d 字节对齐（契约 §3.3）", ...);
144        if (RING_SIZE % BLK_BYTES != 0)
145            $display("[ERROR] pd_ddr_ring_wr: RING_SIZE=%h 不是 %0d 字节对齐，回绕后块会跨边界", ...);
147        if (BURST_BYTES % BLK_BYTES != 0)
148            $display("[ERROR] pd_ddr_ring_wr: BURST_BYTES=%0d 不是 %0d 的整数倍", ...);
150        if (BURST_BYTES / AXI_BYTES > 256)
151            $display("[ERROR] pd_ddr_ring_wr: 突发 %0d beat 超过 DataMover 单次上限 256", ...);
153        else
154            $display("[INFO ] pd_ddr_ring_wr: RING_BASE=%h SIZE=%h BURST=%0dB(%0d beat)", ...);
156    end
157    // synthesis translate_on
```
⭐ **`// synthesis translate_off` / `// synthesis translate_on` 是一对综合指令**：夹在中间的内容**只在仿真时生效，综合时被完全忽略**。
- **为什么这里用指令而不是 `initial` 里放 `$display`？** 因为 `initial` 本身综合时也会被忽略，但**有些工具会对 `initial` 里的复杂逻辑报 warning**。用 `translate_off` 明确标注"这段不是硬件"，更干净 ✓。
- ⭐ **四条自检覆盖了三条对齐铁律 + 一条 DataMover 限制**：
  1. `RING_BASE % 24 == 0`（铁律 ① 的基址部分）；
  2. `RING_SIZE % 24 == 0`（铁律 ① 的长度部分）；
  3. `BURST_BYTES % 24 == 0`（保证命令的起止地址都在块边界）；
  4. `BURST_BYTES / 8 <= 256`（**DataMover 单次 AXI4 突发上限 256 beat**）。
- ⭐ **这四条正好对应"改参数时必须同时成立"的约束**。如果改 `BURST_BLOCKS = 128`：`128 × 24 = 3072`，`3072 % 24 = 0` ✓，`3072/8 = 384 > 256` ✗ **会触发第 4 条错误**！所以 **`BURST_BLOCKS` 最大只能到 `256 × 8 / 24 = 85.33` → 最大 85 块**。
  - ⚠️ **但 85 × 24 = 2040，不是 8 的倍数？2040/8 = 255 ✓ 可以。所以 85 是上限（实测边界）。** 这就是"为什么选 64 而不是更大的数"的量化理由 —— **64 是一个"远小于上限、但对齐性良好"的安全选择**。
- ⚠️ **【观察】自检用的是 `$display` 而不是 `$error`** —— `$display` **不会中止仿真**，只是打印。所以如果参数配错，仿真会**继续跑下去，产生难以理解的行为**。**对比 `pd_dds_adc_source.v:83-88` 用的是 `$error`（硬失败）**。⚠️ **参数自检应该用 `$error`**（B09 已记录这个不一致）。

---

## 3. 状态机与命令组装（第 159–340 行）

### 3.1 状态定义与内部寄存器（第 162–193 行）

```
162    localparam [1:0] S_IDLE = 2'd0;
163    localparam [1:0] S_CMD  = 2'd1;
165    reg [31:0]          wr_off;        // 环内字节偏移 [0, RING_SIZE)
166    reg [31:0]          seg_len_r;     // 本条命令长度
167    reg [31:0]          cmd_addr_r;    // 本条命令的绝对地址
168    reg [AVAIL_W-1:0]   avail_bytes;   // 字节信用
169    reg [31:0]          ring_remain_r; // 本条命令起点到环尾的剩余字节
170    reg [7:0]           wr_inflight;   // 已被 DataMover 接受、尚未完成的写命令数
```
- `wr_off`：**环内偏移**（0 ~ `RING_SIZE`）。这是环形缓冲区的"写指针"。
- `seg_len_r`/`cmd_addr_r`/`ring_remain_r`：**"本条命令"的三件套**（在 `S_IDLE` 决定、在 `S_CMD` 使用）。
- ⭐ `wr_inflight`：**"已发出但还没收到状态字的命令数"**。8 位可表示 0…255。
  - **为什么需要它？** DataMover 的状态字是**异步回来的**（数据搬完才回）。所以不能"发一条命令就认为写完了"。**用 inflight 计数追踪"有多少写还没落地"** → `o_write_idle = (wr_inflight == 0) && !m_cmd_tvalid` ✓。
  - ⚠️ **8 位够吗？** 命令间隔约 1280 拍，DataMover 搬 1536 字节（192 beat）大约需要 192 拍 + 延迟。所以同时在飞的命令最多 1–2 条 ✓ 8 位远远够。
  - ⚠️ **【观察 · 潜在风险】** 如果 `wr_inflight` 溢出回绕（255 → 0），`o_write_idle` 会**误报"空闲"** → 快照搬运在写未落地时开始 → 快照内容错。**但需要 255 条命令同时在飞（约 390 KB 数据未落地），而 FIFO 只有 4 KB → 不可能发生** ✓ 安全。

```
175    reg                 freeze_commit_wait; // 已锁结束，等边界数据真正写入 DDR
176    reg [31:0]          f_start_off;
177    reg [31:0]          f_end_off;
178    reg [63:0]          f_start_total;
179    reg [31:0]          commit_bytes_left; // 周期终点后仍需下发的 24B 对齐尾量
184    reg [1:0]           stream_beat_phase;
185    reg [31:0]          stream_off;
186    reg [63:0]          stream_total;
```
⭐ **第 181–186 行是"精确周期边界"的关键设计**，注释说得很好：

```
181    // 以 3 个 64-bit beat = 1 个 24-byte 原始块跟踪“已进入 Data FIFO”的
182    // 逻辑流位置。它独立于大突发命令指针，因此周期边界不会被 1536B
183    // 命令粒度量化。
```
**问题**：`wr_off`（命令指针）是**按 1536 字节的突发粒度**前进的。但"周期边界"（`cycle_start`）可能落在**突发中间**。如果用 `wr_off` 来锁冻结地址，就会**被量化到 1536 字节的网格上**，误差最大 1536 字节 = 64 个采样时刻 → **相位误差 = 64/520000 × 360° = 0.044°**（不大，但没必要）。

**解法**：**另开一套"精确流位置"追踪器**：
- `stream_beat_phase`（2 位，0–2）：每 3 个 beat 构成一个 24 字节块；
- `stream_off`：块粒度的环内偏移（**只在块完成时前进**）；
- `stream_total`：累计写入的字节数（块粒度）；
- 第 187 行 `stream_block_done = i_beat_en && (stream_beat_phase == 2'd2)`：**第 3 个 beat 到达时，一个块完成**。
- 第 188–193 行：算出"块完成那一刻"的 `stream_off`/`stream_total`（`stream_off_at_edge`/`stream_total_at_edge`）——⭐ **这解决了"周期边界那一拍正好有 beat 到达"的边界情况**：用组合逻辑算出"如果这一拍块完成，位置应该是多少"，然后冻结时用这个值。

⭐⭐ **这是一个"分离『命令粒度』与『数据粒度』"的漂亮设计**：
- 命令粒度（1536 B）→ 决定 DDR 写入的地址与长度；
- 数据粒度（24 B）→ 决定"周期边界"的精确位置；
- 两套独立追踪，各服务一个目的。

### 3.2 命令长度与地址的组合计算（第 195–218 行）

```
198    wire [31:0] ring_remain = RING_SIZE - wr_off;
199    wire [31:0] normal_seg_len = (ring_remain >= BURST_BYTES)
200                               ? BURST_BYTES : ring_remain;
201    wire [31:0] commit_cap_len = (commit_bytes_left < BURST_BYTES)
202                               ? commit_bytes_left : BURST_BYTES;
203    wire [31:0] commit_seg_len = (ring_remain >= commit_cap_len)
204                               ? commit_cap_len : ring_remain;
205    wire [31:0] cmd_addr   = RING_BASE + wr_off;
```
⭐ **这五行就是"回绕拆分"的全部逻辑**：
- 第 198 行 `ring_remain`：**从当前写指针到环尾还剩多少字节**。
- 第 199–200 行 `normal_seg_len`：**正常采集时的命令长度** = `min(BURST_BYTES, ring_remain)`。
  - ⭐ **如果剩余不足一个突发（`ring_remain < 1536`），就只发"剩余这么多"** —— 这一条命令把数据写到环尾，**下一拍 `wr_off` 归 0，下一条命令从 `RING_BASE` 开始** → **这就是"回绕"的实现** ✓。
  - ⭐ **因为 `RING_SIZE % 24 == 0` 且 `BURST_BYTES % 24 == 0`，所以 `ring_remain` 在"不足一个突发"时仍然是 24 的倍数** ✓ 命令的起止地址仍在块边界上。
- 第 201–204 行 `commit_seg_len`：**冻结收尾（commit）时的命令长度**，上限是 `commit_bytes_left`（剩余的收尾字节数）。
- 第 205 行 `cmd_addr = RING_BASE + wr_off`：**绝对地址**。

```
209    assign m_cmd_tdata = {
210        {`DM_RSVD_W{1'b0}},                 // [71:68] RSVD
211        {`DM_TAG_W{1'b0}},                  // [67:64] TAG（不用）
212        cmd_addr_r,                         // [63:32] 目标地址（写 DDR）
213        1'b0,                               // [31]    DRR：关闭 DRE，无需重对齐
214        1'b0,                               // [30]    EOF：不用 TLAST，长度由 BTT 定
215        {`DM_DSA_W{1'b0}},                  // [29:24] DSA
216        1'b1,                               // [23]    TYPE=1 INCR（地址递增）
217        seg_len_r[`DM_BTT_W-1:0]            // [22:0]  BTT
218    };
```
⭐ **命令字组装（72 位）**，与 `pd_ddr_defines.vh:41-43` 的位域一一对应：

| 位段 | 值 | 理由 |
|---|---|---|
| `[71:68]` RSVD | 0 | 保留 |
| `[67:64]` TAG | 0 | 不用 |
| `[63:32]` ADDR | `cmd_addr_r`（**寄存器版**） | 目标 DDR 地址 |
| `[31]` DRR | **0** | 关闭 DRE → 起始地址须 8 B 对齐（已被 24 B 对齐满足） |
| `[30]` EOF | **0** | **不用 TLAST 定长度** → 长度完全由 BTT 决定 |
| `[29:24]` DSA | 0 | 关 DRE 时填 0 |
| `[23]` TYPE | **1** | **INCR（地址递增）** —— 写连续 DDR 必须递增 |
| `[22:0]` BTT | `seg_len_r[22:0]` | 本条命令的字节数（最大 8388607） |

- ⭐ **注意第 212 行用的是 `cmd_addr_r`（寄存器）而不是组合的 `cmd_addr`** —— 因为命令字是**组合输出**，而 `cmd_addr_r` 在 `S_IDLE` 拍就被寄存了，所以在 `S_CMD` 拍输出时地址已经稳定 ✓。**这是"把地址在状态转移时锁存"的标准做法**（避免 `m_cmd_tdata` 的组合路径过长）。
- ⭐ **`EOF = 0` 是一个重要选择**：如果 `EOF = 1`，DataMover 会在最后一个 beat 拉 TLAST；但本设计的 `pd_pack192` 的 `m_axis_tlast` **恒为 0**（见 A05 第 3.4 节），所以 TLAST 机制根本用不上。**长度完全由 BTT 决定**，这样 DataMover 就知道"搬够 BTT 字节就停"，不会等 TLAST 而挂住 ✓。

```
220    assign s_sts_tready = 1'b1;             // 状态通道恒接收，避免 IP 侧堵塞
```
- ⭐ **状态通道恒 ready** —— 因为状态字来自 DataMover，如果本模块不接收，IP 侧会堵塞（可能影响后续命令处理）。**恒 ready 是对的**（状态字很少，不会造成拥塞）。

### 3.3 主状态机（第 233–337 行）

#### 3.3.1 信用更新与 inflight 追踪（第 255–270 行）

```
255            // ---- 3.1 信用更新（生产 +8 / 消费 -seg_len）----
256            // 同一个时钟周期既可能有新 beat 入 FIFO，也可能有 DataMover
257            // 命令被接受。两次非阻塞赋值会让后者覆盖前者，长期会少记 8B/命令，
258            // 最终出现 FIFO 有数据但信用为零的停滞。S_CMD 的 cmd_fire 分支
259            // 必须合并这两个变化。
260            if (i_beat_en)
261                avail_bytes <= avail_bytes + AXI_BYTES[AVAIL_W-1:0];
```
⭐⭐ **第 256–259 行是**一个非常经典的 bug 与它的说明**，务必理解：
- **场景**：某一拍既 `i_beat_en = 1`（生产 +8）**又** `cmd_fire = 1`（消费 −1536）；
- **错误写法**：
  ```
  if (i_beat_en)  avail_bytes <= avail_bytes + 8;      // 第一次赋值
  if (cmd_fire)   avail_bytes <= avail_bytes - seg_len; // 第二次赋值
  ```
  - 两次都是"读旧值 + 偏移"，**第二次赋值会完全覆盖第一次** → 那一拍**少记了 8 字节的生产**。
  - ⭐ **长期累积**：每次"生产与消费同拍"都少记 8 字节 → 信用计数**系统性偏低** → 最终出现"**FIFO 里明明有数据，但信用不够 `BURST_BYTES`，命令永远发不出去**"的**停滞**。
  - ⭐ **这个 bug 极难发现**：它只在"生产与消费同拍"时触发，而且是**缓慢累积**的（每 1280 拍才可能触发一次）。表现是"跑一段时间后突然不写 DDR 了"。
- **正确写法**（见第 304–306 行）：**在 `cmd_fire` 分支里把两个变化合并成一次赋值**：
  ```
  avail_bytes <= avail_bytes
                + (i_beat_en ? AXI_BYTES : 0)
                - seg_len_r;
  ```
  ✓ 这样就"净变化"了一次。
- ⭐ **可复用结论**：**同一变量在同一拍既有"无条件 +"又有"有条件 −"时，必须合并成一次赋值**，否则会静默丢失增量。**这是一个"非阻塞赋值不会自动累加"的直接后果。**

- 第 260–261 行：`if (i_beat_en)` 时 `avail_bytes += 8` —— 这是**"非 cmd_fire 拍"的生产路径**（cmd_fire 拍由第 304–306 行处理）。

⭐ **注意这里就是前面提到的"`i_acq_en` 不门控信用累计"**：
- 第 260 行**没有 `i_acq_en` 条件** → **即使采集关闭（`acq_en = 0`），只要上游还在送 beat，信用就继续累加**。
- ⭐ **这是 `DDR_STATUS[5]` 一次性误报的根因**（详见 §5.3）。**当前 `i_acq_en = 0` 时 `pd_pack192` 仍可能继续往 FIFO 里推数据**（因为 `fifo_rd_en` 只依赖 `pk_s48_ready` 与 FIFO 非空，不看 `acq_en`）。

```
263            // 命令/状态可同拍出现，计数必须保持净变化。状态在没有已发命令
264            // 时忽略，防止第三方 IP 复位边沿的孤立状态字导致下溢。
265            case ({cmd_fire, sts_fire})
266                2'b10: wr_inflight <= wr_inflight + 8'd1;
267                2'b01: if (wr_inflight != 8'd0)
268                           wr_inflight <= wr_inflight - 8'd1;
269                default: ;
270            endcase
```
⭐ **第 265–270 行用 `case ({cmd_fire, sts_fire})` 显式列出三种情况**，与 `pd_axis_fifo` 里 `num` 的三态更新是同一个手法（**比写 `wr_inflight <= wr_inflight + cmd_fire - sts_fire` 更清晰、更安全**）。
- ⭐ **`2'b01` 分支加了 `if (wr_inflight != 0)` 保护**（第 267 行）：防止"没有已发命令却收到状态字"导致**下溢**（0 − 1 = 255）。
  - **为什么会收到"孤立状态字"？** 注释第 264 行说"**第三方 IP 复位边沿的孤立状态字**" —— 例如 DataMover 被复位时可能吐出一个残留状态字。**加保护是必要的** ✓。
  - ⭐ **对比**：如果用 `wr_inflight <= wr_inflight - sts_fire`，下溢会让 `o_write_idle` 误判（255 ≠ 0，反而更"忙"）—— 但更糟的情况是下溢到 0 后"看起来空闲"。**所以保护是必要的。**

#### 3.3.2 `S_IDLE`（第 274–294 行）

```
274                S_IDLE: begin
275                    m_cmd_tvalid <= 1'b0;
278                    // 冻结尾量在周期终点已经全部进入 FIFO，可直接按寄存的剩余
279                    // 字节数规划命令；避免把 64-bit 累计写计数的减法串到发命令
280                    // 使能路径。正常采集仍攒够一条命令后才发，防止 DataMover 饿死。
281                    if (freeze_commit_wait && (commit_seg_len != 0)) begin
282                        cmd_addr_r    <= cmd_addr;
283                        seg_len_r     <= commit_seg_len;
284                        ring_remain_r <= ring_remain;
285                        m_cmd_tvalid  <= 1'b1;
286                        o_state       <= S_CMD;
287                    end else if (!freeze_commit_wait && i_acq_en &&
288                                 (normal_seg_len != 0) &&
289                                 (avail_bytes >= normal_seg_len[AVAIL_W-1:0])) begin
290                        cmd_addr_r    <= cmd_addr;
291                        seg_len_r     <= normal_seg_len;
292                        ring_remain_r <= ring_remain;
293                        m_cmd_tvalid  <= 1'b1;
294                        o_state       <= S_CMD;
```
⭐ **两个发命令的分支，条件不同**：

| 分支 | 条件 | 是否检查信用 |
|---|---|---|
| **冻结收尾**（`freeze_commit_wait`） | `commit_seg_len != 0` | ❌ **不检查信用** |
| **正常采集** | `i_acq_en && normal_seg_len != 0 && avail_bytes >= normal_seg_len` | ✅ **检查信用** |

⭐ **为什么冻结收尾不检查信用？** 注释第 278–280 行解释了：
- 冻结收尾时，**数据已经全部进入 FIFO 了**（因为 `freeze_commit_wait` 的进入条件是"周期终点已经锁存"，而此时 `stream_total` 已经记录了所有进入 FIFO 的字节）；
- 所以"信用必然够"，不需要检查；
- ⭐ **更重要的是**：`commit_bytes_left` 是从 `freeze_tail_at_edge` 算出来的（一个 64 位减法的结果），如果把它串进"发命令使能路径"，会形成**长组合路径**。**用"不检查信用"来避免这条路径** ✓ 这是一个"为了时序而放宽条件"的取舍。
- ⚠️ **【观察】代价**：如果冻结收尾时的数据**还没完全进 FIFO**（例如上游溢出被丢弃），这条命令会让 DataMover 去取**不存在的数据** → 可能挂住或产生错误。**但此时 `i_up_ovf` 已经会置位 `o_ovf`，告警是有的** ✓ 有余量。

#### 3.3.3 `S_CMD`（第 297–317 行）

```
297                S_CMD: begin
298                    m_cmd_tvalid <= 1'b1;
299                    if (cmd_fire) begin
300                        m_cmd_tvalid <= 1'b0;
301                        o_last_btt   <= seg_len_r;
302                        o_cmd_cnt    <= o_cmd_cnt + 32'd1;
303                        o_wr_bytes   <= o_wr_bytes + {32'd0, seg_len_r};
304                        avail_bytes  <= avail_bytes
305                                      + (i_beat_en ? AXI_BYTES[AVAIL_W-1:0] : {AVAIL_W{1'b0}})
306                                      - seg_len_r[AVAIL_W-1:0];
308                        // ---- 指针推进与回绕 ----
309                        if (seg_len_r >= ring_remain_r) begin
310                            wr_off      <= 32'd0;                   // 回绕到环首
311                            o_wrap_cnt  <= o_wrap_cnt + 32'd1;
312                        end else begin
313                            wr_off      <= wr_off + seg_len_r;
314                        end
315                        o_state <= S_IDLE;
316                    end
317                end
```
- 第 298 行 `m_cmd_tvalid <= 1'b1`：**在 `S_CMD` 状态保持 valid**（因为上一步已经置位，这里是为了"若上一拍没握手成功就继续保持"）。
- 第 299 行 `if (cmd_fire)`：**只在真正握手成功（`tvalid & tready`）时才推进**。这是标准 AXI-Stream 握手 ✓。
- ⭐ **第 309–314 行：回绕判据是 `seg_len_r >= ring_remain_r`**。
  - `ring_remain_r` 是"这条命令发出时的起点到环尾的剩余字节"；
  - 若 `seg_len_r >= ring_remain_r`，说明**这条命令恰好（或超过）到达环尾** → `wr_off = 0`（回绕）✓；
  - 否则 `wr_off += seg_len_r`。
  - ⭐ **为什么用 `>=` 而不是 `==`？** 因为 `seg_len_r` 在"正常突发"时等于 `BURST_BYTES`（1536），而 `ring_remain_r` 在"剩余不足一个突发"时小于 1536 → 此时 `seg_len_r = ring_remain_r`（相等）→ 回绕 ✓。**用 `>=` 是"相等或超过"的稳妥写法**（虽然不会"超过"，因为 `seg_len_r` 取的是 `min`）。
  - ⚠️ **【观察 · 已知的时序问题（B09 已记录）】** 第 198 行的 `ring_remain = RING_SIZE - wr_off` 是**组合的 32 位减法**，会综合成 11 级 CARRY4。它与 `normal_seg_len`/`commit_seg_len` 的比较选择、状态机的下一状态判断**挤在同一拍**，且**直驱目的寄存器的 CE（时钟使能）** → 这是 `timing_summary_routed.rpt` 里那条 **WNS −0.054 ns、19 级、布线占 54.9%** 的关键路径。
    - ⭐ **修法**（在方案文档里已提出）：新增 `reg ring_remain_q`，**连续寄存** `RING_SIZE − wr_off`，第 198 行改为 `wire ring_remain = ring_remain_q;`。因为 `wr_off` 每拍只可能不变或加 `seg_len_r`，所以 `ring_remain_q` 也可以每拍更新（`ring_remain_q <= RING_SIZE - wr_off_next`），这样把减法从"关键路径"移到"有富裕的路径上"。
    - ⚠️ **改这里必须做仿真回归**：跨 1536 临界拍、回绕前后各一拍。
- 第 303 行 `o_wr_bytes <= o_wr_bytes + {32'd0, seg_len_r}`：**64 位累加**（`{32'd0, seg_len_r}` 把 32 位零扩展成 64 位）✓ 永不溢出。
- 第 304–306 行：**信用净变化**（见 3.3.1 的详细解释）。

#### 3.3.4 `o_blk_idx` 的除法（第 324 行）

```
322            o_wr_off  <= wr_off;
323            o_wr_addr <= RING_BASE + wr_off;
324            o_blk_idx <= wr_off / BLK_BYTES;
325            o_hiwm    <= (avail_bytes >= OVF_THRESH[AVAIL_W-1:0]);
```
⚠️ **`o_blk_idx <= wr_off / 24`** —— 这是一个**除以常数 24**。
- 综合器会把它变成"乘 1/24 的近似"或一个除法网络。24 = 8 × 3，所以可以写成 `(wr_off >> 3) / 3`，仍有除 3。
- ⭐ **【观察】** `pd_ddr_wr_top.v:442` 把 `.o_blk_idx(o_blk_idx)` 接成**悬空**（`.o_blk_idx ()`），所以**这个除法器被综合优化掉了**（因为输出没人用）。所以"本轮不动"（时序文档的结论）。
- ⭐ **但如果将来有人把 `o_blk_idx` 引出来**，这个除法器会**重新出现**，可能成为新的时序瓶颈。**记入 B09。**

---

## 4. 冻结快照：周期边界锁存（第 342–414 行）

### 4.1 状态变量与触发/恢复（第 345–375 行）

```
347            freeze_arm     <= 1'b0;
348            freeze_wait    <= 1'b0;
349            freeze_commit_wait <= 1'b0;
...
360            if (i_freeze_trig) begin
361                freeze_arm     <= 1'b1;
362                freeze_wait    <= 1'b0;
363                freeze_commit_wait <= 1'b0;
364                commit_bytes_left <= 32'd0;
365                o_freeze_done  <= 1'b0;
366                o_snap_overrun <= 1'b0;
367            end
368            if (i_freeze_resume) begin
369                freeze_arm     <= 1'b0;
370                freeze_wait    <= 1'b0;
371                freeze_commit_wait <= 1'b0;
372                commit_bytes_left <= 32'd0;
373                o_freeze_done  <= 1'b0;
374                o_snap_overrun <= 1'b0;
375            end
```
⭐ **四个状态的语义**：

| 变量 | 含义 |
|---|---|
| `freeze_arm` | 已收到触发，等**第一个**周期边界 |
| `freeze_wait` | 已锁起始，等**第二个**周期边界 |
| `freeze_commit_wait` | 已锁结束，**等边界数据真正写入 DDR** |
| `o_freeze_done` | 真正完成（可给 PS 读） |

- ⭐ **第 360–367 行（触发）与第 368–375 行（恢复）是"对称的软复位"**：两者都把三个状态标志清 0、清 `commit_bytes_left`、清 `o_freeze_done`、清 `o_snap_overrun`。
  - **含义**：**收到新的 `trig` 时会"重启"整个冻结流程**（丢弃上一次未完成的状态）；
  - **收到 `resume` 时"取消并释放"** ✓。
- ⭐ **注意"清 `o_freeze_done`"** 这一点很重要：`o_freeze_done` 是给 PS 的"完成标志"，如果不清，PS 会一直以为"有完成快照"。**清它等价于"告诉 PS：上一次的快照已被我作废"** ✓。

### 4.2 两个周期边界的锁存（第 377–396 行）

```
377            // 第 1 个周期边界：锁存起始地址（此时起就是一个完整周期的开端）
378            if (freeze_arm && i_cycle_start) begin
379                f_start_off   <= stream_off_at_edge;
380                f_start_total <= stream_total_at_edge;
381                freeze_arm    <= 1'b0;
382                freeze_wait   <= 1'b1;
383            end
384            // 第 2 个周期边界：锁存逻辑结束块，随后把不足一个常规突发的
385            // 尾部用 24B 整数倍短命令冲入 DDR。
386            else if (freeze_wait && i_cycle_start) begin
387                f_end_off     <= stream_off_at_edge;
388                commit_bytes_left <= freeze_tail_at_edge[31:0];
389                freeze_wait   <= 1'b0;
390                freeze_commit_wait <= 1'b1;
391                o_freeze_base <= RING_BASE + f_start_off;
392                o_freeze_len  <= stream_total_at_edge - f_start_total;
393            end else if (freeze_commit_wait && cmd_fire) begin
394                commit_bytes_left <= (commit_bytes_left > seg_len_r)
395                                   ? (commit_bytes_left - seg_len_r) : 32'd0;
396            end
```
⭐ **逐条拆解**：

- **第 1 个边界（378–383 行）**：
  - 锁 `f_start_off`（起始块粒度的环内偏移）与 `f_start_total`（起始的累计字节数）；
  - 状态从 `freeze_arm` 转到 `freeze_wait`。
  - ⭐ **用 `stream_off_at_edge`/`stream_total_at_edge`**（而不是 `stream_off`/`stream_total`）→ **解决了"边界拍正好有 beat 到达"的竞态**（见 §3.1）。

- **第 2 个边界（386–392 行）**：
  - 锁 `f_end_off`（结束位置）；
  - ⭐ **计算 `commit_bytes_left`（`freeze_tail_at_edge`）**：**"已经进入 FIFO 但还没被命令搬走"的字节数**。
    - 定义见第 229–231 行：
      ```
      wire [63:0] issued_bytes_at_edge = o_wr_bytes + (cmd_fire ? seg_len_r : 0);
      wire [63:0] freeze_tail_at_edge =
                        (stream_total_at_edge > issued_bytes_at_edge)
                        ? (stream_total_at_edge - issued_bytes_at_edge) : 0;
      ```
    - 即"已进 FIFO 的字节数（`stream_total`）− 已下发命令的字节数（`o_wr_bytes`）"= **尾部余量**。
    - ⭐ **为什么需要它？** 因为命令是 1536 字节粒度的，冻结结束点可能落在某个突发的中间 → **最后一段数据虽然进了 FIFO，但没有对应的命令去搬它** → **必须补发一条"短命令"**把这个尾巴冲进 DDR。
  - `freeze_commit_wait = 1`：进入"收尾"阶段；
  - ⭐ **`o_freeze_base <= RING_BASE + f_start_off`**：冻结段的**绝对地址**；
  - ⭐ **`o_freeze_len <= stream_total_at_edge - f_start_total`**：冻结段的**长度**（字节）。
    - ⭐ **注意这里是"两个周期的累计字节数之差"**，所以 `FREEZE_LEN` **就是"一个工频周期的字节数"**（不是固定的 3,120,000，而是实际测出来的）。**这是一个很好的设计**：如果工频频率是 60 Hz，`FREEZE_LEN` 会自动变成约 2,600,000 ✓。

- **第 393–396 行**：每发出一条命令就减少 `commit_bytes_left`，直到 0。

```
398            // done 表示周期末尾已经真实写入 DDR，而不仅是描述符已锁存。
399            if (freeze_commit_wait && !i_freeze_resume && !i_freeze_trig &&
400                (commit_bytes_left == 0) && o_write_idle) begin
401                freeze_commit_wait <= 1'b0;
402                o_freeze_done <= 1'b1;
403            end
```
⭐⭐ **第 398 行的注释点出了一个极其重要的语义区别**：
> **`done` 表示"周期末尾已经真实写入 DDR"，而不仅是"描述符已锁存"。**

- **完成条件 = 四个**：
  1. 处于 `freeze_commit_wait`；
  2. 没有同时收到 `resume`/`trig`（避免和新请求冲突）；
  3. `commit_bytes_left == 0`（所有尾量都下发了）；
  4. ⭐ **`o_write_idle`（所有写命令都已收到完成状态）**。
- ⭐ **条件 4 是关键的**：如果只判 `commit_bytes_left == 0`，那只证明"命令都发出去了"，**不证明"DDR 里的数据都已经落地"** → 此时开始读快照会读到**旧数据**。**加上 `o_write_idle` 才能保证"数据真的在 DDR 里了"** ✓。

### 4.3 覆盖保护（第 405–412 行）

```
405            // 覆盖保护：冻结完成后，写指针若再次进入 [start,end) 区间则告警
406            if (o_freeze_done) begin
407                if (wr_off >= f_start_off && wr_off < f_end_off)
408                    o_snap_overrun <= 1'b1;
409                else if (f_end_off < f_start_off &&                 // 跨回绕的冻结区
410                         (wr_off >= f_start_off || wr_off < f_end_off))
411                    o_snap_overrun <= 1'b1;
412            end
```
⭐ **两种情况的处理，说明设计者考虑到了"跨回绕"**：

- **情况 1（第 407–408 行）**：`f_end_off >= f_start_off`（冻结区**不跨**回绕点）→ 判据是简单区间 `[f_start_off, f_end_off)`。
- **情况 2（第 409–411 行）**：`f_end_off < f_start_off`（冻结区**跨**回绕点，即从 `f_start_off` 到环尾，再从 0 到 `f_end_off`）→ 判据是**两个区间**：`wr_off >= f_start_off`（在环尾那一段）**或** `wr_off < f_end_off`（在环首那一段）。
  - ⭐ **这是一个必须记住的"环形区间包含判断"技巧**：跨回绕的区间不能用一个比较表达，必须拆成两段。
  - ⚠️ **【观察 · 潜在缺陷】** `o_snap_overrun` 一旦置 1，**只有 `trig`/`resume` 才能清**（第 366/374 行）。而且它是**纯电平置位**（不是"单次检测"）→ 只要写指针**落在冻结区内**就每拍置位。这没问题（粘滞），但 ⚠️ **它不区分"覆盖的是还没搬走的数据"还是"已经搬完的数据"** —— 因为 `o_freeze_done` 一直在（直到 resume），所以**即使快照已经搬完，只要写指针还在那个区间里，就会一直报 overrun**。
  - ⚠️ **实际上 `o_snap_overrun` 的语义是"冻结区被写指针追上过"**，这是一个"历史事实"（粘滞），而不是"当前状态"。**PS 读到它应该理解为"这次快照可能已被污染"** ✓ 语义可以接受，但**判据设计时要知道它是粘滞且不精确的**。

---

## 5. 状态字与错误汇总（第 416–441 行）

```
419    always @(posedge clk or negedge rst_n) begin
420        if (!rst_n) begin
421            o_sts_tdata <= 8'h00;
422            o_err       <= 1'b0;
423            o_ovf       <= 1'b0;
424        end else begin
425            if (s_sts_tvalid)
426                o_sts_tdata <= s_sts_tdata;
427            // 状态字约定：0x80 = 正常完成；非 0x80 视为异常。错误同样
428            // 粘滞到软件确认，避免轮询漏掉单拍 DataMover 状态。
429            if (i_freeze_resume || !i_acq_en)
430                o_err <= 1'b0;
431            else if (i_s2mm_err || (s_sts_tvalid && (s_sts_tdata != `DM_STS_OK)))
432                o_err <= 1'b1;
```
- 第 425–426 行：**记录最近一次状态字**（给 `o_sts_tdata`，但该输出在顶层悬空）。
- ⭐ **错误判据**：`i_s2mm_err`（DataMover 硬错误）**或** 状态字 ≠ `0x80`。
- ⭐ **错误"粘滞"的理由**（注释第 427–428 行）：DataMover 状态字是**单拍脉冲**，PS 轮询**可能漏读** → 必须粘滞。
- ⭐ **清除条件 = `i_freeze_resume || !i_acq_en`**：
  - `i_acq_en = 0`（停止采集）→ 清错误（因为此时在"停机状态"，之前的错误不再有意义）；
  - `i_freeze_resume`（PS 的显式恢复命令）→ 也清（**作为 PS 的"我看到了，请清掉"确认**）。
  - ⭐ **这是一个重要细节**：**PS 必须先读 `DDR_STATUS` 再发 `resume`**，否则会漏掉错误。`pd_snapshot_poll.c:279-281` 就是这个顺序 ✓ 且注释也说明了：
    ```c
    /* FREEZE_RESUME is also the documented acknowledge/clear for ring o_err. */
    err_after_resume = reg_read(DDR_STATUS) & DDR_STATUS_ERR;
    if (err_after_resume) fail_stop("DDR_STATUS[5] remained set after freeze_resume");
    ```
    ⚠️ **注意这里有个逻辑问题**：`resume` 会**清** `o_err`，所以 `err_after_resume` 读到的应该是 **0**（因为刚被清）。判据"resume 之后仍置位则失败"是对的（证明清除机制生效）✓。

```
433            // 采集侧丢数：上游(异步FIFO满 / Data FIFO 反压)汇总是异步事件，
434            // 必须**粘滞锁存**后再给 PS 轮询，否则窄脉冲会被软件漏读。
435            // 清除条件：停止采集(i_acq_en=0) 或 PS 下发 freeze_resume 作为确认。
436            if (i_freeze_resume || !i_acq_en)
437                o_ovf <= 1'b0;
438            else if (i_up_ovf)
439                o_ovf <= 1'b1;
440        end
441    end
```
- ⭐ **`o_ovf` 的语义与清除条件与 `o_err` 完全一致**：粘滞 + 同样两个清除条件。
- ⭐ **"异步事件必须粘滞"是本工程反复出现的原则**（`pd_feature_core.st_overflow`、`pd_ddr_snap_copy.o_err` 都是同一个模式）。

### 5.1 三条输出路径的最终去向

| 本模块输出 | 去向 | 说明 |
|---|---|---|
| `o_err` | `pd_ddr_wr_top.v:743` 的 `i_err = ring_err \| ring_ovf \| copy_err \| snapshot_cfg_err` | → `DDR_STATUS[5]` |
| `o_ovf` | 同上 | → `DDR_STATUS[5]` |
| `o_last_btt` | ⚠️ **顶层悬空** | 不可观测 |
| `o_sts_tdata` | ⚠️ **顶层悬空** | 不可观测（注释第 427 行说"粘滞到软件确认"，但**该输出没引出去**！） |
| `o_state` | `dbg_ddr[15:14]` | ⚠️ `dbg_ddr` 未连出 BD → 不可观测 |

### 5.2 `DDR_STATUS[5]` 的二义性（**B09 已记录的核心问题**）

⭐ **`DDR_STATUS[5]` 的表达式（`pd_ddr_wr_top.v:743`）**：
```
i_err = ring_err | ring_ovf | copy_err | snapshot_cfg_err
```
其中 `copy_err` 与 `snapshot_cfg_err` **另有专门的位**（`DDR_STATUS[8]`/`[9]`）。
- ⚠️ **所以 `DDR_STATUS[5]` 实际只能是 `ring_err | ring_ovf`** —— **一个位承载了两种不同的错误语义**，而 PS 侧**无法区分**（因为 `ring_err` 与 `ring_ovf` 都被 OR 进同一位）。
- ⭐ **定稿文档 §5 把它简写成 "err"，掩盖了这个二义性。**

### 5.3 `ring_ovf` 一次性误报的机制（**已闭环定位**）

⭐ **这是本工程最有价值的一次调试结论**，机制如下：

1. **`i_up_ovf` 与 `i_beat_en` 都不受 `i_acq_en` 门控**：
   - 第 260 行 `if (i_beat_en) avail_bytes += 8` —— 无 `acq_en` 条件；
   - `pd_ddr_wr_top.v:413-414` 的 `up_ovf = (s48_a_valid & (fifo_full | fifo_wr_busy)) | (s48_b_valid & (feat_fifo_full | feat_fifo_wr_busy))` —— 也无 `acq_en` 条件。
2. **采集关闭期（`acq_en = 0`）上游仍灌数**：
   - `pd_pack192` 的 `fifo_rd_en = pk_s48_ready & ~fifo_empty & ~fifo_rd_busy`（`pd_ddr_wr_top.v:324`）—— **只看 FIFO 有没有数据，不看 `acq_en`**；
   - 所以数据继续从异步 FIFO 被读出、打成 192 bit 块、推进 Stream FIFO；
   - `i_beat_en` 继续拉高 → **信用累加到 `AVAIL = 4144`**（≥ `OVF_THRESH` 3072 → `o_hiwm = 1`）。
3. **`o_err` 在采集关闭期被强制清零**（第 429 行 `if (... || !i_acq_en) o_err <= 1'b0`）→ ⚠️ **这是"优先分支"**：它压制了第 431 行的错误置位。
   - 所以即使此时 `i_up_ovf` 已经很高，`o_err` 仍然是 0；而 `o_ovf` 同样在 `!i_acq_en` 时被清零（第 436 行）。
4. **`acq_en = 1`（PS 开始采集）那一拍**：
   - 两个清零分支**都不再成立**；
   - `else if` 分支生效：`i_up_ovf` 仍为高 → **`o_ovf <= 1'b1` 立刻置位**！
   - ⭐ **这就是"启动瞬态"的完整机制**：不是真的丢了数据，而是**采集关闭期累积的上游溢出信号在采集使能的那一拍被"补记"了进来**。
5. **`freeze_resume` 之后不复现**：因为 PS 第一次 `resume` 会清 `o_ovf`，而此时 `i_up_ovf` 已经不再为高（采集已经正常运行、FIFO 不再溢出）→ **不会再次置位** ✓。
   - ⭐ **所以它是"一次性"的**，与 `pd_snapshot_poll.c:144-145` 的注释完全吻合："Ring ERR is sampled by the caller after READY, then acknowledged by FREEZE_RESUME. It may contain the known acquisition-startup pulse."

⭐ **两条可能的修法（未实施）**：
- **方案 ①（顶层门控）**：`beat_en_gated = acq_en & beat_en`、`up_ovf_gated = acq_en & up_ovf`。
  - ⚠️ **注意这会改变"采集关闭期信用累计"的语义**（关闭期不再累加信用），**改前必须与 PS 确认**。
- **方案 ②（模块内清零）**：在 `pd_ddr_ring_wr` 内部检测 `i_acq_en` 的**上升沿**，清 `avail_bytes`。
  - ⭐ 这个方案更保守：**只清信用，不改门控关系**。

⚠️ **要 100% 排除 `ring_err`（而不是 `ring_ovf`）仍需把 `dbg_ddr[9]`（= `ring_err`）引出来**（`pd_ddr_wr_top.v:788`），因为当前两者 OR 在同一位上，无法区分。

---

## 6. 本篇易错点小结

1. ⭐ **同一变量同拍既有"无条件 +"又有"有条件 −"时必须合并成一次赋值**，否则静默丢失增量（信用计数系统性偏低 → 最后停摆）。
2. ⭐ **信用机制的联动约束**：`avail_bytes >= BURST_BYTES` 才发命令 → **Stream FIFO 深度必须 ≥ `BURST_BYTES`**；改 `BURST_BLOCKS` 必须同步核对。
3. ⭐ **`BURST_BLOCKS` 有上限**：`BURST_BYTES / 8 ≤ 256 beat` → 最大约 **85 块**（1536 = 64 块是安全选择）。
4. ⭐ **`ring_remain` 的组合 32 位减法是当前的时序关键路径**（WNS −0.054 / 19 级 / 布线 54.9%）。修法是"连续寄存 `ring_remain`"，但**必须做跨 1536 临界拍与回绕前后的仿真回归**。
5. **参数自检用 `$display` 而非 `$error`**（不会中止仿真），与 `pd_dds_adc_source` 不一致。
6. ⭐ **`o_write_idle` 是快照搬运的必要前提**：它保证"数据真的写进 DDR 了"，不只是"命令发出去了"。
7. ⭐ **冻结用"两个周期边界"界定一个周期**，自动适应工频频率变化；`FREEZE_LEN` 是**实测值**而非固定 3,120,000。
8. ⭐ **"精确流位置"（`stream_*`，24 B 粒度）与"命令指针"（`wr_off`，1536 B 粒度）必须分开追踪**，否则周期边界会被 1536 字节量化。
9. **环形区间包含判断在"跨回绕"时必须拆成两段**（`wr_off >= f_start_off || wr_off < f_end_off`）。
10. **`o_snap_overrun` 是粘滞且不精确的**：它只表示"冻结区被追上过"，不代表"数据确实被破坏了"；且不会区分"已搬完"与"未搬完"。
11. **错误清除条件 = `i_freeze_resume || !i_acq_en`** → **PS 必须先读 `DDR_STATUS` 再发 `resume`**，否则漏掉错误。
12. ⭐ **`DDR_STATUS[5]` = `ring_err | ring_ovf`**（另有 `copy_err`/`snapshot_cfg_err` 占了 bit8/9）→ **一位承载两种语义，PS 无法区分**。
13. ⭐ **`ring_ovf` 的"启动瞬态"机制**：`beat_en` 与 `up_ovf` 都不受 `acq_en` 门控 + `!acq_en` 时 `o_err`/`o_ovf` 被强制清零（优先分支）→ **`acq_en` 上升那一拍把关闭期累积的上游溢出"补记"进来**。是**一次性**的，`resume` 后不复现。
14. ⭐ **`wr_inflight` 的减计数必须加 `!= 0` 保护**，防止"孤立状态字"导致下溢。
15. ⚠️ **`o_last_btt`/`o_sts_tdata`/`o_blk_idx`/`o_state` 全部不可观测**（前者悬空，后者 `dbg_ddr` 未连出 BD）。
16. **冻结收尾命令不检查信用**（避免把 64 位减法串进使能路径），代价是"若上游溢出被丢数据，这条命令会去取不存在的数据"。

---

**本卷下一篇**：`A09_快照拷贝引擎.md` —— 用 DataMover 的 MM2S+S2MM 环回做"内存到内存"拷贝，含分片、串行化与错误粘滞。
