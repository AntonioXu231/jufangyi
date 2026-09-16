# pd_ddr_slot_mgr 设计决议记录

- **日期**：2026-09-16
- **工程**：`pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916`
- **范围**：Phase 5 —— PL 侧四槽 DDR 快照管理器（`pd_ddr_slot_mgr.v`）
- **状态**：D1/D2/D4/D5 已定案；D3 已给建议待最终确认
- **本文件不含任何 RTL 实现**，只冻结接口事实与约束，作为改动 `pd_ddr_defines.vh` 前的文档前提

---

## 0. 背景结论

交接文档建议的四槽基址 `0x2000_0000 / 0x20C0_0000 / 0x2180_0000 / 0x2240_0000`
**全部 mod 24 = 8**，违反 `pd_ddr_defines.vh` 头部的对齐铁律
（基址与长度均须为 `LCM(4096,24) = 12288 = 0x3000` 的整数倍）。

后果：`pd_ddr_snap_copy` 内 `pd_align24_check(i_dst_addr)` → `start_valid = 0`
→ 直接置 `o_err` 且状态机不启动，命令通道一条命令都不会发出。

同一约束在契约中也有依据：
《接口契约 v3.0》§6 —— `SNAP_BASE`「必须位于编译期快照分区且 **24 字节对齐**」，
`SNAP_SIZE`「必须为 24 字节整数倍」。

---

## 1. D1（已定案）：四槽地址图

采纳「基址整体 `+0x1000`」最小改动方案。步长保持 12 MiB = `0x00C0_0000`。

| 槽 | 基址 | 末地址 | 容量 |
|---|---|---|---|
| slot0 | `0x2000_1000` | `0x20C0_0FFF` | 12 MiB |
| slot1 | `0x20C0_1000` | `0x2180_0FFF` | 12 MiB |
| slot2 | `0x2180_1000` | `0x2240_0FFF` | 12 MiB |
| slot3 | `0x2240_1000` | `0x2300_0FFF` | 12 MiB |

- 区域总跨度：`0x2000_1000` – `0x2300_0FFF`（48 MiB），位于 1 GB DDR 范围内。
- 对齐校验：四个基址 `mod 12288 = 0`（`mod 24 = 0`、`mod 4096 = 0`、`mod 8 = 0`）全部满足。
- 容量校验：单槽 12 MiB = 12,582,912 B。
  - 26 MSPS：1 个 50 Hz 周期 = 520,000 样本 × 6 B = 3,120,000 B ≈ 2.98 MiB
  - 65 MSPS：1,300,000 样本 × 6 B = 7,800,000 B ≈ 7.44 MiB
  - 两档均单槽可容，符合交接文档「一槽容纳一个完整工频周期并留余量」。
- 未采纳的备选：整区上移至 `0x3000_0000`（同样满足 `0x3000` 对齐且更整齐），
  因改动面更大而未选。

**文档先行要求**：`pd_ddr_defines.vh` 头部声明「改动须先改文档再动代码」，
且契约 v3.0 全文未定义四槽分区 → 本区域属新的接口决策，PS 侧需同步知悉。

---

## 2. D2（已定案，方案 B）：合法性检查分工

- `pd_ddr_slot_mgr` 自行输出 `o_cfg_err`，负责**槽区**的合法性判断。
- 顶层汇总上报，形如 `snapshot_cfg_err = legacy_cfg_err & ~auto_mode | slot_cfg_err`。

`o_cfg_err` 需要覆盖的最小集合：

1. **`freeze_len` ≤ 单槽容量（12 MiB）** —— 这条是硬需求。
   `pd_ddr_snap_copy` 的 `start_valid` 只校验「源地址落在环形区内」与三处 24B 对齐，
   **不校验目标地址上界**。若 `freeze_len` 超槽，数据会静默越界写入相邻槽，
   属数据损坏级错误。
