# A05 · PRPD 存储与数据打包

覆盖文件：
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_prpd_ram.v`（89 行）—— PRPD 图谱双口 RAM
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_pack48.v`（79 行）—— 4 通道 → 48 bit 样本字
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_pack192.v`（114 行）—— 4 个样本字 → 192 bit 块 → 3×64 bit beat

> 这三个模块分别解决一个具体问题：**"结果存哪里"（PRPD RAM）**、**"如何把四路拼成一字"（pack48）**、**"如何把字拼成 AXI beat 且不用桶形移位器"（pack192）**。后两个的优化手法非常值得学。

---

## 1. `pd_prpd_ram.v`（89 行）—— 每通道一片 1024×16 bit 真双口 BRAM

### 1.1 文件头（第 1–15 行）

```
1  // =====
2  // pd_prpd_ram.v  --  PRPD (相位分辨局部放电) 图谱存储
3  // ---
4  // 每通道一片 1024(相位窗) x 16bit(视在电荷量) 的真双口 RAM, 目标实现为 Block RAM:
5  //   端口 A: PL 侧读-改-写 (取该相位窗的历史最大值) —— 读写同址
6  //   端口 B: PS 侧经 AXI-Lite 只读
7  // 支持整片清零 (cfg_clear 脉冲后逐地址写 0)。
8  //
9  // 【重要 · 勿改回】BRAM 每个物理端口只有一组地址线, 读写必须同址。
10 //   若端口 A 出现"写 clr_addr / 读 pl_addr"两址并存, Vivado 会无视
11 //   ram_style="block" 退回分布式 RAM(每通道 704 LUT, 4 通道 2816 LUT,
12 //   Slice 占用冲到 97%)。改动端口 A 时务必保持 addr_a 读写共用。
```
⭐ **第 9–12 行是本文件的灵魂**，记录了一次**真实的资源爆炸事故**：
- **BRAM 的每个物理端口只有一组地址线。** 你不可能"用端口 A 同时写地址 X 和读地址 Y"。
- 原代码在清零扫描（`clr_run = 1`）时，把"写地址"设为 `clr_addr`、把"读地址"设为 `pl_addr` —— **两个地址并存**，物理 BRAM 无法实现。
- Vivado 的反应：**无视 `ram_style = "block"` 的提示**，退回**分布式 RAM（LUTRAM）**实现。
- **代价**：每通道 **704 个 LUT** → 4 通道 **2816 个 LUT** → **Slice 占用冲到 97%**（几乎放不下）。
- **修法**：让读写**共用**同一个地址 `addr_a`。清零扫描期间 PL 侧不做读改写，读出的值是无关的，所以语义不受影响。

⭐ **这是一条可以推广的硬知识**：
> **想让存储体落到 BRAM，必须让"每个端口只有一组地址"。** 凡是想在同一个端口上做"两个不同地址的操作"，BRAM 物理上做不到。

### 1.2 模块头（第 17–38 行）

```
17 module pd_prpd_ram #(
18     parameter integer PH_W   = `PD_PH_W,
19     parameter integer DEPTH  = 1024,
20     parameter integer DATA_W = 16
21 )(
22     input  wire                  clk,
23     input  wire                  rst_n,
25     // ---- PL 侧 (core) ----
26     input  wire [PH_W-1:0]       pl_addr,
27     output reg  [DATA_W-1:0]     pl_rdata,
28     input  wire [DATA_W-1:0]     pl_wdata,
29     input  wire                  pl_we,
31     // ---- PS 侧 (AXI-Lite) ----
32     input  wire [PH_W-1:0]       ps_addr,
33     output reg  [DATA_W-1:0]     ps_rdata,
35     // ---- 控制 ----
36     input  wire                  clear_req,     // 1 = 请求整片清零
37     output wire                  clear_busy
38 );
```
- **两个端口、一个时钟**：端口 A（PL 侧，`pl_*`）可读写；端口 B（PS 侧，`ps_*`）只读。
  - ⚠️ "真双口（true dual port）"通常指两个端口都能读写；这里是"简单双口（simple dual port）"的变体：A 端口读写、B 端口只读。**但因为两个端口都能独立给出地址，所以仍需要 BRAM 的双地址能力**（这也是为什么"端口 A 内部两址并存"会失败 —— 双口 BRAM 总共只有两组地址线，A 端口内部还要两组就超了）。
- ⚠️ **【观察】** `DEPTH` 是参数（默认 1024）而 `PH_W = 12`（可表示 0…4095）。**默认配置下 `ps_addr`/`pl_addr` 的高位会被截断**（见第 56/68 行的 `[AW-1:0]`）。`pd_feature_top.v:298` 硬编码传 `DEPTH(1024)`，而 `cfg_phase_win` 允许到 2048 → **若 PS 配 N > 1024，PRPD 地址会被截断到 10 位**（只有 1024 格）。B09 已记录。

### 1.3 地址位宽与存储体（第 40–49 行）

