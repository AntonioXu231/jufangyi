# A10 · DDR 顶层、寄存器接口与 BD 适配

覆盖文件：
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_ddr_wr_top.v`（794 行）—— DDR 支路顶层（三条拷贝路径的仲裁 + 全部 IP 例化）
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_ddr_axil.v`（380 行）—— DDR 支路的 AXI4-Lite 寄存器
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_ddr_bd_adapter.v`（280 行）—— IP Integrator 适配层

---

## 1. `pd_ddr_wr_top.v` —— DDR 支路顶层

### 1.1 数据流与 IP 清单（第 1–23 行）

```
4  // pd_ddr_wr_top.v  --  Path A：ADC -> 48bit 打包 -> 跨时钟 FIFO -> 192bit 块
5  //                      -> AXI-Stream FIFO -> DataMover -> PS7 HP -> DDR3 环形区
6  //                      + 冻结快照区拷贝链（DataMover MM2S->S2MM 环回）
8  // 官方 IP 使用清单（尽量把"轮子"交给 IP，自研只留真正的业务逻辑）：
9  //   1. ZYNQ7 Processing System (PS7)   —— DDR3 控制器本体 + S_AXI_HP0/1 从机口
10 //      （领航者V2 的 DDR3 挂在 PS 上，PL 经 HP 口访问；若换用 PL 侧 DDR 则改 MIG7）
11 //   2. fifo_generator (fifo_48_cdc)    —— 48bit 独立时钟 BRAM FIFO（ADC 域->130M 域）
12 //   3. axis_data_fifo (axis_dfi_64)    —— 64bit AXI-Stream 弹性缓冲（吸收突发抖动）
13 //   4. axi_datamover  (dm_wr)          —— S2MM：流 -> 内存，负责 AXI4 突发/拆包
14 //   5. axi_datamover  (dm_cp)          —— MM2S+S2MM：快照区内存到内存拷贝
15 //   6. SmartConnect / AXI Interconnect —— AXI4(DataMover) 转 AXI3(PS7 HP) 并仲裁
16 //   7. Clocking Wizard / Proc Sys Reset—— 130MHz 时钟与复位（BD 内）
```
⭐ **第 8 行的设计理念值得抄下来**：**"尽量把轮子交给 IP，自研只留真正的业务逻辑。"**
- 本文件例化了 5 个 IP 核 + 7 个自研模块，分工明确：
  - **IP 负责**：跨时钟 FIFO、Stream 弹性缓冲、AXI 主机协议（DataMover）；
  - **自研负责**：48 bit 打包、192 bit 组织、**环形指针、信用机制、冻结锁存、槽管理**。
- ⭐ **这是一个正确的工程判断**：AXI 主机协议（含 4 KB 拆包、outstanding、错误处理）**极其繁琐且容易出错**，自己写不划算；而"环形缓冲区的业务语义"没有任何 IP 提供，**必须自己做**。

⚠️ **第 10 行的注释提到"领航者V2"** —— 这是开发板的型号（正点原子领航者 V2）。**说明设计者最初是按开发板做的**，而当前工程已改为**核心板 + 外部 50 MHz 晶振**（见 `.bd`：`clk_wiz_0` 的 `PRIM_IN_FREQ = 50`）。

```
20 // 数据率核对（26MSPS 当前验证档）：
21 //   4ch x 12bit x 26MSPS = 156 MB/s；130MHz x 8B = 1040 MB/s，占用 15%。
22 //   一个 50Hz 周期快照为 3.12MB，和环写叠加后仍有充分的 HP 带宽余量。
```
- 与 `pd_ddr_defines.vh:10-13` 的核算一致 ✓。
- ⭐ **"和环写叠加后仍有充分的 HP 带宽余量"**：环写 156 MB/s + 快照拷贝（3.12 MB / 假设 100 µs ≈ 31 GB/s 瞬时？） —— 实际上**快照拷贝是"爆发式"的**（508 片串行，但每片的 DataMover 突发可以全速跑），所以**瞬时带宽会冲得很高**，但因为持续时间短（几十到几百微秒），平均占用仍然很低 ✓。

### 1.2 复位（第 233–250 行）

```
237    wire fifo_rst = ~rst_n;                     // 高有效
238    wire ddr_rst_n = rst_n & ~sw_rst;           // 低有效，含软件复位
243    (* ASYNC_REG = "TRUE" *) reg [1:0] adc_rst_sync;
244    always @(posedge adc_clk or negedge rst_n) begin
245        if (!rst_n)
246            adc_rst_sync <= 2'b00;
247        else
248            adc_rst_sync <= {adc_rst_sync[0], 1'b1};
249    end
250    wire adc_rst_n = adc_rst_sync[1];
```
- ⭐ **第 235–236 行的注释**："`fifo_generator` 的 `rst` 为**高有效异步**；DataMover 为**低有效同步**。"
  - **不同 IP 的复位极性不同**，所以要做转换：`fifo_rst = ~rst_n` ✓。
  - ⭐ **这是一个很实用的提醒**：Xilinx 的 FIFO Generator 生成的 FIFO 复位是**高有效**，而大多数 AXI IP 是低有效。**接错极性会导致"上电后 FIFO 一直处于复位"或"永远不复位"。**
- ⭐ **第 243–250 行与 `pd_feature_sys_top.v:134-141` 完全同构**：在 `adc_clk` 域用两级同步器释放复位（"异步复位 + 同步释放"）。**这里用的是"每拍移入 1"的技巧**（与那里一样）。

⚠️ **【观察 · 已知问题（B09 已记录）】** `sw_rst` 是**单拍脉冲**（`pd_ddr_axil.v:257`：`if (wstrb[0] && wdata[1]) o_sw_rst <= 1'b1;`，且第 249 行每拍清 0）→ 所以 `ddr_rst_n = rst_n & ~sw_rst` **只低一个时钟周期 = 7.7 ns @130 MHz**。
- ⭐ **对 DataMover 这种"需要若干时钟才能完成复位"的 IP 来说，7.7 ns 的复位脉冲可能太窄**：DataMover 内部有复位状态机，一个周期的低电平可能不足以让它完整复位。
- ⚠️ **影响**：软复位可能"部分生效"或"完全不生效"，而**没有错误指示**。这是一个**未闭合的风险**（B09 已记录）。

⚠️ **【观察 · 重要】** `pd_ddr_wr_top.v:332` 把 `ddr_rst_n` 接到 `pd_pack192` 的 `rst_n`，但 **`pd_ddr_snap_copy`（第 666 行）与 `pd_ddr_slot_mgr`（第 622 行）也用 `ddr_rst_n`** —— 所以软复位会清掉槽管理状态（`slot_state` 全回 `FREE`）→ **所有未处理的槽被静默丢弃**。这是"软复位"的预期行为吗？**需要确认**（B09 已记录）。

### 1.3 两条独立的 48 bit 异步 FIFO（第 270–313 行）

```
275    wire fifo_wr_en = s48_a_valid & ~fifo_full & ~fifo_wr_busy;
278    fifo_48_cdc u_fifo48 (...);          // Path A
294    wire feat_fifo_wr_en = s48_b_valid & ~feat_fifo_full & ~feat_fifo_wr_busy;
295    wire feat_fifo_rd_en = ~feat_fifo_empty & ~feat_fifo_rd_busy;
297    fifo_48_cdc u_fifo48_feature (...);  // Path B
311    assign o_feat_data = feat_fifo_dout;
312    assign o_feat_dv   = {CH_NUM{feat_fifo_rd_en}};
313    assign o_feat_ovf  = s48_b_valid & (feat_fifo_full | feat_fifo_wr_busy);
```
⭐ **第 292–293 行的注释点出了"扇出边界"的意义**：
```
292    // Path B has a separate official asynchronous FIFO. This is the actual
293    // fanout boundary: DDR backpressure cannot stall or reorder feature input.
```
- ⭐ **"两条独立 FIFO"就是 Path A / Path B 的物理分界点**：
  - **Path A**（`u_fifo48`）→ 送进 `pd_pack192` → DataMover → DDR；
  - **Path B**（`u_fifo48_feature`）→ 直接输出给 `pd_filter_0`/`pd_feature_0`。
- ⭐ **为什么必须独立？** 因为 Path A 会被 DDR **反压**（FIFO 满、DataMover 忙、DDR 带宽竞争）。如果两条路共用 FIFO，那么**DDR 的反压会传入 Path B，导致实时特征链的样本节拍被拖慢甚至乱序** → 相位统计错乱。
- ⭐ **"independent FIFO = 扇出边界"是一个极其重要的架构结论**：**实时路径与存储路径必须在扇出点各有一条独立缓冲。**
- ⚠️ **注意第 312 行 `o_feat_dv = {CH_NUM{feat_fifo_rd_en}}`**：**读使能直接当 dv 的复制**。
  - 因为 FIFO 是 FWFT 的（`dout` 在非空时已有效），所以 `rd_en` 拉高 = 本拍的数据有效 ✓。
  - ⭐ 但**这里假设了"每次 `rd_en` 都真的读出一个样本"**。`feat_fifo_rd_en = ~feat_fifo_empty & ~feat_fifo_rd_busy` → 非空且不在复位期 ✓ 所以每次 `rd_en` 都真的读 ✓ 正确。
  - ⭐ **注意 `o_feat_dv` 是 4 位全同**（四通道同拍有效）—— 这正是**"先对齐再跨域"**的体现（见 A07 §3.1）✓。
- ⭐ **第 313 行 `o_feat_ovf`** 是 Path B 的溢出指示（FIFO 满时仍在写）→ 但 ⚠️ **它连到 `pd_ddr_bd_adapter` 的 `o_feat_ovf` 端口，该端口在 BD 里没有连出去**（B09 已记录）。

### 1.4 `up_ovf`（第 412–414 行）

```
412    // 上游溢出：异步 FIFO 满时仍在写 / Data FIFO 反压导致丢样本
413    wire up_ovf = (s48_a_valid & (fifo_full | fifo_wr_busy)) |
414                  (s48_b_valid & (feat_fifo_full | feat_fifo_wr_busy));
```
- ⭐ **`up_ovf` 同时监控两条路径的溢出**：
  - `s48_a_valid & (fifo_full | fifo_wr_busy)`：Path A 的 FIFO 满时还在写 → 丢样本；
  - `s48_b_valid & (feat_fifo_full | feat_fifo_wr_busy)`：Path B 的 FIFO 满时还在写 → 丢样本。
- ⭐ **注意第 313 行的 `o_feat_ovf` 与这里的第二项完全一样** —— 所以 `up_ovf` 包含了 `o_feat_ovf`。**`up_ovf` 是"任何一路丢样本"的汇总。**
- ⚠️ **这是"`ring_ovf` 一次性误报"的参与方之一**（见 A08 §5.3）：`up_ovf` **不受 `acq_en` 门控** → 采集关闭期如果 FIFO 满，`up_ovf` 会持续为高 → 在 `acq_en` 上升那一拍被"补记"进 `o_ovf`。

### 1.5 三条拷贝路径的仲裁（第 538–616 行）—— 本文件最复杂的逻辑

```
539    // 9) 快照拷贝控制：freeze_done 后必须等 DataMover 写尾部全部完成。
540    //    否则 MM2S 可能先于最后一条 S2MM 写读到旧数据。
```
⭐ **三条路径**：

| 路径 | 触发条件 | 目标地址 | 何时启用 |
|---|---|---|---|
| **legacy 自动** | `!auto_snap_en && legacy_auto_copy_pending` | 固定的 `snap_base`（`0x1800_0000`） | 四槽模式**关闭**时 |
| **手动** | `o_snap_start`（`DDR_CTRL[2]`） | 固定的 `snap_base` | 任何时候（**优先级最高**） |
| **四槽自动** | `auto_snap_en && slot_freeze_req` | 轮转槽基址 | 四槽模式**开启**时 |

```
542    reg freeze_done_d;
543    reg legacy_auto_copy_pending;
544    reg slot_freeze_req;
545    reg slot_req_overflow_pulse;
552    always @(posedge clk or negedge rst_n) begin
553        if (!rst_n) begin  ...  end
554        else begin
558            freeze_done_d <= freeze_done;
559            slot_req_overflow_pulse <= 1'b0;
561            if (freeze_resume) begin
562                legacy_auto_copy_pending <= 1'b0;
563                slot_freeze_req <= 1'b0;
564            end else begin
565                if (~freeze_done_d & freeze_done) begin
566                    if (auto_snap_en) begin
567                        if (slot_freeze_req)
568                            slot_req_overflow_pulse <= 1'b1;
569                        else
570                            slot_freeze_req <= 1'b1;
571                    end else begin
572                        legacy_auto_copy_pending <= 1'b1;
573                    end
574                end
575                if (!auto_snap_en)
576                    slot_freeze_req <= 1'b0;
577                if (legacy_auto_copy_launch)
578                    legacy_auto_copy_pending <= 1'b0;
579                if (slot_req_ack || slot_req_drop)
580                    slot_freeze_req <= 1'b0;
581            end
582        end
583    end
```
⭐ **这段是"freeze_done 上升沿 → 分派到三条路径之一"的全部逻辑**，逐条理解：

- 第 558 行 `freeze_done_d <= freeze_done`：**延迟一拍**副本，用于**上升沿检测**。
- 第 565 行 `if (~freeze_done_d & freeze_done)`：**freeze_done 的上升沿**（只在"刚完成"那一拍进入）。
  - ⭐ **用上升沿而不是电平**，保证"一次 freeze 只分派一次" ✓。
- 第 566–573 行：按 `auto_snap_en` 分派：
  - **`auto_snap_en = 1`**（四槽模式）：
    - 第 567–568 行：**如果 `slot_freeze_req` 已经为 1**（说明上一次的请求还没被处理，又有新的 freeze 完成）→ 置 `slot_req_overflow_pulse`（**请求溢出！**）；
    - 第 569–570 行：否则置 `slot_freeze_req = 1`（向槽管理器发请求）。
    - ⚠️ **【观察 · 重要】** 这个"请求溢出"计数是**丢快照的指示**。为什么会有"上一次请求还没处理"的情况？**因为一次快照请求的处理链很长（预留 → 等 ring_write_idle → 拷贝 → 完成 → PS resume）**，如果在这期间又完成了一次 freeze，就会溢出。**但注意：`freeze_trig` 是由 `pd_snapshot_trigger` 控制的，它会在触发后上锁 `waiting_resume`，直到 PS resume 才解锁** → 所以**正常情况下不会连续触发**。
      - ⚠️ **但 `sw_freeze_trig`（软件触发）不受 `waiting_resume` 限制**！如果 PS 连续写两次 `FREEZE_CTRL = 1`，就会产生两次 freeze → 第二次可能在第一次处理完之前完成 → **触发 `slot_req_overflow_pulse`**。**这是一个 PS 侧需要注意的使用约束**（B09 已记录）。
  - **`auto_snap_en = 0`**（legacy 模式）→ 置 `legacy_auto_copy_pending = 1`。
- 第 561–564 行：**`freeze_resume` 会清空两个 pending**（取消未处理的请求）。
- 第 575–576 行：**若中途把 `auto_snap_en` 关掉，清 `slot_freeze_req`**（避免"模式切换后还给槽管理器发请求"）。
- 第 577–578 行：`legacy_auto_copy_launch` 成立时清 `legacy_auto_copy_pending`（**说明请求已被消费**）。
- 第 579–580 行：`slot_req_ack`（被接受）或 `slot_req_drop`（被拒绝）都清 `slot_freeze_req` ✓ **这两条覆盖了"请求被槽管理器处理"的所有结果**。

```
584    wire snap_len_err = (freeze_len > snap_size);
587    wire snap_addr_err = (snap_base < `DDR_SNAP_BASE) ||
588                          (snap_base >= (`DDR_SNAP_BASE + `DDR_SNAP_SIZE)) ||
589                          (freeze_len > (`DDR_SNAP_BASE + `DDR_SNAP_SIZE - snap_base));
602    wire snap_align_err = !snap_base_align24 || !snap_size_align24 ||
603                          !freeze_base_align24 || !freeze_len_align24;
604    wire legacy_snapshot_cfg_err = snap_len_err || snap_addr_err || snap_align_err;
```
⭐ **legacy 路径的配置合法性检查（三项）**：
1. `snap_len_err`：冻结长度不能超过 `snap_size`；
2. `snap_addr_err`：**快照目标必须完全落在编译期保留的 Snapshot 分区内** —— 注意第 585–586 行的注释："防止可配置 `SNAP_BASE`/`SNAP_SIZE` 把拷贝写到 Ring 或其他 DDR 使用者区域。"
   - ⭐ **这是一个重要的安全设计**：`snap_base`/`snap_size` 是**PS 可写的**（`0x20`/`0x24`）。如果不加范围检查，PS 可以把 `snap_base` 设成环形区地址 → **一次拷贝就把正在写的环形区覆盖了** → 数据全部损坏。
   - 三个判据：不低于 `SNAP_BASE`、不高于 `SNAP_BASE + SNAP_SIZE`、**且"起始 + 长度"不超过分区末尾** ✓ 完整。
3. `snap_align_err`：四个值都要 24 B 对齐。

```
607    assign legacy_auto_copy_launch = !auto_snap_en && legacy_auto_copy_pending &&
608                                     ring_write_idle && !copy_busy &&
609                                     !legacy_snapshot_cfg_err;
610    assign manual_copy_launch = snap_start && freeze_done && ring_write_idle &&
611                                !copy_busy && !legacy_snapshot_cfg_err;
612    wire snapshot_cfg_err = auto_snap_en ? slot_cfg_err : legacy_snapshot_cfg_err;
613    wire copy_start = legacy_auto_copy_launch | manual_copy_launch | slot_copy_start;
614    wire [AXI_ADDR_W-1:0] copy_src_addr = slot_copy_start ? slot_copy_src_addr : freeze_base;
615    wire [AXI_ADDR_W-1:0] copy_dst_addr = slot_copy_start ? slot_copy_dst_addr : snap_base;
616    wire [31:0] copy_len = slot_copy_start ? slot_copy_len : freeze_len;
```
⭐ **三条路径的启动条件对比**：

| 路径 | `ring_write_idle` | `!copy_busy` | 配置检查 | 额外条件 |
|---|---|---|---|---|
| legacy 自动 | ✓ | ✓ | `!legacy_snapshot_cfg_err` | `!auto_snap_en && legacy_auto_copy_pending` |
| 手动 | ✓ | ✓ | `!legacy_snapshot_cfg_err` | `snap_start && freeze_done` |
| 四槽自动 | （由 slot_mgr 内部判） | （由 slot_mgr 内部判） | （由 slot_mgr 内部判） | — |

⭐ **第 605–606 行的注释**：
```
605    // Legacy automatic capture is retained when slot automatic mode is disabled.
606    // A manual o_snap_start remains available in either mode and gets priority.
```
- **手动路径在两种模式下都可用，且优先**：因为 `slot_copy_start` 由槽管理器产生，而槽管理器里有 `!i_manual_copy_launch` 条件 → **手动启动时自动路径会让路** ✓。
- 第 613 行 `copy_start = 三条路径的或` —— **三条路径共享同一个 `pd_ddr_snap_copy` 实例**（因为只有一个 MM2S+S2MM 拷贝引擎）。
- 第 614–616 行：**描述符的选择用三目选择器** —— `slot_copy_start` 为真时用槽的描述符，否则用 `freeze_base`/`snap_base`/`freeze_len`。
  - ⚠️ **【观察】** 如果**同拍**有两条路径都成立（例如 `manual_copy_launch` 与 `slot_copy_start` 同时为 1），则 `copy_start = 1` 但描述符取槽的 → **手动路径会"借用"槽的目标地址**！这是个逻辑漏洞，但**实际不会发生**（因为 `slot_mgr` 里有 `!i_manual_copy_launch` 条件）。**属于"依赖上游互斥"的隐含假设**（B09 已记录）。

### 1.6 中断与调试观测（第 774–793 行）

```
777    // slot_snapshot_ready is level-held until SLOT_CTRL.status_clear.  This
778    // converts an otherwise one-cycle automatic-copy completion into a PS-
779    // observable interrupt without changing the external irq port contract.
780    assign irq = slot_snapshot_ready | copy_done | snap_overrun | ring_err |
781                 ring_ovf | copy_err | snapshot_cfg_err;
782    assign dbg_ddr = {
783        ring_state,          // [15:14]
784        copy_busy,           // [13]
785        freeze_done,         // [12]
786        snap_overrun,        // [11]
787        hiwm,                // [10]
788        ring_err,            // [9]
789        copy_err,            // [8]
790        skew_err,            // [7]
791        wr_off[6:0]          // [6:0] 写指针低 7 位（回绕观测）
792    };
```
⭐ **中断（`irq`）是 7 个条件的或**：
| 来源 | 含义 |
|---|---|
| `slot_snapshot_ready` | ⭐ **四槽自动快照完成（电平保持）** |
| `copy_done` | 拷贝完成（单拍脉冲） |
| `snap_overrun` | 冻结区被追上 |
| `ring_err` | 环形写错误 |
| `ring_ovf` | 环形写上游溢出 |
| `copy_err` | 拷贝错误 |
| `snapshot_cfg_err` | 快照配置错误 |

- ⭐ **第 777–779 行的注释解释了 `slot_snapshot_ready` 的特殊地位**："它电平保持直到 `SLOT_CTRL.status_clear`。这把一个原本只有单周期宽度的'自动拷贝完成'转换成了 **PS 可观测的中断**，而不改变外部 irq 端口契约。"
  - ⭐ **这是"把窄脉冲转成电平"以满足中断需求**的典型手法（`copy_done` 只有 1 拍 = 7.7 ns，电平触发的中断可能错过）。
  - ⭐ **注释还说"不改变外部 irq 端口契约"** —— 意思是"irq 仍然是一个电平信号，没有新增端口"，所以**对 BD 与 PS 侧无影响** ✓ 这是一个"在不破坏接口的前提下增强功能"的好例子。

⭐⭐ **`dbg_ddr` 是本工程最大的可观测性缺口**（B09 已记录）：
- 第 782–792 行**算好了 16 位调试信号**，包含：
  - `ring_state[1:0]`（状态机状态）
  - `copy_busy`、`freeze_done`、`snap_overrun`、`hiwm`
  - ⭐ **`ring_err[9]`** —— **这一位正是"区分 `ring_err` 与 `ring_ovf`"所需的**
  - `copy_err[8]`、`skew_err[7]`、`wr_off[6:0]`
- 但 ⚠️ **在 `.bd` 里 `pd_ddr_0` 的 `dbg_ddr` 端口只有声明、没有连线**：
  - 于是 `dbg_ddr` **不可观测**；
  - 而且因为它**没有被任何下游使用**，综合器会**把整个 `dbg_ddr` 逻辑优化掉**（包括那些比较器）。
- ⭐ **引出的方法很简单**：`pd_ddr_axil` 的寄存器表**止于 `0xA0`**，而 `0xA4–0xFF` 是空闲的 → **只要把 `dbg_ddr` 引到 `0xA4` 就能零逻辑开销地分开 `ring_err` 与 `ring_ovf`**。
- ⚠️ **但注意：`ring_err` 与 `ring_ovf` 的 OR 目前已经进了 `DDR_STATUS[5]`**，而 `dbg_ddr[9]` 只带 `ring_err`（**不带 `ring_ovf`**）→ 引出来后可以"`DDR_STATUS[5] = 1` 且 `dbg_ddr[9] = 0`" ⇒ **确认是 `ring_ovf` 而非 `ring_err`** ✓ 这正是当前调试所需要的。

---

## 2. `pd_ddr_axil.v`（380 行）—— DDR 支路寄存器表

### 2.1 文件头（第 1–45 行）—— 完整寄存器表

```
7  // 说明：现有 pd_axil_regs 已占用 0x1000~0x101C 全局区。为免与正在演进的
8  //       v3 寄存器冲突，本从机使用**独立 4KB 页 0x2000**（addr[15:12]=2），
11 //      但冻结组保持契约的组内偏移 0x28/0x2C/0x30/0x34，
```
⭐⭐ **第 7–11 行揭示了一个"地址映射与契约不符"的来源**：
- 本从机内部使用**页内偏移** `0x00`–`0xA0`，并且假定自己坐在**页基址 `0x2000`** 上；
- 但 **BD 里 `pd_ddr_0/s_axi` 实际被映射到 `0x4001_0000`**（见 `.bd` 的 `addressing`）；
- 所以**实际地址 = `0x4001_0000 + 页内偏移`**。
  - 例：`FREEZE_BASE_LO` 页内偏移 `0x28` → **实际地址 `0x4001_0028`**；而**契约写的是 `0x1028`**。
- ⚠️ **这就是 B09 里记录的"契约地址 ≠ 实际地址"**：`pd_ddr_axil` 内部用页 `0x2000` 但 BD 映射到 `0x4001_0000`。
- ⭐ **好消息**：因为在 AXI-Lite 从机的视角里，**只要地址的低 12 位（页内偏移）正确，高位由互连决定** —— 所以**功能是对的**（`s_axi_awaddr[11:0]` 用于解码，见第 177 行）。**只是"契约文档里的绝对地址"没有跟着更新**。

**完整寄存器表（页内偏移）**：

| 偏移 | 名称 | R/W | 位域 |
|---|---|---|---|
| `0x00` | DDR_CTRL | RW | `[0]acq_en` `[1]sw_rst(W1P)` `[2]snap_start(W1P,调试)` |
| `0x04` | DDR_STATUS | RO | `[0]acq_en` `[1]copy_busy` `[2]hiwm` `[3]freeze_done` `[4]snap_overrun` `[5]err` `[6]copy_done(sticky)` `[7]copy_pending` `[8]copy_err` `[9]snapshot_cfg_err` |
| `0x08` | RING_BASE | RO | 编译期固定 |
| `0x0C` | RING_SIZE | RO | 编译期固定 |
| `0x10` | RING_WR_PTR | RO | 环内字节偏移 |
| `0x14` | RING_WRAP | RO | 回绕次数 |
| `0x18`/`0x1C` | WR_BYTES_LO/HI | RO | 累计写入字节数 |
| `0x20`/`0x24` | SNAP_BASE/SNAP_SIZE | RW | legacy 快照区参数 |
| `0x28` | FREEZE_BASE_LO | RO | 冻结段基址（**契约偏移**） |
| `0x2C` | FREEZE_BASE_HI | RO | 恒返回 0（32 位地址） |
| `0x30` | FREEZE_LEN | RO | 冻结段长度（字节） |
| `0x34` | FREEZE_CTRL | RW | `[0]trig(W1P)` `[1]resume(W1P)` / RO `[8]done` |
| `0x38` | CMD_CNT | RO | 已下发命令数 |
| `0x3C` | AVAIL_BYTES | RO | 当前字节信用 |
| `0x40` | SNAP_CHUNKS | RO | 已完成的拷贝分片数 |
| `0x44` | SNAP_BYTES | RO | 本次已完成的拷贝字节数 |
| `0x48` | SNAP_SEQ | RO | 成功快照序号（每次 `copy_done` +1） |
| `0x4C` | SLOT_CTRL | RW | `[0]auto_snap_en` `[7:4]lock(W1P)` `[11:8]release(W1P)` `[12]status_clear(W1P)` |
| `0x50` | SLOT_STATUS | RO | `[3:0]valid` `[7:4]busy` `[11:8]locked` `[12]full` `[13]cfg_err` `[14]req_overflow` `[15]cmd_err` `[16]pending` `[18:17]last_slot` `[19]snapshot_ready` |
| `0x54` | SLOT_SEQ | RO | 自动快照成功序号 |
| `0x58` | SLOT_DROPS | RO | 自动请求拒绝/溢出累计数 |
| `0x5C` | SNAP_TRIG_CTRL | RW | `[0]event_trig_en` `[4:1]event_ch_mask` `[8]drop_count_clear(W1P)` / RO `[16]armed` `[17]drop` |
| `0x60`–`0x9C` | 四槽描述符 | RO | 每槽 `BASE`/`LEN`/`SEQ`/`FLAGS`（步长 `0x10`） |
| `0xA0` | SNAP_TRIG_DROPS | RO | 自动触发因四槽全满被拒绝的累计数 |

⭐ **注意 SLOT_STATUS 的位布局（第 36–38 行注释）**：
```
[3:0] valid   [7:4] busy   [11:8] locked   [12] full   [13] cfg_err
[14] req_overflow   [15] cmd_err   [16] pending   [18:17] last_slot   [19] snapshot_ready
```
→ ⭐ **这 4 组 4 位掩码（valid/busy/locked）与 `pd_feature_top` 的 `pd_axil_regs` 里 `pd_feature_0` 的对应位完全一致**（`PD_SLOT_VALID_MASK = 0x0F`、`PD_SLOT_BUSY_MASK = 0xF0`、`PD_SLOT_LOCKED_MASK = 0xF00`，见 `pd_hw_map.h:57-59`）✓ 一致性良好。

### 2.2 写通道实现（第 184–296 行）

```
192            if (aw_done && w_done && !s_axi_bvalid &&
193                (((waddr == A_CTRL) && wstrb[0] && wdata[2]) ||
194                 ((waddr == A_FZ_CTRL) && wstrb[0] && (wdata[0] || wdata[1]))))
195                copy_done_sticky <= 1'b0;
196            else if (i_copy_done)
197                copy_done_sticky <= 1'b1;
```
⭐ **`copy_done_sticky`（`DDR_STATUS[6]`）的置位/清除逻辑**：
- **置位**：`i_copy_done`（拷贝完成脉冲）→ **粘滞**；
- **清除**：写 `DDR_CTRL[2]`（`snap_start`）**或** 写 `FREEZE_CTRL[0]`（`trig`）或 `[1]`（`resume`）。
- ⭐ **又是一次"单拍脉冲转粘滞"**，让 PS 轮询能可靠看到"拷贝完成过" ✓。

```
200            if (s_axi_awvalid && !aw_done) begin
201                waddr         <= s_axi_awaddr[11:0];
202                aw_done       <= 1'b1;
203                s_axi_awready <= 1'b1;
204            end else
205                s_axi_awready <= 1'b0;
...
215            if (aw_done && w_done && !s_axi_bvalid) begin
216                s_axi_bvalid <= 1'b1;
217                s_axi_bresp  <= 2'b00;      // OKAY
218                aw_done      <= 1'b0;
219                w_done       <= 1'b0;
220            end else if (s_axi_bvalid && s_axi_bready)
221                s_axi_bvalid <= 1'b0;
```
- ⭐ **本从机与 `pd_axil_regs` 不同，它是"AW 与 W 独立接收"的正确实现**：
  - 第 200–205 行：`AWVALID` 一到就收，立即拉 `AWREADY` 一拍（**不要求 `WVALID` 同时到**）；
  - 第 207–213 行：`WVALID` 一到就收，立即拉 `WREADY` 一拍；
  - 第 215–219 行：**两者都到齐（`aw_done && w_done`）才回 `BVALID`** → 顺序无关 ✓。
- ⭐ **所以本工程里有三种 AXI-Lite 从机风格**：
  | 模块 | AW/W 独立？ |
  |---|---|
  | `pd_axil_regs`（feature） | ❌ 要求同时有效 |
  | `pd_filter_chain`（filter） | ✅ 用 `aw_hold`/`w_hold` 缓存 |
  | `pd_ddr_axil`（ddr） | ✅ 用 `aw_done`/`w_done` 缓存 |
  → ⚠️ **`pd_axil_regs` 是唯一的例外**（B09 已记录）。**改进方向：把 DD 或 FILTER 的写法移植到 FEATURE。**

### 2.3 写寄存器实现（第 228–296 行）

```
253            if (wr_fire) begin
254                case (waddr)
255                    A_CTRL: begin
256                        if (wstrb[0]) o_acq_en <= wdata[0];
257                        if (wstrb[0] && wdata[1]) o_sw_rst <= 1'b1;
258                        if (wstrb[0] && wdata[2]) begin
259                            o_snap_start <= 1'b1;
260                        end
261                    end
262                    A_SNAP_BASE: o_snap_base <= wdata;
263                    A_SNAP_SIZE: o_snap_size <= wdata;
264                    A_FZ_CTRL: begin
265                        if (wstrb[0] && wdata[0]) begin
266                            o_freeze_trig <= 1'b1;
267                        end
268                        if (wstrb[0] && wdata[1]) begin
269                            o_freeze_resume <= 1'b1;
270                        end
271                    end
272                    A_SLOT_CTRL: begin
273                        // AUTO_EN is an RW bit.  Command writes must not
274                        // accidentally clear it merely because bit0 is zero.
275                        // Combine mode changes and W1P commands only in
276                        // separate AXI-Lite writes.
277                        if (wstrb[0] && (wdata[7:4] == 4'b0) &&
278                            !(wstrb[1] && (|wdata[12:8])))
279                            o_auto_snap_en <= wdata[0];
280                        if (wstrb[0]) o_slot_lock <= wdata[7:4];
281                        if (wstrb[1]) o_slot_release <= wdata[11:8];
282                        if (wstrb[1] && wdata[12]) o_slot_status_clear <= 1'b1;
283                    end
284                    A_SNAP_TRIG_CTRL: begin
285                        if (wstrb[0]) begin
286                            o_event_trig_en   <= wdata[0];
287                            o_event_trig_mask <= wdata[4:1];
288                        end
289                        if (wstrb[1] && wdata[8])
290                            o_event_trig_stat_clear <= 1'b1;
291                    end
292                    default: ;
293                endcase
294            end
```
⭐ **注意每个寄存器都带 `wstrb` 判断**（与 `pd_axil_regs` 的"整字覆盖"不同）→ **本从机支持字节选通** ✓ 更符合 AXI 规范。

⭐⭐ **第 272–283 行（`SLOT_CTRL`）是本文件最微妙的一段**，注释第 273–276 行说得很好：
> **`AUTO_EN` 是一个 RW 位。命令写不得仅仅因为 bit0 为零就意外把它清掉。模式变更与 W1P 命令只能在分开的 AXI-Lite 写里组合。**

- ⭐ **问题背景**：`SLOT_CTRL` 同时承载两类操作：
  - **RW 位**：`[0] auto_snap_en`（模式开关，需要保持）；
  - **W1P 命令**：`[7:4] lock`、`[11:8] release`、`[12] status_clear`（一次性动作）。
- ⚠️ **如果写成 `o_auto_snap_en <= wdata[0]` 无条件**，那么"写 `lock` 命令"（`wdata[0]` 通常为 0）会**顺手把 `auto_snap_en` 清成 0** → **四槽模式被意外关闭**！**这是一个非常隐蔽的功能破坏。**
- ⭐ **修法（第 277–279 行的三条件）**：
  ```
  if (wstrb[0] && (wdata[7:4] == 4'b0) && !(wstrb[1] && (|wdata[12:8])))
      o_auto_snap_en <= wdata[0];
  ```
  - `wstrb[0]`：本次写覆盖了低字节；
  - `wdata[7:4] == 4'b0`：**本次没有同时下发 `lock` 命令**（`lock` 字段为空才认作"纯粹的模式写"）；
  - `!(wstrb[1] && (|wdata[12:8]))`：**本次也没有同时下发 `release` 或 `status_clear`**（`wstrb[1]` 覆盖第 2 字节时，检查 `release`/`status_clear` 是否全 0）。
  - ⭐ **三个条件同时成立才更新 `auto_snap_en`** → 保证"下发纯命令写不会误改模式位" ✓。
- ⭐ **`o_slot_lock`/`o_slot_release` 的写法**（第 280–281 行）：**直接赋值 `wdata[7:4]`/`wdata[11:8]`**，所以是"**矢量 W1P**"（一次可以锁多个槽）✓ 与 `pd_ddr_slot_mgr` 的 `i_slot_lock[3:0]` 匹配。
  - ⚠️ 但注意第 246–252 行**每拍先清零**（`o_slot_lock <= 4'b0;`）→ 所以写进去的值**只持续一拍** → **效果等价于"W1P 脉冲矢量"** ✓ 正确。

### 2.4 读寄存器与状态位的组装（第 314–371 行）

```
316                    A_STATUS:    s_axi_rdata <= {22'd0, i_snapshot_cfg_err, i_copy_err,
317                                                 i_copy_pending, copy_done_sticky, i_err,
318                                                 i_snap_overrun, i_freeze_done, i_hiwm,
319                                                 i_copy_busy, o_acq_en};
```
⭐ **`DDR_STATUS` 的位组装**（从高位到低位）：
```
[9] i_snapshot_cfg_err
[8] i_copy_err
[7] i_copy_pending
[6] copy_done_sticky
[5] i_err            ← = ring_err | ring_ovf | copy_err | snapshot_cfg_err
[4] i_snap_overrun
[3] i_freeze_done
[2] i_hiwm
[1] i_copy_busy
[0] o_acq_en
```
⭐ **注意 `i_err`（bit5）里已经包含了 `i_copy_err`（bit8）与 `i_snapshot_cfg_err`（bit9）** → **所以读 `DDR_STATUS[5]` 时，如果 bit8/bit9 也是 1，就能区分出"是拷贝错误还是配置错误"** ✓ 这正是 A08 §5.2 说的"`copy_err` 与 `snapshot_cfg_err` 另有专门的位"。
- ⚠️ **但 `ring_err` 与 `ring_ovf` 没有专门的位** → 这就是**唯一无法区分的一对**。

```
331                    A_FZ_CTRL:   s_axi_rdata <= {17'd0, i_snapshot_cfg_err, i_copy_err,
332                                                 i_snap_overrun, i_copy_pending,
333                                                 copy_done_sticky, i_copy_busy,
334                                                 i_freeze_done, 7'd0, o_acq_en};
```
- `FREEZE_CTRL` 读回位组装：`[15] cfg_err [14] copy_err [13] snap_overrun [12] copy_pending [11] copy_done_sticky [10] copy_busy [9] freeze_done [8:2] 保留 [1:0]...`，注意最后一个 `7'd0, o_acq_en` → **`[0] = acq_en`**。但**注释第 28 行说 `FREEZE_CTRL` 的只读位是 `[8]done`** —— ⚠️ **实际 `freeze_done` 落在 `[9]`**（因为 `{..., i_freeze_done, 7'd0, o_acq_en}` → `i_freeze_done` 在 `[9]`）。
  - ⚠️ **【观察】注释（第 28 行）与实际实现（第 334 行）不符**：注释说 `[8]done`，实现是 `[9]done`。**计数一下**：`o_acq_en` 占 [0]，`7'd0` 占 [7:1]，`i_freeze_done` 占 [8]，`i_copy_busy` 占 [9]…… 
  - 让我重数：从低位开始 `{17'd0, i_snapshot_cfg_err, i_copy_err, i_snap_overrun, i_copy_pending, copy_done_sticky, i_copy_busy, i_freeze_done, 7'd0, o_acq_en}`：
    - `o_acq_en` → [0]
    - `7'd0` → [7:1]
    - `i_freeze_done` → [8] ✓ **与注释一致！**
    - `i_copy_busy` → [9]
    - `copy_done_sticky` → [10]
    - `i_copy_pending` → [11]
    - `i_snap_overrun` → [12]
    - `i_copy_err` → [13]
    - `i_snapshot_cfg_err` → [14]
    - `17'd0` → [31:15]
  ✓ **所以 `[8] = freeze_done` 是对的**，注释正确。**我上面的计数错了，纠正。**
- ⭐ **这提醒了一件事：读位域必须从低位往高位逐个数**（`17'd0` 是高位保留、`7'd0` 是中间保留），**不能靠"看起来像"**。这是本工程里"位域一致性"最容易出错的地方（与事件包位位置同理）。

```
353                    A_SLOT0_BASE:s_axi_rdata <= `DDR_SLOT0_BASE;
354                    A_SLOT0_LEN:s_axi_rdata <= i_slot0_len;
355                    A_SLOT0_SEQ:s_axi_rdata <= i_slot0_seq;
356                    A_SLOT0_FLAGS:s_axi_rdata <= {29'd0, i_slot_locked[0], i_slot_busy[0], i_slot_valid[0]};
```
- **四槽描述符**：每槽 4 个寄存器（BASE/LEN/SEQ/FLAGS），步长 `0x10` ✓（与 `pd_hw_map.h:48-51` 的 `PD_SLOT_BASE_OFF(n)` 公式一致）。
- `FLAGS` = `{29'd0, locked, busy, valid}` → `[2]locked [1]busy [0]valid` ✓。

---

## 3. `pd_ddr_bd_adapter.v`（280 行）—— IP Integrator 适配层

### 3.1 它存在的理由（第 1–8 行）

```
3  // pd_ddr_bd_adapter.v -- pd_ddr_wr_top 的 IP Integrator 接口适配层
5  // 业务 RTL 保持扁平端口，所有 X_INTERFACE 属性集中在本文件。这样 Block
6  // Design 的 AXI 自动连线是显式、可检查的，同时不会把 Vivado 元数据渗入
7  // 环形写入和快照业务逻辑。
```
⭐ **这是一个值得学的架构模式**：**把"给工具看的元数据"和"业务逻辑"分开放在两个文件里。**

- **背景**：Vivado 的 IP Integrator 需要 `(* X_INTERFACE_INFO = ... *)` 之类的属性才能自动识别 AXI 接口（否则 20 多条散线要手工连）。
- **如果直接把这些属性写在 `pd_ddr_wr_top` 里**：
  - 业务逻辑文件会**被大量元数据注释淹没**，可读性下降；
  - 元数据是"工具相关的"（换工具就没用），与业务逻辑无关；
  - 模拟器/仿真时会解析这些属性（虽然忽略内容，但增加噪音）。
- **分开后**：
  - `pd_ddr_wr_top.v` 保持**纯扁平端口 + 纯业务逻辑**（可独立仿真、可移植）；
  - `pd_ddr_bd_adapter.v` 只做"端口转发 + 属性标注"（**零逻辑**）。
- ⭐ **本文件确实是"零逻辑"的**：第 191–278 行全是 `pd_ddr_wr_top` 的端口转发。**唯一的"逻辑"是三个 unused 信号的吸收**（见 3.2）。

### 3.2 未使用信号的显式吸收（第 182–189 行）

```
182    // AWPROT/ARPROT are AXI4-Lite metadata. pd_ddr_axil has no policy on them
183    // and therefore intentionally ignores these two adapter-only inputs.
184    wire unused_prot = &{1'b0, s_axi_awprot, s_axi_arprot};
185    // PS7 HP ports expose no USER sideband. Keep DataMover's fixed-width
186    // USER outputs local instead of advertising a mismatched AXI interface.
187    wire [3:0] m_axi_wr_awuser_i;
188    wire [3:0] m_axi_rd_aruser_i;
189    wire [3:0] m_axi_cw_awuser_i;
```
⭐ **两处"有意的处理"**：
1. **第 182–184 行（`AWPROT`/`ARPROT`）**：
   - AXI 规范里 `AWPROT`/`ARPROT` 是"保护属性"（指示访问是特权/安全/指令/数据）。`pd_ddr_axil` **没有实现任何保护策略**，所以这两个输入被忽略。
   - ⭐ **第 184 行 `wire unused_prot = &{1'b0, s_axi_awprot, s_axi_arprot};` 的写法很讲究**：
     ```
     `1'b0` 与两个 PROT 信号拼接后做归约与 → 因为拼了 0，结果**恒为 0**；
     ```
     - 目的是**构造一个"用到了这些信号但结果恒 0"的表达式**，从而**避免综合器报"unused signal"警告**；
     - ⭐ **这是一个"用显式 dummy 表达式抑制警告"的技巧**（比加 `(* keep = "false" *)` 更明确）。
     - ⚠️ **但它有副作用**：工具会**推断出整条归约与逻辑**（虽然被常数折叠掉）。**更干净的做法是直接写注释说明不使用**，或用 `$display` 在仿真时提示。
2. **第 185–189 行（`AWUSER`/`ARUSER`）**：
   - DataMover 的 AXI 接口带 4 位 `USER` 侧带信号，但 **PS7 的 HP 端口不提供 `USER`**。
   - ⭐ **如果把这个 4 位 USER 端口声明成 AXI 接口的一部分，而 BD 里的对端没有，就会出现"接口维度不匹配"**（这是 B09 里"接口维度退化"类问题的一种）。
   - **处理方式**：**把 `awuser` 声明为普通 wire（不是 AXI 接口的一部分），并把它们接成内部未连接**（第 232、250、265 行都是 `.m_axi_wr_awuser(m_axi_wr_awuser_i)` 等）→ **"局部化"这些信号** ✓。
   - ⭐ **这是一个"接口对不齐时如何收尾"的实例**：把不匹配的那一路**降级为普通信号并本地吸收**，而不是强行声明成接口。

### 3.3 X_INTERFACE 属性的写法（第 16–179 行）

```
16     (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME CLK, FREQ_HZ 130000000, ASSOCIATED_BUSIF s_axi:m_axi_wr:m_axi_rd:m_axi_cw, ASSOCIATED_RESET rst_n" *)
17     (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
18     input  wire                      clk,
```
- ⭐ **每一组 AXI 接口都有一对属性**：
  - **`X_INTERFACE_PARAMETER`**：整个接口的参数（接口名、协议、地址宽度、数据宽度、频率、**关联的时钟与复位**）；
  - **`X_INTERFACE_INFO`**：**逐个信号**的归属声明（哪个接口的哪个信号）。
- ⭐ **`ASSOCIATED_BUSIF`（第 16 行）**：声明"这个时钟关联哪些总线接口"。**这是 BD 自动连线与时钟域推断的依据** —— 它告诉工具"`clk` 就是 `s_axi`/`m_axi_wr`/`m_axi_rd`/`m_axi_cw` 四个总线接口的时钟"。
  - ⚠️ **注意 `ASSOCIATED_RESET rst_n`**：声明复位关联。
  - ⚠️ **但本模块的 `adc_clk`（第 22–24 行）没有 `ASSOCIATED_BUSIF`**（因为它不关联任何 AXI 总线，只关联 ADC 数据）✓ 正确。
- ⭐ **`XIL_INTERFACENAME`（`S_AXI` / `M_AXI_WR` / `M_AXI_RD` / `M_AXI_CW`）要与 BD 里看到的接口名一致** —— 否则 BD 里会看到"未命名接口"或"接口名不匹配"。

```
76     (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME M_AXI_WR, PROTOCOL AXI4, ADDR_WIDTH 32, DATA_WIDTH 64, ID_WIDTH 4, READ_WRITE_MODE WRITE_ONLY" *)
...
112    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME M_AXI_RD, PROTOCOL AXI4, ADDR_WIDTH 32, DATA_WIDTH 64, ID_WIDTH 4, READ_WRITE_MODE READ_ONLY" *)
```
- ⭐ **`READ_WRITE_MODE`（`WRITE_ONLY`/`READ_ONLY`）是一个重要的声明**：
  - `M_AXI_WR` 只有 AW/W/B → `WRITE_ONLY`；
  - `M_AXI_RD` 只有 AR/R → `READ_ONLY`；
  - **声明正确能让 BD 里的 SmartConnect 知道"这个主口只需要写通道/读通道"**，从而**只分配需要的资源**（SmartConnect 会据此减少内部通路）。
  - ⚠️ **如果声明成 `READ_WRITE` 而实际只有写通道，BD 可能报"接口端口缺失"或生成多余的逻辑。**

⭐ **一句话总结本模块的价值**：
> **它把"给工具看的接口契约"集中在一个零逻辑文件里，使业务 RTL 保持"可仿真、可移植、可读"。** 当需要换工具（例如改用 Verilog 参数化 IP 而不是 IP Integrator）时，直接删掉这个文件即可。

---

## 4. 本篇易错点小结

1. ⭐ **不同 Xilinx IP 的复位极性不同**：`fifo_generator` 的 `rst` **高有效**，AXI IP 通常**低有效**——接错会"永不复位"或"永远复位"。
2. ⚠️ **`sw_rst` 是单拍脉冲 → `ddr_rst_n` 只低 7.7 ns**，对 DataMover 可能太窄（未闭合风险）。
3. ⚠️ **`ddr_rst_n` 同时驱动 `pd_ddr_slot_mgr`** → 软复位会把所有槽静默丢回 `FREE`。
4. ⭐ **Path A / Path B 必须各有一条独立异步 FIFO**（扇出边界）：否则 DDR 反压会拖慢实时特征链的节拍与顺序。
5. ⭐ **手动拷贝优先级最高**（`!i_manual_copy_launch` 在槽管理器的启动条件里）。
6. ⭐ **三条拷贝路径的启动条件都含 `ring_write_idle`**（等写落地）+ `!copy_busy`。
7. ⚠️ **三条路径若同拍成立，描述符会取槽的**（`slot_copy_start` 优先）→ 依赖上游互斥（实际不会发生，但属隐含假设）。
8. ⭐ **手动 `o_snap_start` 不受 `waiting_resume` 限制**：PS 连续写两次 `FREEZE_CTRL=1` 会导致 `slot_req_overflow`（丢快照）。
9. ⭐ **`snap_base`/`snap_size` 是 PS 可写的，必须做范围检查**（防止把快照写到环形区）。
10. ⭐ **`slot_snapshot_ready` 电平保持**（把单拍 `copy_done` 变成中断可观测），**不改变外部 irq 端口契约**。
11. ⭐⭐ **`dbg_ddr` 算好了但 BD 里没连线** → 不可观测且被综合优化掉。**引到 `0xA4` 就能零逻辑分开 `ring_err` 与 `ring_ovf`。**
12. ⭐ **`pd_ddr_axil` 用页内偏移解码，BD 映射到 `0x4001_0000`** → 契约里的 `0x1028` 实际是 `0x4001_0028`。
13. ⭐ **`SLOT_CTRL` 的 `auto_snap_en`（RW）与 lock/release/status_clear（W1P）必须"分开写"**，否则下达命令会误清模式位（代码已用三条件保护）。
14. ⭐ **`pd_ddr_axil` 与 `pd_filter_chain` 都支持 AW/W 独立接收，只有 `pd_axil_regs` 不支持** —— 三种风格并存。
15. ⭐ **读位域必须从低位往高位逐个数**（`FREEZE_CTRL` 的 `freeze_done` 在 `[8]`，需数过 `7'd0` 与 `o_acq_en`）。
16. ⭐ **`pd_ddr_bd_adapter` 是"零逻辑适配层"**：业务 RTL 保持扁平端口，X_INTERFACE 属性集中在这里。
17. **`READ_WRITE_MODE` 声明要与实际通道匹配**（`WRITE_ONLY`/`READ_ONLY`），否则 BD 会抱怨端口缺失。
18. **PS7 的 HP 口没有 `USER` 侧带** → DataMover 的 `awuser` 要"降级为普通 wire 本地吸收"，不要声明成 AXI 接口的一部分。

---

**本卷下一篇**：`A11_仿真测试平台.md` —— 4 个 testbench 的判据设计与"激励为什么要用 MATLAB 生成的 .mem"。