2. `freeze_len` 为 24 B 整数倍（`snap_copy` 内已查，但提前查可获得更清晰的错误归因）。
3. 选中槽的基址位于槽区内且 `0x3000` 对齐。
4. `selected_base + freeze_len` 不越过该槽上界。

**待确认的开口**：`~auto_mode` 门控会同时解除 legacy 手动调试路径
（`o_snap_start`）的既有保护，因为 `manual_copy_launch` 当前复用同一个
`snapshot_cfg_err`。需明确手动路径的保护如何保留。

---

## 3. D3（建议，待最终确认）：freeze 事件的传递形式

`pd_ddr_ring_wr` 的 `o_freeze_done` 是**电平**（置 1 后保持，直到
`i_freeze_trig` / `i_freeze_resume`），因此必须做边沿检测。

「顶层生成上升沿脉冲」方向正确，但**输出不应为 1 拍脉冲**，理由有两条：

1. `pd_ddr_snap_copy` 的 `i_start` **仅在 `S_IDLE` 生效**，`o_busy = 1` 期间
   静默丢弃、不排队、不报错。若脉冲在上一轮拷贝未结束时到达，
   一份完整有效的冻结快照会**无痕消失**。
2. 单拍脉冲把「事件是否发生」与「此刻能否处理」耦合进同一个信号，
   导致「应等待 `copy_busy`/`ring_write_idle` 结束的」与「按 D4 应丢弃的」无法区分。

单次拷贝耗时估算（`CHUNK_BLOCKS = 256`，严格串行，每片 6144 B）：
26 MSPS 一次约 508 片、65 MSPS 约 1270 片，按 768 beat/片估算约 3–8 ms，
通常小于 20 ms 工频周期，但受 HP1 带宽与 DDR 刷新影响可能拉长 ——
即「偶尔重叠」而非「从不重叠」，所以必须有等待能力。

两种落地形式：

- **形式 α（推荐）**：顶层输出**置位型电平** `freeze_req`
  （`freeze_done` 上升沿置位，launch 或 resume 时清除），
  与顶层现有 `auto_copy_pending` 语义完全同构。
- **形式 β**：顶层直连 `freeze_done` 电平，边沿检测与 pending 锁存放入
  `pd_ddr_slot_mgr` 内部，使「何谓一次新冻结」的语义完全归属 slot_mgr。

两者区别仅在边沿检测代码位于哪一侧，均需保证 pending 可被等待清掉。

---

## 4. D4（已定案）：全槽不可用时丢弃

四槽全部处于 VALID（或锁定）时，**丢弃本次冻结快照**，置粘滞 full 标志，
不覆盖任何有效槽、不挂起 pending。

- 粘滞标志的清除方式需与现有 RTL 风格一致（现有代码明确要求错误粘滞到软件确认）。
- 说明：丢弃意味着该工频周期的数据不入槽；`pd_ddr_ring_wr` 的 `o_snap_overrun`
  与 `hiwm` 可作为旁证观察。

---

## 5. D5（已定案，待补规则）：增加 slot_lock

在 `SLOT_CTRL` 中除 `auto_snap_en` 与 W1P 的 `slot_release[n]` 外，
另加入 `slot_lock[n]`。

**待明确的一条交互规则**（位域宽度由它决定）：

- `slot_lock[n] = 1` 时 `slot_release[n]` 是**忽略**、**拒绝并报错**、还是**允许**？
- `slot_lock[n]` 是「PS 读取期间的互斥保护」（lock → 读 → release），
  还是「长期钉住不允许自动快照复用」？两者可并存，但必须在文档中写清。

---

## 6. 三条必须遵守的硬约束

1. **对齐铁律**：任何 DDR 分区基址与长度都必须是 `0x3000`（12288）的整数倍。
   依据：DataMover 关 DRE 的 8 B 对齐 + 契约 §3.3 的 24 B 块对齐。