```
40     localparam AW = (DEPTH == 1024) ? 10 :
41                     (DEPTH ==  512) ?  9 :
42                     (DEPTH == 2048) ? 11 : 10;
44     (* ram_style = "block" *) reg [DATA_W-1:0] mem [0:DEPTH-1];
46     reg [AW-1:0] clr_addr;
47     reg          clr_run;
49     assign clear_busy = clr_run;
```
- 第 40–42 行：**用条件表达式根据 `DEPTH` 算地址位宽**（1024→10、512→9、2048→11），其他值默认 10。这是"参数化"的简化写法（不是 `$clog2`，而是一个白名单查表）。
- 第 44 行：存储体，`ram_style = "block"`。**注意：没有 `initial`、没有复位分支** —— 遵守 BRAM 推断规则（见 `A02` 第 2.1 节）。
- 第 49 行 `clear_busy = clr_run`：告诉调用者"清零扫描还没结束"。

### 1.4 端口 A：地址/数据/写使能统一（第 51–64 行）

```
51     // ---- 端口 A: 地址 / 数据 / 写使能 统一 ---
52     // BRAM 单个物理端口只有一组地址线, 读写必须同址。
53     // 原写法在 clr_run 时"写 clr_addr / 读 pl_addr"两个地址并存, 物理 BRAM 无法实现,
54     // 综合器因此无视 ram_style="block" 退回 LUTRAM(每通道 704 LUT, 4 通道共 2816)。
55     // 修正: 读写共用 addr_a。清零扫描期间 PL 不做 RMW, 读出为无关值, 语义不受影响。
56     wire [AW-1:0]     addr_a = clr_run ? clr_addr : pl_addr[AW-1:0];
57     wire              we_a   = clr_run | pl_we;
58     wire [DATA_W-1:0] din_a  = clr_run ? {DATA_W{1'b0}} : pl_wdata;
60     // ---- 端口 A: PL 读-改-写 (读写同址, 读出 1 拍延迟, READ_FIRST 语义) ----
61     always @(posedge clk) begin
62         if (we_a) mem[addr_a] <= din_a;
63         pl_rdata <= mem[addr_a];
64     end
```
⭐ **三行三目选择器实现了"清零优先"**：
- 第 56 行：`addr_a` = 清零中就用 `clr_addr`，否则用 `pl_addr`（截断到 `AW` 位）；
- 第 57 行：`we_a` = 清零中**或** PL 请求写 → 都要写；
- 第 58 行：`din_a` = 清零中写 0，否则写 `pl_wdata`。
- **优先级完全由 `clr_run` 决定**：清零期间 PL 的读写请求全部被"让路"。

⭐ **第 61–64 行的读改写语义**：
- 第 62 行：条件写（`if (we_a)`）；
- 第 63 行：**无条件读**（`pl_rdata <= mem[addr_a]`，没有 `if`）—— 遵守 BRAM 推断规则"读无条件"。
- ⭐ **读写在同一个 `always` 块、同一个时钟沿、同一个地址** → 这正是 BRAM 的 **`READ_FIRST`（读优先，也叫 read-before-write）** 模式：**读端口输出的是写入前的旧值**。
- **为什么 PRPD 需要 READ_FIRST？** 因为 `pd_feature_core` 的 PRPD 写是"读-改-写"：
  - `S_PRRD` 拍给出地址 → 下一拍 `pl_rdata` = 旧值；
  - `S_PRWR` 拍计算 `max(旧值, 新值)` 并写入。
  - 如果 BRAM 是 `WRITE_FIRST`（写优先），那么在"同址同时读写"时读回的是**新值**，`max` 比较就变成 `max(新值, 新值) = 新值` —— **取历史最大的功能就失效了**。
  - 本设计里读发生在"前一个状态"、写发生在"后一个状态"，**实际上没有同址同拍读写**，所以 `READ_FIRST` 与否不影响功能 ✓。但注释写明 `READ_FIRST` 语义是有价值的（明确了端口行为）。

### 1.5 端口 B：PS 只读（第 66–69 行）

```
66     // ---- 端口 B: PS 只读 ----
67     always @(posedge clk) begin
68         ps_rdata <= mem[ps_addr[AW-1:0]];
69     end
```
- **同步读**（同样遵守 BRAM 规则），地址取 `ps_addr[AW-1:0]`（高位丢弃）。**1 拍延迟** —— 这一点 `pd_axil_regs.v:489-496` 明确配合了（把读地址提前一拍送出）。
- ⚠️ **【观察】** 端口 B 与端口 A 用**同一个时钟**（`clk`）。所以这里的"双口"是"同频双口"，不是"异步双口"。PS 侧通过 AXI-Lite 访问，而 AXI-Lite 的 `S_AXI_ACLK` 就是 `clk`（见 `.bd`：整个 130 MHz 域），所以同频成立 ✓。

### 1.6 清零状态机（第 71–87 行）

```
71     // ---- 清零状态机: 逐地址写 0 ----
72     always @(posedge clk or negedge rst_n) begin
73         if (!rst_n) begin
74             clr_run  <= 1'b0;
75             clr_addr <= {AW{1'b0}};
76         end else begin
77             if (!clr_run && clear_req) begin
78                 clr_run  <= 1'b1;
79                 clr_addr <= {AW{1'b0}};
80             end else if (clr_run) begin
81                 if (clr_addr == {AW{1'b1}})
82                     clr_run <= 1'b0;
83                 else
84                     clr_addr <= clr_addr + 1'b1;
85             end
86         end
87     end
```
- 逻辑：`clear_req` 拉高且当前空闲 → 进入清零（`clr_run = 1`，地址归 0）；清零中每拍地址 +1；**地址到达全 1（最后一个地址）时结束**。
- ⭐ **清零需要 `DEPTH` 个时钟周期**：1024 拍 @130 MHz = **7.88 µs**。
  - ⚠️ **【观察】** 清零期间**端口 A 完全被占用**（PL 的读写请求被让路）。如果此时 `pd_feature_core` 正好要写 PRPD，**那次写入会被丢弃**（`pd_feature_core` 不知道 `clear_busy`，没有握手）。这是一个**功能缺口**：`pd_feature_top.v:310` 把 `clear_busy` 接到了 `prpd_clr_busy_l` 但**该信号并未引出使用**（悬空，见 B09）。
  - 实际影响：`prpd_clear` 是 PS 显式操作（通常在停止采集后执行），所以风险低。**但设计上"清零与采集没有互锁"是个弱点。**

