# A07 · 特征寄存器与 IP 顶层

覆盖文件：
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_axil_regs.v`（615 行）—— 完整的 AXI4-Lite 寄存器文件
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_feature_top.v`（379 行）—— 4 通道实例化 + 仲裁 + 计数
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_feature_sys_top.v`（211 行）—— 加 CDC 的对外顶层

---

## 1. `pd_axil_regs.v` —— 寄存器地图（先看地址表，再看实现）

### 1.1 地址空间划分（第 4–13 行）

```
4  // 地址空间 (byte address, 32bit 对齐)
5  //   !! 区域码取 addr[15:12], 因此各区必须按 4KB 边界对齐 !!
6  //   0x0000 ~ 0x07FF : 通道寄存器区, 通道 i 基址 = 0x0000 + i*0x80 (32 个字)
7  //   0x1000 ~ 0x1FFF : 全局寄存器区
8  //   0x4000 ~ 0x7FFF : PRPD 图谱区, 通道 i 基址 = 0x4000 + i*0x1000 (1024 字)
10 //   [设计教训] 早期版本把全局区放在 0x0800, 但 0x0800 的 addr[15:12] = 0x0,
11 //   与通道区的区域码相同, 导致全局访问被误译为通道寄存器访问(读 VERSION 实际
12 //   读到了 r_scale)。故全局区必须落在独立的 4KB 页。
```
⭐ **第 10–12 行是一段真实设计教训**：
- 早期版本把全局区放在 `0x0800`；
- 但地址解码用的是 **`addr[15:12]`（高 4 位 = 4 KB 页号）**，而 `0x0800` 的高 4 位是 `0x0`，**与通道区相同**；
- 结果：访问"全局 VERSION 寄存器"实际读到了"通道的 `r_scale`"——**一个静默的地址别名 bug**。
- **修法**：全局区必须落在**独立的 4 KB 页**（`0x1000`，高 4 位 = `0x1`）。
- ⭐ **教训：区域解码用高位时，各区尺寸必须是页大小的整数倍且互不重叠**。这是"地址别名"类 bug 的典型成因。

**地址空间总表**：

| 区域码 `addr[15:12]` | 范围 | 内容 | 每通道步长 |
|---|---|---|---|
| `0x0` | `0x0000`–`0x07FF` | 通道寄存器 | `0x80`（32 个字） |
| `0x1` | `0x1000`–`0x1FFF` | 全局寄存器 | — |
| `0x2` | `0x2000`–`0x21FF` | 计数器区（只读） | `0x80` |
| `0x4`–`0x4+NUM_CH-1` | `0x4000`– | PRPD 图谱（只读） | `0x1000` |

⚠️ **注意 `0x3` 段未使用**（`0x3000`–`0x3FFF` 会落到 `default` 分支返回 0）。

### 1.2 通道寄存器区（第 14–41 行注释）

| 偏移 | 名称 | R/W | 位域 |
|---|---|---|---|
| `0x00` | CTRL | RW | `[0]enable [1]sw_rst(W1P) [2]auto_clear [3]ev_all [4]prpd_max [5]prpd_clear(W1P)` |
| `0x04` | CFG0 | RW | `[1:0]mode [31:16]phase_win(N ≤ 2048)` |
| `0x08` | PHASE_INC | RW | `2³²/N`（u 相位累加步长） |
| `0x0C` | THRESH | RW | `[11:0]` 门限（ADC 码） |
| `0x10` | SCALE | RW | Q8.8 视在电荷量标定系数（码→pC） |
| `0x14` | UPEAK | RW | Q8.8 试验电压峰值（用于 P） |
| `0x18` | DEADTIME | RW | `[15:0]` 死区（ADC 样本数，0=禁用） |
| `0x1C` | MEAS_CYC | RW | `[15:0]` 测量窗口长度（同步周期数，0=不自动快照） |
| `0x20` | STATUS | RO | `[0]sync_locked [1]meas_done_sticky [2]overflow` |
| `0x24` | SYNC_PER | RO | 实测同步周期 M；`f_sync = 20e6/M` |
| `0x28` | CNT_N | RO | 放电脉冲计数 n |
| `0x2C` | Q_MAX | RO | 最大视在电荷量 Q（pC） |
| `0x30` | SUM_ABS_LO | RO | `Σ\|q\|[31:0]` |
| `0x34` | SUM_ABS_HI | RO | `Σ\|q\|[63:32]` |
| `0x38` | SUM_QU_LO | RO | `Σq·u[31:0]` |
| `0x3C` | SUM_QU_HI | RO | `Σq·u[63:32]` |
| `0x40` | LIVE_CYC | RO | 自上次清零以来累积的周期数 |
| `0x44` | IRQ_STAT | W1C | `[0]meas_done [1]overflow` |
| `0x48` | IRQ_EN | RW | `[0]meas_done_en [1]overflow_en` |
| `0x4C` | REG0_CFG | RW | `[0]en0 [1]sem0` |
| `0x50` | REG0_PHASE | RW | `[23:12]phi1 [11:0]phi2` |
| `0x54` | REG0_Q | RW | `[31:16]q1 [15:0]q2`（Q8.8 pC） |
| `0x58` | REG1_PHASE | RW | 同上 |
| `0x5C` | REG1_Q | RW | 同上 |
| `0x60` | REG1_CFG | RW | `[0]en1 [1]sem1` |

⭐ **通道基址公式：`通道 i 基址 = 0x0000 + i × 0x80`**。所以四个通道的 `CTRL` 分别在 `0x000`、`0x080`、`0x100`、`0x180`；`THRESH` 在 `0x00C`、`0x08C`、`0x10C`、`0x18C`。
→ ⭐ **testbench `tb_pd_feature_dds.v:89-92` 正是按这个公式写的**：
```c
axil_write(16'h0000,32'h1); axil_write(16'h000C,PD_THRESH_LSB);   // ch0
axil_write(16'h0080,32'h1); axil_write(16'h008C,PD_THRESH_LSB);   // ch1
axil_write(16'h0100,32'h1); axil_write(16'h010C,PD_THRESH_LSB);   // ch2
axil_write(16'h0180,32'h1); axil_write(16'h018C,PD_THRESH_LSB);   // ch3
```
✓ 完全吻合。

⚠️ **【观察 · 字段顺序不一致】** 注意 REG0 与 REG1 的寄存器排布**不同**：
- REG0：`CFG`@`0x4C` → `PHASE`@`0x50` → `Q`@`0x54`（顺序：CFG/PHASE/Q）
- REG1：`PHASE`@`0x58` → `Q`@`0x5C` → `CFG`@`0x60`（顺序：PHASE/Q/CFG）

**即 `REG1_CFG` 排在最后（`0x60`），而 `REG0_CFG` 排在最前（`0x4C`）**。注释第 41 行自己承认："更新清单补充：原清单遗漏 REG1 使能寄存器" —— 也就是说 `REG1_CFG` 是**后补的**，补在了末尾。**软件必须记住这个不对称**（B09 已记录）。

### 1.3 全局寄存器区（第 43–56 行注释）

| 偏移 | 名称 | R/W | 内容 |
|---|---|---|---|
| `0x1000` | GCTRL | RW | `[0]` 全局使能、`[1]` 全局软复位 |
| `0x1004` | GSTATUS | RO | `[NUM_CH-1:0]` 各通道 `sync_locked` |
| `0x1008` | GIRQ_STAT | W1C | 各通道中断聚合 |
| `0x100C` | GIRQ_EN | RW | — |
| `0x1010` | VERSION | RO | `0x0003_0000`（v3：新增 6 对门槛 + 12 计数器 + 帧快照） |
| `0x1014` | NUM_CH | RO | 通道数 |
| `0x1018` | FIFO_CNT | RO | 输出 FIFO 占用深度 |
| `0x101C` | EV_CNT_LO | RO | 已输出事件总数 `[31:0]` |
| `0x1020` | EV_CNT_HI | RO | 已输出事件总数 `[63:32]` |
| `0x1024`–`0x1038` | POS_THR[0..5] | RW | 6 个正档门槛（四通道共用） |
| `0x1040`–`0x1054` | NEG_THR[0..5] | RW | 6 个负档门槛（四通道共用） |
| `0x1058` | SAMPLE_RATE_HZ | RO | 固定返回 `26000000`（硬编码） |
| `0x105C` | APPLY_STATUS | RO | `bits3:0 pending_ch`、`bits7:4 applied_ch`、`bits31:16 apply_seq` |
| `0x1060` | APPLY | W1P | `[0]` 写 1 提交 shadow 配置 |

⭐ **第 53–56 行**：v3 新增的两组：
- **6 正 + 6 负档位门槛（四通道共用）**：即 12 档计数用的门槛。**注意是"四通道共用"** —— 而 `pd_feature_core` 有 4 份 `pos_thr_r` 副本，每份都从同一个全局寄存器取 ✓。
- **APPLY（`0x1060`）**：与通道里的 `cfg_apply` 对应。W1P（write-1-pulse）。

⚠️ **【观察】`0x1058 SAMPLE_RATE_HZ` 硬编码返回 `26000000`**（第 558 行）。这是"编译期常量"而不是从 BD 参数推导的。**若 BD 里改了 `SAMPLE_HZ`，这里不会跟着变** —— 软件会读到错误的采样率。B09 已记录。

### 1.4 计数器区与 PRPD 区（第 58–64 行注释）

| 偏移（通道 i 基址 = `0x2000 + i×0x80`） | 名称 | 内容 |
|---|---|---|
| `0x00`–`0x14` | POS_CNT[0..5] | 6 个 32 bit 正档计数 |
| `0x18`–`0x2C` | NEG_CNT[0..5] | 6 个 32 bit 负档计数 |
| `0x30` | FRAME_N_POS | 本帧正脉冲合计 |
| `0x34` | FRAME_N_NEG | 本帧负脉冲合计 |
| `0x38` | FRAME_AD_MAX | 本帧放电 AD 码最大值 |
| `0x3C` | FRAME_AD_MIN | 本帧放电 AD 码最小值 |
| `0x40` | FRAME_ID | 帧序号 |
| `0x44` | FRAME_STAT | `[0]` frame_valid（**只读**）`[1]` overflow |
| `0x48` | FRAME_ACK | **W1P**：`[0]` 确认已读（清 frame_valid）、`[1]` 记录丢帧（清 overflow） |

⚠️ **【观察】** 注释第 64 行仍写 `[0]frame_valid 读清`，但 **v3.0 契约已改为"只读"**（实现在第 629–630 行与 594–601 行）。**注释未同步更新**（B09 已记录）。

⭐ **PRPD 区**：通道 i 基址 = `0x4000 + i×0x1000`（1024 字 = 1024 个相位窗 × 16 bit）。**只读**。

### 1.5 AXI 握手实现（第 241–327 行）—— 与 `pd_filter_chain` 的对比

```
260            if (~axi_awready && S_AXI_AWVALID && S_AXI_WVALID && aw_en) begin
261                axi_awready <= 1'b1; aw_en <= 1'b0;
262            end else if (S_AXI_BREADY && axi_bvalid) begin
263                axi_awready <= 1'b0; aw_en <= 1'b1;
281        else if (~axi_wready && S_AXI_WVALID && S_AXI_AWVALID && aw_en)
282            axi_wready <= 1'b1;
287    wire slv_wren = axi_wready && S_AXI_WVALID && axi_awready && S_AXI_AWVALID;
```
⭐ **这里就是 B09 记录的那个"不合 AXI 规范"的写法**：
- 第 260 行：`AWREADY` 置位的条件包含 **`S_AXI_WVALID`**；
- 第 281 行：`WREADY` 置位的条件包含 **`S_AXI_AWVALID`**；
- 第 287 行：**写事务成立的条件是四个信号全部为 1**（`AWVALID && AWREADY && WVALID && WREADY`）。
- ⭐ **问题所在**：AXI 规范允许 **AW 和 W 通道完全独立**（任意顺序、任意间隔到达）。本实现要求"两者同时有效"才握手 → **如果主机先发 AW、过几拍才发 W，从机会一直不拉 `AWREADY`**。
  - ⭐ **为什么现在还能工作？** 因为上游是 Xilinx 的 AXI Interconnect（`axi_ic_ctrl`）与 `axi_register_slice`（`axi_rs_feature_ctrl_0`），它们**会缓存并同时呈现 AW 与 W**（"helpful host"行为）。**换个互连、换个主机（例如自己写的 AXI 主设备）、或者换 Vivado 版本改变互连策略，就可能挂死。**
  - ⭐ **对比 `pd_filter_chain.v:37-55`**（`aw_hold`/`w_hold` 机制）**做对了**：它显式缓存先到的通道。**同一工程里两种风格并存，说明这不是"统一的设计决策"，而是历史演化结果。**
- ⭐ **可复用结论**：写 AXI 从机时，**必须让 AW 与 W 独立握手**（用两个 hold 寄存器缓存先到者），否则就是"依赖主机行为的隐式契约"。

### 1.6 写寄存器实现（第 341–481 行）

```
403            if (slv_wren) begin
404                wdata = S_AXI_WDATA;
405                wch   = axi_awaddr[11:7];
406                widx  = axi_awaddr[6:2];
407                cnt_wch = axi_awaddr[7:6];
408                cnt_widx = axi_awaddr[5:2];
410                case (axi_awaddr[15:12])
412                    4'h0: begin
413                        if (wch < NUM_CH) begin
414                            case (widx)
415                                5'd0: begin
416                                    r_ctrl[wch] <= wdata;
417                                    if (wdata[1]) sw_rst_p[wch]   <= 1'b1;
418                                    if (wdata[5]) prpd_clr_p[wch] <= 1'b1;
419                                end
420                                5'd1:  r_cfg0[wch]      <= wdata;
...
427                                5'd17: begin                       // 0x44 IRQ_STAT W1C
428                                    if (wdata[0]) irq_done[wch] <= 1'b0;
429                                    if (wdata[1]) irq_ovf[wch]  <= 1'b0;
430                                end
431                                5'd18: r_irq_en[wch] <= wdata;     // 0x48 IRQ_EN
432                                5'd19: r_reg0_cfg[   wch] <= wdata; // 0x4C
...
438                                default: ;
439                            endcase
```
逐项：

- ⭐ **第 404–408 行：地址字段的切分**（这是"字地址解码"的关键）：
  - `wch = axi_awaddr[11:7]`：**通道号**。因为通道步长是 `0x80` = 2⁷，所以 `addr[11:7]` 正好就是通道号（4 通道只需 2 位，但取到 bit 11 以支持更多通道）。
  - `widx = axi_awaddr[6:2]`：**通道内字偏移**（`0x80 / 4 = 32` 个字，所以需要 5 位）。
  - `cnt_wch = axi_awaddr[7:6]`、`cnt_widx = axi_awaddr[5:2]`：**计数器区的**通道与字偏移（因为计数器区步长是 `0x80`，但位切分起点不同：`[7:6]` 取通道、`[5:2]` 取字索引）。
  - ⭐ **注意这里直接用 `=`（阻塞赋值）而不是 `<=`**（第 404–408 行）。因为这些是"组合解码的中间变量"，在同一拍内被后面的 `case` 使用，所以**必须用阻塞赋值**（否则 `case` 会读到上一拍的值）。⭐ **这是"组合中间量必须用阻塞赋值"的正确用法**，值得注意。
- 第 416–418 行：写 `CTRL` 时，除了存值，还要**产生 W1P 脉冲**：`wdata[1]` → `sw_rst_p`，`wdata[5]` → `prpd_clr_p`。
  - ⭐ **W1P（write-1-pulse）**：写 1 生效一次，写 0 无动作。**这是 AXI 里"命令按钮"的标准做法**（因为寄存器是保持型的，写 0 不会"取消"已经发生的动作）。
- 第 427–430 行：`IRQ_STAT` 的 **W1C（write-1-clear）** 语义：写 1 清标志，写 0 无动作。
  - ⭐ **W1C 与 W1P 的区别**：W1C 是"写 1 清除已有标志"，W1P 是"写 1 产生一次脉冲"。**两者都是"写 1 触发动作、写 0 不动"**，但对象不同（清除状态 vs 产生事件）。
  - ⚠️ 注意第 416 行 `r_ctrl[wch] <= wdata;` **无条件整字写入** —— ⚠️ **这里没有实现 WSTRB 字节选通**（与 `pd_filter_chain` 不同）。所以**若主机做"字节写"（例如只改 `CTRL[0]`），整字会被覆盖**（其他位被写 0）。B09 已记录这个不一致。

- 第 443–465 行：全局区（`case (axi_awaddr[9:2])`，用 8 位字索引）：
  - `8'd0` → `GCTRL`
  - `8'd2` → `GIRQ_STAT`（W1C，清全部通道的 `irq_done`/`irq_ovf`）
  - `8'd3` → `GIRQ_EN`
  - `8'd9`–`8'd14`（`0x1024`–`0x1038`）→ `r_pos_thr[0..5]`
  - `8'd16`–`8'd21`（`0x1040`–`0x1054`）→ `r_neg_thr[0..5]`
  - `8'd24`（`0x1060`）→ **`if (wdata[0]) apply_p <= 1'b1;`**（APPLY 脉冲）
  - ⚠️ **【观察】门槛寄存器只取 `wdata[15:0]`**（第 451 行 `r_pos_thr[0] <= wdata[15:0];`），高位丢弃 ✓ 与"16 bit 门槛"一致。

- 第 467–476 行：计数器区（`4'h2`）**只读**，唯一例外是 `FRAME_ACK`：
```
472                        if (wch < NUM_CH && widx == 5'd18) begin
473                            if (wdata[0]) frame_ack_p[wch]     <= 1'b1;
474                            if (wdata[1]) frame_ovf_ack_p[wch] <= 1'b1;
475                        end
```
  - `widx == 18` 对应 `0x48`（`18 × 4 = 72 = 0x48`）✓ 即 `FRAME_ACK`。
  - ⭐ **实现方式**：写 1 产生**单拍脉冲** `frame_ack_p`/`frame_ovf_ack_p`（在任何拍先被清 0，见第 380–381 行），再经第 600–601 行赋给端口 `o_frame_ack`/`o_frame_ovf_ack`。
  - ⚠️ **【观察】`frame_ack_p` 是"写就产生一次脉冲"，没有做 W1P 的"必须写 1"以外的保护**（例如"必须隔一拍才能再发"）。若主机连续写两次，会产生两个脉冲 —— 对 `pd_feature_core` 来说第二次无效果（`st_frame_stat[0]` 已经是 0）✓ 安全。

### 1.7 读寄存器实现（第 486–592 行）

```
490    wire [C_S_AXI_ADDR_WIDTH-1:0] ar_now   = S_AXI_ARVALID ? S_AXI_ARADDR : axi_araddr;
494    wire [CH_SEL_W-1:0]           prpd_ch  = ar_now[15:12] - 4'd4;
496    assign prpd_ps_addr = ar_now[11:2];
```
⭐ **第 490 行是关键**：`ar_now` = "如果 ARVALID 有效就用当前地址，否则用寄存的地址"。
- **为什么？** 因为 PRPD RAM 是**1 拍同步读**（`pd_prpd_ram.v:68` 的 `ps_rdata <= mem[ps_addr]`）。为了让数据"在这一拍就出来"，必须**提前一拍**把地址送给 RAM。
- 所以 `prpd_ps_addr = ar_now[11:2]`（组合，立即给出）；而 `pd_prpd_ram` 在下一拍输出 `ps_rdata`；`S_AXI_RDATA`（第 591–592 行）在 `s_axi_rvalid` 为 1 时把它送出去。
- ⭐ **这是一个"提前一拍给地址以匹配 RAM 延迟"的经典手法**（注释第 489 行也说明了）。

```
494    wire [CH_SEL_W-1:0]           prpd_ch  = ar_now[15:12] - 4'd4;
```
- PRPD 区地址是 `0x4000 + ch*0x1000`，所以 `addr[15:12] = 4 + ch`。反过来 `ch = addr[15:12] - 4` ✓。
- `prpd_ps_addr = ar_now[11:2]`：PRPD 区内的字索引（`0x1000 / 4 = 1024` 个字，需要 10 位 → `addr[11:2]` 正好 10 位）✓。

```
591    assign S_AXI_RDATA = rdata_is_prpd ?
592                         {16'd0, prpd_ps_rdata[16*prpd_ch +: 16]} : rdata_r;
```
- ⭐ **PRPD 区读返回"零扩展到 32 位"的 16 bit 数据**（`{16'd0, ...}`）。所以软件读 PRPD 时只需取低 16 位 ✓。
- `prpd_ps_rdata` 是 4 个通道 PRPD RAM 输出的拼接（`16*NUM_CH = 64` 位），按 `prpd_ch` 切片选中当前通道 ✓。

⭐ **一个重要的读一致性说明**：`rdata_r` 是**组合逻辑**（`always @*`，第 501 行），它直接用 `ar_now` 解码，所以**寄存器类数据在 AR 握手那一拍就被采样**（第 103 行 `rdata <= read_mux`）。而 PRPD 数据需要 1 拍延迟。**两者用同一个 `rdata` 输出但时延不同** —— 通过 `rdata_is_prpd` 标志区分，且 `S_AXI_RDATA` 是组合输出（在 `rvalid` 有效时给出）✓。

### 1.8 中断聚合（第 603–613 行）

```
609            assign ch_irq[gi] = (irq_done[gi] & r_irq_en[gi][0]) |
610                                (irq_ovf[gi]  & r_irq_en[gi][1]);
613    assign irq = |ch_irq;
```
- 每通道中断 = `meas_done 中断且使能` **或** `overflow 中断且使能`。
- `irq = |ch_irq`：**四通道任意一个中断则总中断拉高**（归约或）。
- ⭐ **中断标志的置位逻辑在写寄存器块里**（第 396–401 行）：
```
397            meas_done_d <= i_meas_done;
398            for (c = 0; c < NUM_CH; c = c + 1) begin
399                if (i_meas_done[c] && !meas_done_d[c]) irq_done[c] <= 1'b1;
400                if (i_overflow[c])                     irq_ovf[c]  <= 1'b1;
401            end
```
  - 第 397 行：`meas_done_d` 是 `i_meas_done` 的**延迟一拍**副本；
  - 第 399 行：**边沿检测**（本拍为 1 且上拍为 0）→ 只在"新发生的" meas_done 上置中断标志（不重复置位）；
  - 第 400 行：**`i_overflow` 是粘滞电平**（`pd_feature_core` 里是粘滞位），所以直接"电平置位"即可，不需要边沿检测 ✓。
  - ⭐ **这段逻辑写在"写寄存器"的 `always` 块里**（注释第 197–199 行解释：为了"避免同一 `reg` 被多个 `always` 块驱动"）。**这是又一次"一个 reg 一个 always"纪律的体现。**
  - ⚠️ **副作用**：因为这段每拍都在执行（不依赖 AXI 写），所以"中断标志置位"与"AXI 时序"没有关系 ✓ 正确。

### 1.9 `max_fanout` 属性的使用（第 186–190、332–334 行）

```
186    (* max_fanout = 64 *) reg [NUM_CH-1:0] frame_ack_p, frame_ovf_ack_p;
188    // 双路扇出: max_fanout 触发综合器复制, 把 890 路驱动分摊到 ~14 个副本,
189    // 消除 LUT1 驱动的可控硅式(net fanout)延迟, 同时打散 control set 拥塞。
190    (* max_fanout = 64 *) reg [31:0] r_gctrl, r_girq_en;
332    // sw_rst_p 旧版扇出 926(经 u_regs 衍生后, 全局复位网 756), 用 max_fanout
333    // 强制复制驱动, 避免 reset distribution 单一节点拥塞。
334    (* max_fanout = 64 *) reg [NUM_CH-1:0] sw_rst_p, prpd_clr_p;
```
⭐ **`(* max_fanout = 64 *)` 是一个综合属性**，含义：**"如果这个信号的扇出超过 64，就让综合器复制它的驱动源"**。
- **为什么需要？** 一个信号扇出到几百个负载时：
  1. **走线变长、延迟变大**（"LUT1 驱动的可控硅式延迟"是设计者的比喻）；
  2. **消耗大量布线资源**，可能造成**拥塞**；
  3. 在 7 系列里会影响 **control set（控制集）** 的数量——同一个 CE/CLR 组合的寄存器算一个控制集，控制集太多会限制打包效率。
- **复制驱动源的做法**：把"一个驱动源驱动 890 个负载"变成"14 个副本各驱动 ~64 个负载"。**代价**：多 13 个副本的寄存器与逻辑；**收益**：延迟下降、拥塞缓解、打包率提高。
- ⭐ **注释给出的实测数字**："`sw_rst_p` 旧版扇出 926（经 u_regs 衍生后，全局复位网 756）"。**这是一个非常具体的 QoR 数据**，说明设计者做过资源/时序分析。
- ⚠️ **【观察】** 这个属性是**性能优化手段，不改变功能**。但如果把 `sw_rst_p` 复制了 14 份，那么"全局软复位"的到达时间会有细微差别（不同通道的复位可能差几个皮秒到几十皮秒）——**对一个"复位脉冲"来说这无关紧要**，但对"必须严格同拍"的信号就不能这么用。

---

## 2. `pd_feature_top.v`（379 行）—— 4 通道实例化 + 仲裁 + 计数

### 2.1 数据流（第 1–20 行）

```
4  // 数据流:
5  //   ADC(4ch x 12bit @20MSPS)
6  //     -> pd_feature_core  分段峰值提取 / 1024 相位窗 / n,I,P,Q 统计 / PRPD 写
7  //     -> pd_axis_fifo     突发平滑
8  //     -> pd_axis_arb      帧级轮询仲裁
9  //     -> M_AXIS           8 字节事件包 (供 AXI DMA 搬至 DDR)
10 //   AXI4-Lite -> pd_axil_regs -> 各通道配置; 状态/累加值回读; PRPD 图谱读取
```
⚠️ **【观察】** 第 8 行注释写"**帧级轮询仲裁**"，但实际实现是 **beat 级轮转**（`pd_axis_arb.v:9`）。**注释没跟上实现演化**（B09 已记录）。

### 2.2 复位（第 126–133 行）

```
127    // 复位: 全局复位 | 全局软复位 | 每通道软复位脉冲
128    // 注意: rst_n 是 1bit, 必须显式复制成 NUM_CH 位, 否则 Verilog 会按零扩展
129    //       导致只有 bit0 有效(其余通道被永久复位) —— 这是一个容易踩的坑。
131    assign core_rst_n = {(NUM_CH){rst_n}} &
132                        {(NUM_CH){~w_gctrl[1]}} &
133                        ~w_sw_rst;
```
⭐ **第 128–129 行是一个极容易踩的 Verilog 坑**：
- `rst_n` 是 1 bit，`w_gctrl[1]` 是 1 bit，`w_sw_rst` 是 `NUM_CH` 位；
- 如果直接写 `rst_n & ~w_gctrl[1] & ~w_sw_rst`，Verilog 会把 1 bit 的操作数**零扩展**到 4 位（因为 `w_sw_rst` 是 4 位）→ **`rst_n` 被扩展成 `{3'b000, rst_n}`** → `core_rst_n[3:1]` 恒为 0 → **通道 1/2/3 被永久复位**！
- **正确做法**：显式复制成 `{(NUM_CH){rst_n}}` 与 `{(NUM_CH){~w_gctrl[1]}}`，再按位与。✓ 本代码做对了，并且注释写得很清楚。
- ⭐ **为什么"零扩展"而不是"符号扩展"？** 因为 Verilog 里**无符号**操作数在做位宽对齐时用零扩展，**有符号**才用符号扩展。`rst_n` 声明为 `wire`（无符号）→ 零扩展 → 高位补 0。**所以 1 bit 的信号在按位运算里"扩展成 0"是一个静默的功能错误。**
- ⭐ **可复用结论**：**Verilog 里把 1 bit 信号与多位信号做按位运算时，必须显式复制位宽**。这是一条"看起来应该报 warning 但实际上不报"的坑。

- 第 131–133 行的三层复位：**全局复位（`rst_n`）& 全局软复位（`GCTRL[1]`）& 每通道软复位脉冲（`CTRL[1]`）**。
- ⚠️ **注意 `core_rst_n` 只驱动 `pd_feature_core`、`pd_prpd_ram`、`pd_axis_fifo` 三者**（第 239、302、319 行），**不驱动 `pd_axil_regs`（它用 `rst_n`）、也不驱动 `pd_axis_arb`（它用 `rst_n`）**。所以"软复位通道"时，寄存器配置与仲裁器不受影响 ✓ 这是对的（否则软复位会把配置也清掉）。

### 2.3 四通道 generate（第 216–332 行）

```
220    generate
221        for (i = 0; i < NUM_CH; i = i + 1) begin : GEN_CH
224            wire [PH_W-1:0]   prpd_addr_l;
225            wire [15:0]       prpd_wdata_l;
226            wire              prpd_we_l;
227            wire [15:0]       prpd_rdata_l;
228            wire              prpd_clr_busy_l;
230            // ---- 特征提取核心 ----
231            pd_feature_core #(
232                .CH_ID    (i),
...
295            // ---- PRPD 图谱 RAM ----
296            pd_prpd_ram #(
297                .PH_W   (PH_W),
298                .DEPTH  (1024),
299                .DATA_W (16)
300            ) u_prpd (
...
313            // ---- 事件 FIFO ----
314            pd_axis_fifo #(
315                .DATA_W (EV_W),
316                .ADDR_W (FIFO_AW)
317            ) u_fifo (
...
328                .count    (f_count[i*(FIFO_AW+1) +: (FIFO_AW+1)]),
329                .overflow ()
330            );
```
- 每通道例化三个模块：`pd_feature_core`、`pd_prpd_ram`、`pd_axis_fifo`。
- 第 232 行 `.CH_ID(i)`：**通道号作为参数传入** —— 这是"同一份 RTL 例化 4 次后如何区分通道"的唯一手段（会写进事件包 `ch_id` 字段）。
- ⭐ 第 297–298 行 **`.DEPTH(1024)` 是硬编码的**，而 `cfg_phase_win` 允许到 2048 → **N > 1024 时 PRPD 地址被截断**（B09 已记录）。
- ⭐ **第 329 行 `.overflow ()` 是悬空连接** —— `pd_axis_fifo` 的溢出标志**没有引出**！所以 4 个通道的事件 FIFO 溢出**完全不可观测**。这是 B09 里的高优先级可观测性缺口。
- ⭐ 第 228 行 `prpd_clr_busy_l` 也是**接了就悬空**（只声明、赋值但不使用）→ `pd_prpd_ram` 的清零完成信号**不可观测**。
- ⚠️ **【观察】** 第 293 行 `pd_feature_core` 例化结束于第 293 行，而第 291–292 行接的是 `.st_apply_done`/`.st_apply_pending`。注意 `st_apply_pending` 在 core 里是 `assign`（wire 输出），`st_apply_done` 是 `reg` —— 都连到 `w_apply_done`/`w_apply_pending`（4 位向量）✓。

### 2.4 FIFO 占用深度求和与事件计数（第 354–377 行）

```
355    reg [31:0] fifo_sum;
356    integer j;
357    always @* begin
358        fifo_sum = 32'd0;
359        for (j = 0; j < NUM_CH; j = j + 1)
360            fifo_sum = fifo_sum + {{(31-FIFO_AW){1'b0}},
361                                   f_count[j*(FIFO_AW+1) +: (FIFO_AW+1)]};
362    end
363    assign w_fifo_count = fifo_sum;
```
- ⭐ **第 360–361 行的位宽处理值得注意**：`f_count` 每个通道是 `FIFO_AW+1 = 9` 位，要加到 32 位累加器上，所以先零扩展（`{{(31-FIFO_AW){1'b0}}, ...}` = 补 23 个 0 凑成 32 位）。
  - ⚠️ **【观察】** `31 - FIFO_AW = 31 - 8 = 23`，加上 9 位 = 32 位 ✓ 正确。
  - 但**这个写法在 `FIFO_AW` 变化时会出错**（若 `FIFO_AW = 9`，则 `31-9 = 22`，加 10 位 = 32 ✓ 也对）。**所以只要 `FIFO_AW ≤ 31` 就成立** ✓。
- ⭐ **`for` 循环在 `always @*` 里、用阻塞赋值 `=`** —— 这是**组合展开**（综合成 4 个并行的加法器树），不是时序循环。**用 `=` 是对的**（组合逻辑）。

```
366    reg [63:0] ev_cnt;
367    always @(posedge clk or negedge rst_n) begin
368        if (!rst_n)
369            ev_cnt <= 64'd0;
370        else if (m_axis_tvalid && m_axis_tready)
371            ev_cnt <= ev_cnt + 64'd1;
372    end
373    assign w_event_count = ev_cnt;
```
- **64 位事件计数器**：每成功发出一个事件包（含统计包）+1。**64 位保证永不回绕**（在 26 MSPS 下即使每秒千万个事件也要几百年）。
- ⭐ 注释第 365 行"已输出事件总数" —— **注意它计的是"所有 type"**（含周期统计包），**不是"放电次数"**。放电次数要去读 `CNT_N`（`st_sn_n`）。**语义容易混淆**（B09 已记录）。

```
375    // c_tready is driven by the local event FIFO.  Therefore this pulse means
376    // the feature event has been retained, rather than merely detected.
377    assign o_event_accept = c_tvalid & c_tready;
```
⭐ **这段注释解释了一个重要语义**：`o_event_accept = c_tvalid & c_tready` 中的 `c_tready` **来自本地的 `pd_axis_fifo` 的 `s_tready`**（第 323 行）。所以这个脉冲的含义是"**特征事件已经被 FIFO 收下了**"，而不只是"检测到了"。
- ⭐ **这个语义差别很重要**，因为 `o_event_accept` 会被送到 `pd_ddr_0/i_event_accept` → `pd_snapshot_trigger` → **触发自动快照**。如果它只是"检测到"，那么 FIFO 满时仍会触发快照，导致"有快照但没有对应事件数据"的不一致。**用"已进入 FIFO"作为触发条件更严谨** ✓。
- 注释还强调："**它故意不依赖下游 AXI DMA 握手**"（第 72–75 行）。为什么？因为 DMA 握手可能会有长时间背压（PS 侧来不及取），如果快照触发依赖它，**快照时机就会受 PS 处理速度影响** —— 这会让"触发时刻"变得不确定。**所以触发点选在"事件进入本地 FIFO"这一级，与 PS 完全解耦** ✓ 这是很好的设计判断。

---

## 3. `pd_feature_sys_top.v`（211 行）—— 加 CDC 的对外顶层

### 3.1 两种输入模式（第 28–36、118–159 行）

```
28 module pd_feature_sys_top #(
29     parameter integer NUM_CH    = 4,
30     parameter integer ADC_W     = `PD_ADC_W,
31     parameter integer CLK_HZ    = 130000000,   // 系统时钟 (Hz), PL 主时钟
32     parameter integer SAMPLE_HZ = 26000000,    // ADC 采样率 (Hz), 必须整除 CLK_HZ
33     // 0: adc_data/adc_dv 已由上游公共 CDC 送入 clk 域。
34     // 1: 保持兼容原有独立通道 adc_clk -> clk CDC 架构。
35     parameter integer INPUT_CDC = 1
36 )(
```
⭐ **`INPUT_CDC` 是本模块的"架构开关"**：
- **`INPUT_CDC = 1`**（默认）：本模块**自己**为每个通道例化一个 `pd_adc_cdc`（12 bit 异步 FIFO），把 ADC 域数据搬到 `clk` 域。**这是"方案 B"（独立通道 CDC）。**
- **`INPUT_CDC = 0`**：`adc_data`/`adc_dv` **已经在 `clk` 域**，直接透传。**这是"方案 A"（上游公共 CDC），也是当前 BD 实际配置。**

⭐ **BD 里的实际取值是 `INPUT_CDC = 0`**（`.bd` 的 `pd_feature_0` CONFIG）。原因在注释第 118–124 行：

```
121    // INPUT_CDC=0 用于统一采集架构：pd_pack48 在 ADC 域完成一次四通道
122    // 对齐，随后由公共 48-bit FIFO 把 Path-B 送到这里。这样特征链不再对
123    // 四通道分别做 CDC，避免各通道 FIFO 空标志独立同步造成的样本错位。
```
⭐⭐ **这段解释了一个重要的架构决策**：
- **如果 4 个通道各做一次 CDC**（各有一个异步 FIFO），那么每个 FIFO 的 `empty` 标志是**独立同步**到 `clk` 域的；
- **独立同步意味着"节拍可能不一致"** —— 某一拍 ch0 的 FIFO 有数据、ch1 的没有 → **四通道的样本错位**！
- **正确做法**：在 ADC 域先用 `pd_pack48` 把 4 通道**对齐成一个 48 bit 字**（此时四通道严格同拍），然后用**一个** 48 bit FIFO 搬过时钟域 → 四通道的"同拍关系"被一个 FIFO 保证 ✓。
- ⭐ **结论：多通道同步采样数据跨时钟域时，必须"先对齐再跨域"，不能"各通道独立跨域后再对齐"。** 这是一条非常有价值的设计原则。

```
128    genvar i;
129    generate
130        if (INPUT_CDC != 0) begin : G_ADC_CDC
132            // rst_n is generated in the 130 MHz PL domain. The ADC clock is
133            // independent, so release its reset only after two adc_clk edges.
134            (* ASYNC_REG = "TRUE" *) reg [1:0] adc_rst_sync;
135            always @(posedge adc_clk or negedge rst_n) begin
136                if (!rst_n)
137                    adc_rst_sync <= 2'b00;
138                else
139                    adc_rst_sync <= {adc_rst_sync[0], 1'b1};
140            end
141            wire adc_rst_n = adc_rst_sync[1];
142
143            for (i = 0; i < NUM_CH; i = i + 1) begin : GEN_CH
144                pd_adc_cdc #(.ADC_W(ADC_W), .DEPTH(64),
145                             .CLK_HZ(CLK_HZ), .SAMPLE_HZ(SAMPLE_HZ)) u_cdc (
...
154            end
155        end else begin : G_CLK_DOMAIN_INPUT
156            assign adc_100    = adc_data;
157            assign adc_dv_100 = adc_dv;
158        end
159    endgenerate
```
- ⭐ **第 132–141 行是"跨域复位同步"的范例**：
  - `rst_n` 是在 **130 MHz 域**产生的；`adc_clk` 是**独立时钟**；
  - **直接把 130 MHz 域的复位送给 ADC 域的电路是危险的**（复位释放时刻在 ADC 域是"随机的"，可能落在时钟沿附近 → 亚稳态）；
  - **做法**：在 `adc_clk` 域用两级同步器（`ASYNC_REG` 标注）同步复位，`adc_rst_sync` 从 `2'b00` 逐拍变成 `2'b11`：
    - 第 1 拍：`adc_rst_sync <= {1'b0, 1'b1}` = `2'b01`
    - 第 2 拍：`adc_rst_sync <= {1'b1, 1'b1}` = `2'b11`
  - 所以 `adc_rst_n = adc_rst_sync[1]` **在 2 个 `adc_clk` 沿之后才释放** ✓ 注释第 132–133 行说的就是这个。
  - ⭐ **注意这是一个"没有复位输入的移位器"**：它靠 `rst_n` 的**异步复位端**（`negedge rst_n`）清零，靠"每拍移入 1"来"释放"。**所以 `rst_n = 0` 时它清 0（`adc_rst_n = 0`），`rst_n = 1` 后 2 拍 `adc_rst_n` 变 1** ✓ 逻辑正确。
  - ⚠️ **【观察】** 因为敏感表里有 `negedge rst_n`（异步复位），而 `rst_n` 属于 130 MHz 域、`adc_clk` 是独立时钟 —— **这本身就是"异步复位跨域"**，但**异步复位的释放**被同步了（这是重点）。⭐ **"异步复位、同步释放"是跨域复位的标准做法**，这里实现正确。
- 第 155–157 行：`INPUT_CDC = 0` 时**直接连线透传**，零逻辑 ✓。

### 3.2 `pd_feature_top` 的例化（第 164–209 行）

```
164    pd_feature_top #(
165        .NUM_CH               (NUM_CH),
166        .ADC_W                (`PD_ADC_W),
167        .PH_W                 (`PD_PH_W),
168        .TS_W                 (`PD_TS_W),
169        .EV_W                 (`PD_EV_W),
170        .FIFO_AW              (8),
171        .C_S_AXI_DATA_WIDTH   (32),
172        .C_S_AXI_ADDR_WIDTH   (16)
173    ) u_feat (
```
- ⚠️ **【观察】第 164–173 行的例化没有传 `INPUT_CDC`（本模块也没有这个参数）** —— 因为 CDC 是在 `pd_feature_sys_top` 这一层做的，`pd_feature_top` 只管单时钟域。
- ⚠️ **【观察】位宽宏用法不一致**：第 166–169 行用 `` `PD_ADC_W `` 等宏（来自 `pd_defines.vh`），而 `pd_feature_top` 内部的 parameter 默认值也是这些宏。**功能相同，但用宏而不用参数传递，意味着"无法从这一层覆盖位宽"** —— 如果将来 `pd_defines.vh` 改了而 `pd_feature_top` 的默认值没改（不会，因为它就是引用宏），或者 BD 里改了 `pd_feature_0` 的参数（**BD 只暴露了 `INPUT_CDC` 和 `SAMPLE_HZ`**），位宽不会被覆盖。**这是一个"参数不可配置"的封闭设计** ✓ 对稳定性有利，对灵活性不利。

---

## 4. 本篇易错点小结

1. ⭐ **区域解码用 `addr[15:12]` → 各区必须按 4 KB 边界对齐**，否则地址别名（曾把全局区放 `0x0800` 导致读错寄存器）。
2. ⭐ **`pd_axil_regs` 要求 AW/W 同时有效**（第 260/281/287 行），**不合 AXI 规范**；换互连或换主机可能挂死。**`pd_filter_chain` 用 `aw_hold`/`w_hold` 做对了** —— 同一工程两种风格并存。
3. ⭐ **1 bit 信号与多位信号按位运算会被"零扩展"**（`rst_n & ~w_sw_rst`），必须显式复制 `{(NUM_CH){rst_n}}`，否则通道 1/2/3 被永久复位。
4. **组合中间量（`wdata`/`wch`/`widx`）必须用阻塞赋值 `=`**，因为它们在同一拍被后面的 `case` 使用。
5. **W1P（写 1 产生脉冲）与 W1C（写 1 清标志）的区别**：对象不同（产生事件 vs 清除状态），但都是"写 1 动、写 0 不动"。
6. **`pd_axil_regs` 没有实现 WSTRB 字节选通**（整字覆盖），而 `pd_filter_chain` 实现了 —— 软件做"字节写"时行为不同。
7. **REG0 与 REG1 的寄存器顺序不对称**（REG0：CFG/PHASE/Q；REG1：PHASE/Q/CFG），因为 `REG1_CFG` 是后补的。
8. **`SAMPLE_RATE_HZ`（`0x1058`）硬编码 `26000000`**，改 BD 的 `SAMPLE_HZ` 不会跟着变。
9. **`FRAME_STAT` 注释仍写"读清"，实际已是只读**（v3.0 契约改为 `FRAME_ACK` 显式确认）。
10. ⚠️ **`pd_axis_fifo.overflow` 悬空** → 4 通道事件 FIFO 溢出完全不可观测。
11. ⚠️ **`prpd_clr_busy_l` 悬空** → PRPD 清零完成信号不可观测。
12. **`core_rst_n` 只驱动 core/prpd/fifo**，不驱动 `axil_regs`/`arb` → 软复位不清配置 ✓ 正确。
13. **`EV_CNT` 计的是"所有事件包"（含统计包）**，不是"放电次数"；放电次数读 `CNT_N`。
14. ⭐ **多通道同步数据跨时钟域必须"先对齐再跨域"**（`INPUT_CDC = 0` 的理由）：各通道独立 CDC 会导致空标志独立同步 → 样本错位。
15. **跨域复位的标准做法是"异步复位 + 同步释放"**（`adc_rst_sync` 在 `adc_clk` 域用两级同步器释放）。
16. **`pd_feature_top.v` 头部注释写"帧级轮询仲裁"，实际是 beat 级轮转** —— 注释漂移。
17. **`(* max_fanout = 64 *)`** 用来解决高扇出信号的延迟/拥塞/控制集问题（`sw_rst_p` 曾扇出 926）。
18. **`pd_feature_0` 在 BD 里只暴露 `INPUT_CDC` 与 `SAMPLE_HZ` 两个参数** → 位宽/深度不可从 BD 覆盖。

---

**本卷下一篇**：`A08_DDR环形写入.md` —— 本工程最核心的业务逻辑：字节信用机制、回绕拆分、冻结地址锁存。