2. **`dm_cp` 独占裁决**：全设计只有一份 `pd_ddr_snap_copy` / `dm_cp`。
   `auto_snap_en = 1` 时，顶层必须屏蔽 legacy `auto_copy_launch`
   （见 `pd_ddr_wr_top.v` 第 536 行），把启动权与
   `i_src_addr` / `i_dst_addr` / `i_len_bytes` 的描述符所有权一并交给 slot_mgr，
   否则两条自动路径会争抢同一份 DataMover，且竞争中的 `i_start` 会被静默丢弃。
   legacy 手动路径（`o_snap_start`）按交接文档要求保留。
3. **`i_start` 静默丢弃**：任何启动 `pd_ddr_snap_copy` 的路径，
   都必须在 `!o_busy` 且 `ring_write_idle` 且配置合法时才发 start。
   参照 `pd_ddr_wr_top.v` 第 536–540 行的四条件写法。

---

## 7. 相关既有缺陷（非本次引入，供决策参考）

《接口契约 v3.0》§9 规定「保留地址**读返回 0**、写忽略」，
但 `pd_ddr_axil.v` 第 246 行 `default: s_axi_rdata <= 32'hDEAD_BEEF;`。
新增 `0x4C` 起的 SLOT 寄存器组时，需一并决定保留区行为。
（现有偏移 `0x00`–`0x48` 不得改动。）

寄存器偏移分歧：交接文档列到 `0x4C/0x50/0x54/0x58` 后跳至 `0x60` onward，
`0x5C` 未说明，需确认是否为对齐 16 B 而有意留空。

---

## 8. 待办顺序

1. 确认 D3 形式（α / β）与 D5 的 lock 交互规则。
2. 补齐本文档第 2 节的 legacy 手动路径保护开口。
3. 按「文档先行」更新接口契约（v3.1 增补或 v3.0 附录）与变更记录后，
   方可改动 `pd_ddr_defines.vh` 增加槽区宏。
4. 由开发者本人编写 `pd_ddr_slot_mgr.v`：先仅模块接口，编译通过后再逐段补逻辑。

---

## 9. v2 增补（2026-09-16 第二轮）

来源：`pd_ddr_slot_mgr_decisions_reply_20260916.md`（Codex 回复）+ 本轮代码评审修正。

### 9.1 已确认项（无异议）

| 项 | 结论 |
|---|---|
| D1 | 采纳 `+0x1000` 地址图，RTL 不做上取整或地址补偿 |
| D2 | 方案 B；三条路径门控分离 —— auto 槽用 `slot_cfg_err`，legacy 自动在 `auto_snap_en=1` 时禁止发起，legacy 手动恒用 `legacy_cfg_err` |
| D3 | 形式 α；`freeze_req` 为置位保持信号，`copy_start` 单拍 + `req_ack` 回执后清零 |
| D4 | 全槽 VALID 或 LOCKED 时立即丢弃：不等待、不延长冻结窗口、不复用旧槽 |
| D5 | 严格两段式 `VALID --lock--> LOCKED --release--> FREE`；对非 LOCKED 槽 release 一律拒绝 + 粘滞错误；LOCKED 槽绝不参与自动分配 |
| 寄存器 | `0x5C` 有意保留，用于把槽描述符对齐到 `0x60`；保留地址读 0 写忽略，与契约 §9 一致 |

`snapshot_cfg_err = auto_snap_en ? slot_cfg_err : legacy_cfg_err`
**仅用于向软件汇总上报**；启动门控必须按描述符所有者分别判断
（形成两条独立 wire），不得直接用该 mux 门控手动路径。

顺带修正既有契约不一致：`pd_ddr_axil.v` 第 246 行
`default: 32'hDEAD_BEEF` 违反契约 §9「保留地址读返回 0」，
扩展本模块时一并改为返回 0。注意该 `default` 覆盖整页未映射偏移，
属**全局行为变更**，须记入变更记录。

### 9.2 评审修正（四条，写 RTL 前必须钉死）