---

## 2. `pd_pack48.v`（79 行）—— 4 通道 → 48 bit 样本字（同时实现"双路扇出"）

### 2.1 文件头（第 1–20 行）

```
3  // pd_pack48.v  --  4 通道样本字打包（IF-1 -> IF-3 §3.1）
5  // 契约 §3.1：一个采样时刻的 4 通道拼成一个 48bit 字
6  //     bit [47:36] ch3   [35:24] ch2   [23:12] ch1   [11:0] ch0
7  //   offset binary（0x800 = 0V），本模块只做拼接，不做码制转换（无损）。
9  // 本模块同时完成 契约 §2.2 的 "双路扇出"：
10 //     Path A -> DDR 环形缓存（48bit 样本字流）
11 //     Path B -> 实时算法链（带通/选频/FFT/门槛统计），按通道拆开输出
12 //
13 // 通道同步检查（开发目标 风险 2）：4 路 AD9226 共用同一采样时钟，正常情况下
14 //   4 个 adc_dv 必然同拍。若出现错位（PCB 等长不良 / 采样时钟偏移），
15 //   本模块拉高 o_ch_skew_err 一拍，供 QUALITY 寄存器上报，不阻断数据。
16 //
17 // 数据通路是本模块的"直通"部分：adc_data 本身已是 {ch3,ch2,ch1,ch0} 顺序，
18 //   因此 48bit 字在数值上等于输入总线；这里显式写出是为了把"字段顺序"这一
19 //   契约条款固定下来，避免上游改序后静默出错。
```
⭐ **第 13–15 行是一个很实用的"通道同步检查"**：4 路 ADC 共用采样时钟时，`adc_dv` 应该永远同拍。若出现部分有效（`dv_any & ~dv_all`），说明**通道错位**（PCB 等长不良、采样时钟偏移）。
- 本模块把它变成一个告警信号 `o_ch_skew_err`（拉高 1 拍），**但故意不阻断数据** —— 因为"少一拍的通道数据"比"整条链路停摆"代价小。
- ⚠️ **【观察】** 这个信号在 `pd_ddr_wr_top.v:267` 被接到 `skew_err`，然后只用于 `dbg_ddr[7]`（第 790 行）—— 而 `dbg_ddr` 在 BD 里**没有连出去**（A11 会讲）。所以**这个告警目前不可观测**。B09 已记录。

⭐ **第 17–19 行说清了一个重要事实**：`adc_data` 本身就是 `{ch3,ch2,ch1,ch0}` 顺序，所以 48 bit 字在数值上**等于**输入总线。**那为什么还要显式重拼一次？** 答案在注释最后一句：**"把字段顺序这一契约条款固定下来，避免上游改序后静默出错。"**
- 这个理由很充分：如果只写 `assign o_s48_data = i_adc_data;`，那么将来有人改了上游的拼接顺序，这里会**静默地跟着错**；
- 显式写出字段映射后，**改动上游会立刻在这里暴露**（不匹配会明显）。
- ⭐ **这就是"用冗余代码换取契约显式性"的工程权衡** —— 值得学习。

### 2.2 模块与端口（第 21–42 行）

```
21 module pd_pack48 #(
22     parameter integer CH_NUM = 4,
23     parameter integer ADC_W  = 12
24 )(
25     input  wire                      clk,
26     input  wire                      rst_n,
28     // ---- IF-1：ADC 采样域（adc_clk）输入 ----
29     input  wire [CH_NUM*ADC_W-1:0]   i_adc_data,   // {ch3,ch2,ch1,ch0}
30     input  wire [CH_NUM-1:0]         i_adc_dv,     // 每通道样本有效（正常应全部同拍）
32     // ---- Path A：48bit 样本字（送异步 FIFO -> DDR 环形缓存）----
33     output reg  [CH_NUM*ADC_W-1:0]   o_s48_data,
34     output reg                       o_s48_valid,
36     // ---- Path B：按通道拆开（送实时算法链）----
37     output reg  [CH_NUM*ADC_W-1:0]   o_ch_data,    // 与 Path A 同序，供算法链按切片取用
38     output reg                       o_ch_valid,
40     // ---- 同步/异常 ----
41     output reg                       o_ch_skew_err // 4 通道 dv 不同拍
42 );
```
- **注意 `o_s48_data` 与 `o_ch_data` 的位宽相同（都是 48 bit）**，内容也相同 —— 唯一的差别是**下游怎么理解**：
  - Path A：当作"48 bit 样本字"，交给 `pd_pack192` 攒成 192 bit 块；
  - Path B：当作"4×12 bit 通道数据"，交给 `pd_filter_chain`（它用 `adc_data[ch*ADC_W +: ADC_W]` 按通道切片）。