**(1) `slot_lock[n]` 必须是保位写，不能是脉冲。**
原回复写作「W1P/W1S」，两者语义互斥。锁状态要跨多拍保持，故：

- `slot_lock[n]` = **W1S**（写 1 置位并保持）
- `slot_release[n]` = **W1P**（写 1 单拍脉冲，触发 LOCKED → FREE）
- lock 的**唯一**清除路径是 release，不存在独立的 unlock。

**(2) pending 期间必须锁存描述符副本 —— 否则后果比丢弃更坏。**
`pd_ddr_ring_wr` 的 `o_freeze_base` / `o_freeze_len` 是**共享寄存器**，
在下一次 `freeze_trig` 后的第 2 个周期边界会被重新写入（第 391–392 行）；
同一时刻 `o_freeze_done` 被清零（第 365、373 行），之后才重新拉高。

因此若旧 pending 在此窗口内仍被保留，它引用的描述符已被下一次冻结
**静默替换**，最终的结果是「请求 A 的元数据 + 数据 B 的载荷」写入 DDR ——
比直接丢弃更糟，且软件无法察觉。
→ 要求：在 `freeze_rise` 当拍把 `freeze_base` / `freeze_len`
锁进 slot_mgr 自有寄存器，此后不再看输入端口。

**(3) `drop_count`、`req_overflow`、`drop_busy` 需要明确的寄存器位域归属。**
`0x4C`–`0x58` 四个字已分配给 `SLOT_CTRL` / `SLOT_STATUS` / `SLOT_LAST` /
`SLOT_SEQ`，`0x5C` 又要留给 16 B 对齐。新增的计数与粘滞标志目前**没有家**。
必须在写 AXI-Lite 接口前明确：塞进上述四字的空闲位，还是另外占用保留地址。

**(4) 「手动优先」的实现位置未定，而它决定端口表。**
两种落地：

- 甲：slot_mgr 增加一个「手动待发」输入，由它自行避让；
- 乙：顶层对 `copy_start` 做与门 —— **有缺陷**，slot_mgr 会误认为自己已获准
  启动，从而错误清除 pending。

→ 建议改为 `o_copy_start`（请求）+ `i_copy_grant`（获准）两线握手，
或明确让 slot_mgr 感知手动待发。二者选一后再定端口。

### 9.3 一条简化建议

顶层已有的 `auto_copy_pending`（`pd_ddr_wr_top.v` 第 496–512 行）
与新的 `freeze_req` 是同一件事 —— 同一上升沿触发、同一清除条件。
不要实现两份：让同一个信号按 `auto_snap_en` 分发给 legacy 自动路径或 slot_mgr。

### 9.4 状态编码提示

在 D5 的严格语义下，每个槽真正需要的信息只有 2 bit：

- `valid[n]`：槽内有完整数据
- `locked[n]`：PS 已取得读所有权

不变量 `locked ⇒ valid` 恒成立（FREE 槽不允许 lock），可直接写成断言。

| 对外状态 | valid | locked | 可被自动分配 |
|---|---|---|---|
| FREE | 0 | 0 | 是 |
| VALID | 1 | 0 | 否 |
| LOCKED | 1 | 1 | 否 |

`RESERVED` / `COPYING` 全设计至多一个，用一个 `busy_slot[1:0]` 索引表达即可，
不必给每个槽保留一份「正在拷贝」编码。写端口时无需处理，写状态声明时再定。

### 9.5 执行顺序的现实约束

Codex 回复的第 1 步要求更新接口契约与变更记录，但：

- `接口契约v3.0.md` 与 `版本管理/` 位于**本工程目录之外**，
  且契约是「双方确认基线」，单方修改不合流程 → 需明确授权或双方确认后单独执行。
- 工程内已有本决议记录 + Codex 回复，可视为满足「改代码前先有书面依据」。

→ 实际执行顺序调整为：先闭环工程内可完成的部分
（defines 宏 → 骨架 → 编译检查 → 分段补逻辑），
契约与变更记录待授权后补做。