- ⭐ **这是一个"同一份数据、两种视图"的设计**：物理上复制一份（无损），逻辑上分开用途。
- **为什么输出要寄存一拍？** 见第 60 行的注释（第 3 节）。

### 2.3 通道判据与拼接（第 44–58 行）

```
44     localparam SAMPLE_W = CH_NUM * ADC_W;          // 48
46     // ---- 1) 通道有效判据：要求 4 路 dv 全部有效（与）----
47     wire       dv_all   = (&i_adc_dv);             // 全 1 -> 该拍为有效采样时刻
48     wire       dv_any   = (|i_adc_dv);             // 至少 1 路有效
49     wire       skew_err = dv_any & ~dv_all;        // 部分有效 = 通道错位
51     // ---- 2) 48bit 样本字（严格按 §3.1 字段顺序，逐通道显式拼接）----
53     wire [SAMPLE_W-1:0] s48_word = {
54         i_adc_data[ (3*ADC_W) +: ADC_W ],          // ch3 -> [47:36]
55         i_adc_data[ (2*ADC_W) +: ADC_W ],          // ch2 -> [35:24]
56         i_adc_data[ (1*ADC_W) +: ADC_W ],          // ch1 -> [23:12]
57         i_adc_data[ (0*ADC_W) +: ADC_W ]           // ch0 -> [11:0]
58     };
```
- 第 47 行 `&i_adc_dv`：**归约与**（4 位全 1 才为 1）→ `dv_all` = "四路全部有效"。
- 第 48 行 `|i_adc_dv`：**归约或** → `dv_any` = "至少一路有效"。
- 第 49 行 `skew_err = dv_any & ~dv_all`：**"有但不全"= 通道错位**。优雅的写法 ✓。
- 第 53–58 行：**逐通道显式拼接**。`(3*ADC_W) +: ADC_W` = 从 bit 36 起取 12 位 = `ch3`。注意拼接的顺序：
  - 拼接运算符 `{a, b, c, d}` 中 **`a` 占最高位**，所以 `{ch3, ch2, ch1, ch0}` 得到 `ch3` 在 `[47:36]` ✓ 与契约一致。

### 2.4 输出寄存（第 60–77 行）

```
60     // ---- 3) 输出寄存一拍，改善时序（130MHz 下 48bit 直连也满足，但寄存更稳）----
61     always @(posedge clk or negedge rst_n) begin
62         if (!rst_n) begin
63             o_s48_data    <= {SAMPLE_W{1'b0}};
64             o_s48_valid   <= 1'b0;
65             o_ch_data     <= {SAMPLE_W{1'b0}};
66             o_ch_valid    <= 1'b0;
67             o_ch_skew_err <= 1'b0;
68         end else begin
69             o_s48_valid   <= dv_all;
70             o_ch_valid    <= dv_all;
71             o_ch_skew_err <= skew_err;
72             if (dv_all) begin
73                 o_s48_data <= s48_word;            // Path A
74                 o_ch_data  <= s48_word;            // Path B（同一份数据，无损复制）
75             end
76         end
77     end
```
- 第 69–71 行：`o_s48_valid`/`o_ch_valid` **无条件每拍更新**（= 本拍 `dv_all`）；`o_ch_skew_err` 同理。这三者都是"拍对拍"的直通信号，所以**输出数据与输出 valid 同拍有效** ✓（因为数据也是 `dv_all` 时才更新，且保持到下次）。
- 第 72–75 行：**`if (dv_all)` 条件写数据**（"数据保持到下次有效"模式）。这样 `o_s48_data` 在非有效拍保持不变，下游按 `o_s48_valid` 采样即可。
- 第 60 行注释解释了为什么要打拍："改善时序（130 MHz 下 48 bit 直连也满足，但寄存更稳）"。
  - **为什么要"更稳"？** `i_adc_data` 来自顶层端口（经 BD 连线），走线较长；打一拍后，`s48_word` 的组合路径 + 走线被切断，向后级的时序更宽松。
- ⚠️ **【观察】** 注意 `o_ch_data`/`o_s48_data` 是**两个独立的 48 bit 寄存器**，综合后是 **96 个 FF**。如果把它们合并成"一份数据 + 两个用途"，可以省掉一半 FF —— 但那样就无法在两个不同方向（Path A 走异步 FIFO、Path B 走直连）独立打拍。**这是"资源换清晰度"的取舍。**

### 2.5 一个隐含的时序问题

⚠️ **【观察】** `pd_pack48` 的输出 `o_s48_valid` 是 `dv_all` 的**寄存版本**。而 `pd_ddr_wr_top.v:275` 用 `s48_a_valid & ~fifo_full & ~fifo_wr_busy` 作 FIFO 写使能。所以：
- 路径 = `dv_all`（组合）→ 打一拍 → 与 `fifo_full`（来自 IP，异步）做与 → 写 FIFO。
- **`fifo_full` 是同一个 26 MHz 域（写侧）的信号**，所以这里是同域组合逻辑，不是 CDC ✓。
- 但 **`fifo_full` 到写使能之间只有很少的组合逻辑余量**，而 `fifo_full` 是 IP 输出（可能来自 BRAM/寄存器的较深逻辑）。**这是"写侧时序"的一个潜在风险点**（26 MHz 下 38.46 ns 周期，余量很大，实际无风险）。

---

## 3. `pd_pack192.v`（114 行）—— 48 bit 字 → 192 bit 块 → 3×64 bit beat

### 3.1 文件头（第 1–21 行）—— "为什么不用桶形移位器"

```
3  // pd_pack192.v  --  48bit 字 -> 192bit 块 -> 3x64bit AXI beat（契约 §3.2 / §3.3）
5  // 契约 §3.2：连续 4 个采样时刻的 48bit 字（W0~W3）拼成 192bit 整数 V
6  //     V = W0 + W1<<48 + W2<<96 + W3<<144        （小端）
7  //     beat0 = V[63:0]   beat1 = V[127:64]   beat2 = V[191:128]
9  // 实现要点（为什么不用移位寄存器做齿轮箱）：
10 //   把 blk 固定布局为 {W3, W2, W1, W0}，则该 192 位数在数值上恰好等于 V，
11 //   于是 beat_i 就是 blk 的固定切片 blk[64i +: 64]，只需一个 3:1 的 64bit mux，
12 //   完全不需要桶形移位器（省约 300 LUT）。写入侧同理：第 j 个字写 blk[48j +:48]。
14 // 时序（tready 常高、数据常有时）：每 5 个时钟完成 1 个块（4 字入 / 3 beat 出）
15 //     c5:in W0 | c6:in W1 | c7:out b0 + in W2 | c8:out b1 + in W3 | c9:out b2
16 //   65MSPS 时 130MHz 域需 2 clk/样本 = 8 clk/块，5 < 8，余量充足；
17 //   26MSPS(测试) 时 20 clk/块，更宽松。
19 // 背压：m_axis_tready 拉低时停止发 beat，wcnt 饱和在 4 并使 s48_ready=0，
20 //       反压逐级传到上游异步 FIFO，绝不丢样本（契约"原始无损"要求）。
```
⭐ **第 9–12 行是全文件最有价值的一段**，讲的是**如何避免一个桶形移位器（barrel shifter）**：

**问题**：契约要求把 4 个 48 bit 字拼成一个 192 位数 `V = W0 + W1<<48 + W2<<96 + W3<<144`，然后按 64 位切三块输出。
- **朴素做法**：真的去算 `V`，然后每拍输出 `V >> (64*bcnt)` 的低 64 位 —— 这需要**桶形移位器**（能按任意位数移位），约 **300 个 LUT**。

⭐ **巧妙做法**：
1. 把存储变量 `blk` 声明为 192 位，**固定布局为 `{W3, W2, W1, W0}`**（高位到低位）；
2. 因为 `V = W0 + W1·2⁴⁸ + W2·2⁹⁶ + W3·2¹⁴⁴` 正好也把 `W3` 放在最高 48 位，所以 **`blk` 这个 192 位变量在数值上恰好等于 `V`**；
3. 于是 `beat_i` 就是 `blk` 的**固定切片** `blk[64i +: 64]`，只需一个 **3:1 的 64 位多路选择器**（约 64×2 个 LUT）。
4. 写入侧同理：第 j 个字写进 `blk[48j +: 48]`（固定位置的切片写）。

⭐ **收益**：省约 300 个 LUT，且不需要移位控制逻辑。**这是"把动态移位问题转化为静态切片"的经典技巧。**

**第 14–17 行的时序表**（假设 `tready` 常高）：
```
c5: 收 W0
c6: 收 W1
c7: 收 W2 + 发 beat0
c8: 收 W3 + 发 beat1
c9: 发 beat2
→ 5 个时钟完成一个块（4 字入 / 3 beat 出）
```
- **26 MSPS 档**：130 MHz 下每样本 5 拍？不，26 MSPS 时 `adc_dv` 每 5 拍一个（CDC_RATIO=5），所以 4 个样本要 20 拍 → 每块 20 拍，远大于 5 拍的需求 ✓ 余量 4 倍。
- **65 MSPS 档**：130 MHz 下每样本 2 拍 → 4 个样本 8 拍 → 每块 8 拍 > 5 拍 ✓ **余量 1.6 倍**。
- ⭐ **这就是"5 拍/块"这个设计指标的意义**：它决定了本模块能支持的最高采样率。**如果设计成 6 拍/块，65 MSPS 就只剩 1.33 倍余量；如果 8 拍/块，就刚好不够。**

**第 19–20 行讲背压**：`tready` 拉低时停止发 beat，`wcnt` 饱和在 4 且 `s48_ready = 0`（不再收新字），反压逐级传到上游异步 FIFO，**绝不丢样本**（满足契约"原始无损"）。

### 3.2 模块与端口（第 22–46 行）

```
22 module pd_pack192 #(
23     parameter integer SAMPLE_W     = 48,    // 输入字宽
24     parameter integer BLK_W        = 192,   // 块宽
25     parameter integer AXI_DW       = 64     // 输出 AXI-Stream 位宽
26 )(
27     input  wire                  clk,
28     input  wire                  rst_n,
30     // ---- 上游：48bit 样本字（来自异步 FIFO 读侧，clk 域）----
31     input  wire [SAMPLE_W-1:0]   i_s48_data,
32     input  wire                  i_s48_valid,
33     output wire                  o_s48_ready,
35     // ---- 下游：64bit AXI-Stream（送 AXI-Stream Data FIFO -> DataMover）----
36     output reg  [AXI_DW-1:0]     m_axis_tdata,
37     output wire                  m_axis_tvalid,
38     input  wire                  m_axis_tready,
39     output wire [AXI_DW/8-1:0]   m_axis_tkeep,
40     output wire                  m_axis_tlast,
42     // ---- 辅助 ----
43     output reg                   o_beat_en,   // 1 个 64bit beat 被下游接收（供字节信用计数）
44     output reg                   o_blk_done,  // 1 个完整 192bit 块发出
45     output reg  [31:0]           o_blk_cnt    // 已发出块数（块地址源）
46 );
```
- **上游接口是 "valid/ready" 型**（`i_s48_valid` + `o_s48_ready`），下游是标准 AXIS（`m_axis_t*`）。所以本模块是**"流 → 流"的宽度转换器 + 握手桥**。
- ⭐ `o_beat_en` 是**关键的观测/计数信号**：每被下游接收 1 个 64 bit beat 就拉高 1 拍。它在 `pd_ddr_wr_top.v:341` 被接到 `dbg_ddr` 之外，更重要地接到了 **`pd_ddr_ring_wr` 的 `i_beat_en`** 作为**字节信用（credit）计数源**（见 A08）。⭐ **这就是"PL 内部如何知道'多少字节已经躺进 FIFO 了'"的机制。**
- ⭐ `o_blk_cnt` 是"已发出块数"，本应是**块地址源** —— 但 `pd_ddr_wr_top.v:343` 接的是 `.o_blk_cnt(blk_cnt)` 然后 `blk_cnt` **没有被使用**（悬空）。实际地址由 `pd_ddr_ring_wr` 自己维护 `wr_off`。→ 属冗余输出（B09 已记录）。
- `m_axis_tlast` 恒为 0（见第 69 行），因为 DataMover 靠 BTT 定长度，不靠 TLAST。

### 3.3 常量与状态（第 48–57 行）

```
48     localparam integer WORD_PER_BLK = BLK_W / SAMPLE_W;   // 4
49     localparam integer BEAT_PER_BLK = BLK_W / AXI_DW;     // 3
51     // 注意：wcnt 需要表示 0..4，必须用 3bit；写成 WORD_PER_BLK[1:0] 会把 4 截断成 0
52     reg [BLK_W-1:0] blk;                   // {W3, W2, W1, W0}
53     reg [2:0]       wcnt;                  // 已接收字数 0..4
54     reg [2:0]       bcnt;                  // 已发出 beat 数 0..2
56     wire in_acc  = i_s48_valid & o_s48_ready;
57     wire out_acc = m_axis_tvalid & m_axis_tready;
```
⭐ **第 51 行的注释是一个容易踩的位宽坑**：`wcnt` 需要表示 **0…4 共 5 个值**，所以**必须用 3 bit**。如果偷懒写成 `WORD_PER_BLK[1:0]`（`4 = 3'b100` 的低 2 位 = `2'b00`），那么 `wcnt` 永远无法达到 4，**块永远攒不满** → **整个链路死锁**。这是一个"值域分析不到位就死锁"的典型例子。

- 第 53–54 行：`wcnt` = 已接收字数（0…4）；`bcnt` = 已发出 beat 数（0…2）。
- 第 56–57 行：**`in_acc`/`out_acc` = "本拍真实发生了一次传输"**。这是所有 AXI-Stream 设计的标准写法：**只有 `valid & ready` 同时成立才算传输成功**。

### 3.4 握手信号与数据选择（第 59–79 行）

```
61     assign o_s48_ready  = (wcnt != WORD_PER_BLK[2:0]);
62     // 有效 bit 数 = wcnt*48，需覆盖第 (bcnt+1) 个 beat 的 64bit
63     //   bcnt=0 需 wcnt>=2 ; bcnt=1 需 wcnt>=3 ; bcnt=2 需 wcnt>=4
64     assign m_axis_tvalid = (bcnt == 3'd0) ? (wcnt >= 3'd2)
65                          : (bcnt == 3'd1) ? (wcnt >= 3'd3)
66                          : (bcnt == 3'd2) ? (wcnt >= WORD_PER_BLK[2:0])
67                          : 1'b0;
68     assign m_axis_tkeep  = {AXI_DW/8{1'b1}};   // 恒全有效（192bit 100% 利用）
69     assign m_axis_tlast  = 1'b0;               // 由 DataMover 的 BTT 决定长度，不依赖 tlast
```
⭐ **第 64–67 行的 `tvalid` 判据是"流式流水"的关键**，必须理解：

- beat0 覆盖 `blk[63:0]`，也就是 `W0`（48 位）+ `W1` 的低 16 位。所以要发 beat0，**至少要先收到 2 个字**（`wcnt ≥ 2`）。
  更精确地说：`W0` 占 bit 0–47、`W1` 占 bit 48–95。beat0 是 bit 0–63，所以只需要 `W0` 全部（48 位）+ `W1` 的前 16 位。但既然 `W1` 是**整字写入**的（`blk[48+:48] <= i_s48_data`），收到 `W1` 时它的 48 位都到齐了，所以判据写成 `wcnt ≥ 2`（收到 W0 和 W1）✓ 正确且安全。
  - ⭐ **为什么不能要求 `wcnt ≥ 1`（只等 W0）？** 因为在"收到 W0 但还没收到 W1"的那一刻，`blk[63:48]` 是**上一次的残留数据**（或复位值 0），发出去的第一个 beat 会包含错误的高 16 位。
- beat1 覆盖 `blk[127:64]` = `W1` 的后 32 位 + `W2` 的 32 位 → 需要 `wcnt ≥ 3`；
- beat2 覆盖 `blk[191:128]` = `W2` 的后 16 位 + `W3` 的全部 → 需要 `wcnt ≥ 4`（即整块收满）。
- 第 67 行 `: 1'b0`：`bcnt == 3`（非法值）时不发。
- ⭐ 第 68 行 `tkeep` 恒全 1：因为 192 = 3 × 64，**100% 利用，没有填充字节**。
- ⭐ 第 69 行 `tlast` 恒 0：**长度由 DataMover 的 BTT 决定**（详见 A08 的 ring_wr 命令字），所以不需要 TLAST。**这是一个刻意的简化**：如果依赖 TLAST 定长度，DataMover 就不知道"这一包什么时候结束"，会一直等着。

```
71     // ---- 输出数据：固定切片选择，无桶形移位 ----
72     always @(*) begin
73         case (bcnt)
74             3'd0: m_axis_tdata = blk[ 63:  0];
75             3'd1: m_axis_tdata = blk[127: 64];
76             3'd2: m_axis_tdata = blk[191:128];
77             default: m_axis_tdata = blk[63:0];
78         endcase
79     end
```
- **这就是第 3.1 节说的"3:1 的 64 位多路选择器"**：三个固定切片，按 `bcnt` 选一个。**没有移位操作** ✓。
- 第 77 行 `default` 分支：`bcnt` 为非法值时输出 `blk[63:0]`（无害，因为 `tvalid` 此时为 0）。
- ⚠️ **【观察】** 这里是**组合输出**（`always @(*)`），而 `m_axis_tdata` 声明为 `reg`。组合输出的好处是"数据不必等一拍就能和 `tvalid` 同时有效"；代价是"从 `blk` 到输出的路径较长"（64 位多路选择约 2 级 LUT）。在 130 MHz 下应该没问题（注释第 60 行说"48bit 直连也满足，但寄存更稳"，同理这里也没寄存）。

### 3.5 主时序（第 81–112 行）

```
82     always @(posedge clk or negedge rst_n) begin
83         if (!rst_n) begin
84             blk        <= {BLK_W{1'b0}};
85             wcnt       <= 3'd0;
86             bcnt       <= 3'd0;
87             o_beat_en  <= 1'b0;
88             o_blk_done <= 1'b0;
89             o_blk_cnt  <= 32'd0;
90         end else begin
91             o_beat_en  <= out_acc;
92             o_blk_done <= 1'b0;
94             // 写入：第 wcnt 个字放在 blk[48*wcnt +: 48]
95             if (in_acc) begin
96                 blk[wcnt*SAMPLE_W +: SAMPLE_W] <= i_s48_data;
97                 wcnt <= wcnt + 3'd1;
98             end
100            // 读出：3 个 beat 发完即整块复位，形成 5 clk/块 的稳定节拍
101            if (out_acc) begin
102                if (bcnt == BEAT_PER_BLK[2:0] - 3'd1) begin
103                    bcnt       <= 3'd0;
104                    wcnt       <= 3'd0;
105                    o_blk_done <= 1'b1;
106                    o_blk_cnt  <= o_blk_cnt + 32'd1;
107                end else begin
108                    bcnt <= bcnt + 3'd1;
109                end
110            end
111        end
112    end
```
- 第 91 行 `o_beat_en <= out_acc`：**每成功发一个 beat 就输出 1 拍脉冲**。**这是字节信用机制的计数源**（下游 `pd_ddr_ring_wr` 每收到这个脉冲就 `avail_bytes += 8`）。
- 第 92 行 `o_blk_done <= 1'b0`：先清零 → 只有整块发完才置 1（单拍脉冲）。
- ⭐ **第 95–98 行（写入）**：`blk[wcnt*SAMPLE_W +: SAMPLE_W] <= i_s48_data` —— **用 `wcnt` 动态选择写入位置**。
  - ⚠️ **注意这里的"动态"是合法的**：写的是**寄存器的一个位段**，综合后是一个"带使能的位段写入"（每个 48 位段有自己的使能），**不是桶形移位**。
  - ⭐ **对比**：如果写成 `blk <= (blk >> 48) | (i_s48_data << 144)`，那就是真正的桶形移位器。**本设计的写法避免了它。**
  - `wcnt <= wcnt + 3'd1`：收到一个字就 +1。
- ⭐ **第 101–110 行（读出）**：
  - 第 102 行判据 `bcnt == BEAT_PER_BLK - 1`（= 2，即最后一个 beat）；
  - 最后一个 beat 成功后：**同时清 `bcnt`、清 `wcnt`（整块复位）、置 `o_blk_done`、块计数 +1**；
  - ⭐ **"整块复位"就是第 14 行说的"形成 5 clk/块 的稳定节拍"**：不用等"收满 4 个字才允许发"或"发完才允许收"，而是**收和发交叠进行**（第 96 行和第 101 行在同一个 `always` 块里，互不阻塞）。
  - ⚠️ **【观察 · 需要注意的边界】** 在第 102–106 行"清 `wcnt`"的同拍，如果 `in_acc` 也为 1（正在收第 5 个字 = 新块的第 1 个字），那么第 96 行的 `blk[wcnt*SAMPLE_W +: SAMPLE_W] <= i_s48_data` 与第 104 行的 `wcnt <= 0` 会**互相干扰**：
    - 第 96 行用**旧的** `wcnt` 写 `blk`（因为非阻塞赋值读旧值）；
    - 第 104 行把 `wcnt` 置 0；
    - 结果：新块的 W0 被写到了**旧块的第 wcnt 个位置**（如果旧 `wcnt` = 4，那 `blk[192 +: 48]` 越界，Verilog 会**静默忽略**这次赋值）→ **新块的第一个字丢失！**
    - ⚠️ **这是否真的会发生？** 需要检查 `wcnt` 在上一个 beat 发完时的值。按设计，块发完时 `wcnt == 4`（因为 `bcnt == 2` 需要 `wcnt ≥ 4`）。而 `o_s48_ready = (wcnt != 4)`，所以 **`wcnt == 4` 时 `o_s48_ready = 0` → `in_acc = 0`** ✓ 不会同时发生。
    - ⭐ **所以设计是安全的**，但**安全性依赖于 `o_s48_ready` 的判据**（`wcnt != WORD_PER_BLK`）。**如果将来改了 `o_s48_ready` 的判据，这个边界就会出问题。** 记入 B09 作为"脆弱点"。
- 第 106 行 `o_blk_cnt` 自增：**块序号**（本应是块地址源，但当前未被使用）。

### 3.6 吞吐余量校核（把注释里的话算一遍）

| 采样率 | `adc_dv` 间隔（拍） | 每块需要的拍数 | 本模块每块拍数 | 余量 |
|---|---|---|---|---|
| 26 MSPS | 130/26 = 5 | 4 × 5 = **20** | 5 | **4×** |
| 65 MSPS | 130/65 = 2 | 4 × 2 = **8** | 5 | **1.6×** |

⭐ **结论**：本模块能支持到 65 MSPS（这是工程给 65 MSPS 目标留下的余量之一），但**不是无限的**。若将来上到 104 MSPS（130/104 = 1.25 拍/样本不可行，因为必须 ≥1 拍），**每块只需 4 拍 < 5 拍 → 会阻塞**。
→ **"每块 5 拍"就是本链路的吞吐上限：最高约 `130/5 × 4 = 104 MSPS`。** 但实际受限于 `adc_dv` 必须是整数分频，所以上限是 **65 MSPS**（130/2）。

---

## 4. 本篇易错点小结

1. **BRAM 的每个物理端口只有一组地址线**：端口 A 内"读写两址并存"会让 Vivado 无视 `ram_style` 退回 LUTRAM（本项目实测 2816 LUT / Slice 97%）。
2. **BRAM 推断三规则**（本文件再次体现）：写块无复位、读**无条件**、无 `initial`。
3. **`READ_FIRST` 语义**：同址同拍读写时读端口给旧值。本项目靠"读在前一状态、写在后一状态"回避了冲突，但要知道这个语义。
4. **PRPD 清零期间端口 A 被独占**，且 `pd_feature_core` **不看 `clear_busy`** → 若在采集期间清零会丢一次更新。**（`clear_busy` 在顶层也未引出使用。）**
5. **`pd_prpd_ram.DEPTH` 硬编码 1024，而 `cfg_phase_win` 可到 2048** → N > 1024 时 PRPD 地址被截断。
6. **`pd_pack48` 的 `skew_err` 通道错位告警当前不可观测**（只进 `dbg_ddr[7]`，而 `dbg_ddr` 未连出 BD）。
7. **`pd_pack48` 显式重拼字段顺序是刻意的**（"避免上游改序后静默出错"），不要当成冗余删掉。
8. ⭐ **`wcnt` 必须用 3 bit**（要表示 0–4）。写成 `WORD_PER_BLK[1:0]` 会把 4 截断成 0，**块永远攒不满 → 链路死锁**。
9. ⭐ **"固定切片 + 3:1 mux"代替桶形移位器**，省约 300 LUT。写侧同理用 `blk[48*wcnt +: 48]` 的位段写。
10. **`m_axis_tvalid` 判据 `wcnt ≥ {2,3,4}` 不能放宽**：放宽会让 beat 里包含未写入的残留数据。
11. **`tlast` 恒 0 是刻意的**（长度由 DataMover 的 BTT 决定），不要改成靠 TLAST。
12. **每块 5 拍是本链路的吞吐上限**：26 MSPS 余量 4×、65 MSPS 余量 1.6×、104 MSPS 会阻塞。
13. **`o_blk_cnt` 当前是悬空输出**（真实地址由 `pd_ddr_ring_wr` 的 `wr_off` 维护）。
14. ⚠️ **"发完最后一 beat 同拍又收新字"的边界之所以安全，依赖 `o_s48_ready` 的判据（`wcnt != 4`）**；改判据会破坏这个安全性。

---

**本卷下一篇**：`A06_事件FIFO与轮询仲裁.md` —— 能把 BRAM 推断踩到极致的 `pd_axis_fifo`，以及 beat 级轮转仲裁器。
