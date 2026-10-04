# A06 · 事件 FIFO 与轮询仲裁

覆盖文件：
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_axis_fifo.v`（131 行）—— 单 RAMB36 的同步 AXI-Stream FIFO
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_axis_arb.v`（85 行）—— 4 通道 beat 级轮转仲裁器

> `pd_axis_fifo.v` 是**本工程注释最详尽的文件**：它的文件头列出了 6 条 BRAM 推断规则和一条"代价声明"，并给出了**改前改后的实测资源对比**（4852 LUT → 36 LUT）。这份注释本身就是一份 BRAM 设计教材。

---

## 1. `pd_axis_fifo.v`（131 行）—— 6 条 BRAM 硬规则 + 一个"输出保持"陷阱

### 1.1 文件头（第 1–40 行）

```
2  // pd_axis_fifo.v  --  轻量同步 FIFO (AXI-Stream 兼容, 单 RAMB36 实现)
3  // 用于平滑峰值事件的突发, 并吸收下游(AXI DMA / 仲裁器)的瞬时背压。
5  // 【重要 · 勿改回】Block RAM 推断与同步读 FIFO 的六条硬性规则
6  //   违反任意一条, Vivado 都会*静默*降级为 LUTRAM 甚至把存储体打散成寄存器
7  //   ——只有 INFO 级提示, 没有 warning, 极易漏掉。
```

⭐ **第 5–7 行是全工程最重要的"设计纪律"声明**：**违反 BRAM 推断规则的后果是"静默降级"，只报 INFO，没有 warning。**

**六条硬规则**（第 9–31 行）：

| # | 规则 | 违反的后果 | 依据行 |
|---|---|---|---|
| 1 | 存储体写过程块的**敏感表里不能出现 `rst_n`** | 报 "RAM is sensitive to asynchronous reset signal. this RTL style is not supported." → "RAM dissolved into registers" | 第 9–12 行 |
| 2 | 读必须是**同步读**（地址打一拍，数据下一拍才有效） | 写成 `assign m_tdata = mem[rp]` → 综合出 **DEPTH:1 巨型多选器**（实测单实例 2210 个 MUXF7 / 1105 个 MUXF8），且无法映射 BRAM | 第 14–16 行 |
| 3 | 读必须**无条件**执行，不能用 `if (fetch)` 门控 | 门控读 → 判定"非标准模板" → 退回 LUTRAM（实测 352 LUT）；无条件读 → **1 个 RAMB36** | 第 18–21 行 |
| 4 | **输出保持**：无条件读意味着"输出恒等于 `mem[读地址]`"，所以读地址必须在 `num == 0` 期间**回指最后一条有效条目**（`raddr - 1`） | 否则无条件读会把未写入单元的**垃圾**冲掉输出寄存器 → **"valid=1 但数据错"**（实测 90/120 个 beat 为 X） | 第 23–27 行 |
| 5 | 一个物理 BRAM 端口只有一组地址线 → 本模块读写地址不同，走 **simple-dual-port（一写一读）** | 这是 BRAM 最容易识别的结构 | 第 29–30 行 |

⭐ **第 4 条是本文件最独特的一条**，需要重点理解（下面 1.4 节详述）。

**实测对比**（第 32–34 行）：
```
原始(组合读+异步复位): 4852 LUT + 16764 FF + 2210 MUXF7 + 1105 MUXF8 + 0 BRAM
最终(同步读+地址回指):   36 LUT +    27 FF +    0 MUXF7 +    0 MUXF8 + 1 RAMB36
```
⭐ **这个对比极具说服力**：**LUT 从 4852 降到 36（135 倍）**，FF 从 16764 降到 27，还得到了一个真正的 BRAM。
- **教训**：**不要用 LUT 用量去"感觉"设计好坏** —— 一个写法不当的 FIFO 能吃掉近 5000 个 LUT。**必须看资源报告的 `RAMB36/RAMB18` 与 `MUXF*` 计数。**

**时序契约**（第 36–39 行）：
```
时序契约: 端口与旧版一致, 仅"空 FIFO 写入后首字输出"比组合读晚 1 拍;
         连续流传输时 dout_valid 保持连续, 吞吐率不变。
         下游 pd_axis_arb 为纯组合握手(valid/ready 与数据无关), 无需同步修改。
注: count 的语义为 "已写入且尚未被取走的条目数", 上限 DEPTH。
```
- ⭐ **"仅空 FIFO 写入后首字输出比组合读晚 1 拍"** —— 这是一个**边界情况**的说明：
  - 连续流时（FIFO 非空），`fetch` 每拍都在进行，输出寄存器一直被填充，`dout_valid` 连续为 1，**吞吐率不受影响**；
  - 只有"FIFO 原本是空的、刚写入第一个字"时，需要 1 拍把数据装载到输出寄存器 → 比组合读晚 1 拍。
- ⭐ **"下游 `pd_axis_arb` 为纯组合握手（valid/ready 与数据无关），无需同步修改"** —— 这是一个"改动影响面分析"的结论：因为仲裁器只看 `tvalid`/`tready`，不看数据内容，所以 FIFO 的输出延迟变化不影响它 ✓。

### 1.2 模块头与内部状态（第 43–91 行）

```
43 module pd_axis_fifo #(
44     parameter integer DATA_W  = 64,
45     parameter integer ADDR_W  = 8,        // 深度 = 2^ADDR_W
46     parameter integer FWFT    = 0         // 保留参数, 未使用
47 )(
48     input  wire                 clk,
49     input  wire                 rst_n,
51     // 从端 (写入)
52     input  wire [DATA_W-1:0]    s_tdata,
53     input  wire                 s_tvalid,
54     input  wire                 s_tlast,
55     output wire                 s_tready,
57     // 主端 (读出)
58     output wire [DATA_W-1:0]    m_tdata,
59     output wire                 m_tvalid,
60     output wire                 m_tlast,
61     input  wire                 m_tready,
63     output wire [ADDR_W:0]      count,
64     output wire                 overflow     // 写入时 FIFO 满 -> 丢数指示
65 );
```
- **标准 AXI-Stream FIFO**：从端（写）+ 主端（读），外加 `count`（占用数）与 `overflow`（丢数指示）。
- ⚠️ **【观察】`FWFT` 参数是"保留参数, 未使用"** —— 但本模块的行为**实际上就是 FWFT**（First-Word Fall-Through：非空时第一个字已经"落下"到输出寄存器，`m_tvalid` 立刻有效，无需先发读请求）。所以这个参数实际上是"名义上存在、行为上固定为 FWFT 风格"。**参数名与行为的不一致是潜在的误读点**（B09 已记录）。
- `count` 的位宽是 `ADDR_W+1`（要能表示 0…DEPTH，因为"满"时占满深度也需要一个额外位）。
- `overflow` 是"写入时 FIFO 满 → 丢数"的**粘滞**指示（见第 114 行）。

```
67     localparam DEPTH = 1 << ADDR_W;
70     // tlast 与数据打包, 只占用一块存储体, 避免第二个数组再拉一份 BRAM
71     localparam MEM_W = DATA_W + 1;
73     (* ram_style = "block" *) reg [MEM_W-1:0] mem [0:DEPTH-1];
74     reg [ADDR_W-1:0] wp;      // 写指针
75     reg [ADDR_W-1:0] raddr;   // 读地址: 下一条待装载条目的地址
76     reg [ADDR_W:0]   num;     // 已写入、尚未装载到输出寄存器的条目数
78     reg [MEM_W-1:0]  dout_raw;   // 同步读输出(含打包的 tlast)
79     reg              dout_valid;
80     reg              ovf_r;
```
- ⭐ **第 70–71 行是一个很实用的技巧**：`tlast` **打包进存储体的最高位**（`MEM_W = DATA_W + 1 = 65`），而不是单独开一个数组存 `tlast`。
  - **为什么？** 如果单独存 `reg tlast_mem[0:DEPTH-1]`，Vivado 会**再拉一份 BRAM**（哪怕只存 1 位），或者把它塞进 LUTRAM —— 两种情况都浪费资源。
  - **打包后**：一个 65 位的宽存储体，仍然可以映射到 RAMB36（36 Kb 容量；`2⁸ × 65 = 16640 bit < 36864` ✓）。
  - ⭐ **代价**：读写时都要做"拆分/拼接"，但这是纯连线操作，零逻辑开销。
- 第 75 行 `raddr` 的注释是"**下一条待装载条目的地址**" —— 注意"待装载"三个字，这是理解第 4 条规则的关键（见 1.4）。
- 第 76 行 `num` 的注释是"**已写入、尚未装载到输出寄存器的条目数**" —— ⭐ **这个定义很微妙**：它**不包含**已经装载到 `dout_raw` 的那一条。所以：
  - `num = 0` 且 `dout_valid = 1` → FIFO 里还有 1 条数据（在输出寄存器里）；
  - 真正的"占用条目数" = `num + dout_valid`（见第 85 行 `used`）。

### 1.3 握手信号（第 82–97 行）

```
82     wire wr_en = s_tvalid && s_tready;                    // 写入
83     wire rd_en = m_tready && dout_valid;                  // 本拍 dout 被取走
84     wire fetch = (num != 0) && (!dout_valid || m_tready); // 装载下一条
85     wire [ADDR_W:0] used = num + dout_valid;              // 实际占用条目数
87     assign s_tready = (used != DEPTH);
88     assign m_tdata  = dout_raw[DATA_W-1:0];
89     assign m_tlast  = dout_raw[DATA_W];
90     assign m_tvalid = dout_valid;
91     assign count    = used;
92     assign overflow = ovf_r;
```
⭐ **`fetch`（第 84 行）是理解本模块的核心**，逐项拆解：
```
fetch = (num != 0) && (!dout_valid || m_tready)
```
- `num != 0`：还有"待装载"的条目；
- `(!dout_valid || m_tready)`：输出寄存器**要么是空的**（可以装载新数据），**要么本拍正被下游取走**（下拍就空了，可以覆盖）。
- **两条同时成立才装载** → 保证不会覆盖尚未被取走的数据。

⭐ **`used`（第 85 行）= `num + dout_valid`**：因为 `num` 不含输出寄存器里那一条，所以加 `dout_valid` 才是真实占用。
- ⭐ 这是**一个非常容易写错的地方**：如果用 `num` 当 `s_tready` 的判据（`s_tready = (num != DEPTH)`），那么"`num = DEPTH-1` 且输出寄存器已占用"时，`s_tready` 仍为 1 → **允许写入第 DEPTH+1 条 → 覆盖未取走的数据 → 丢数据**。
- ⭐ **而且这种错误不会报错**，只是"偶尔丢一条" —— 极难调试。**必须用 `used`。**

⭐ **`m_tvalid = dout_valid`**（第 90 行）：这就是 FWFT 风格 —— 输出寄存器里一有数据就 `valid`。

⭐ **`rd_en = m_tready && dout_valid`**（第 83 行）：下游取走。

### 1.4 读地址回指（第 94–103 行）—— 第 4 条规则的实现

```
94     // 读地址选择: 有待装载条目 -> raddr(下一条);
95     //             无待装载条目 -> raddr-1(回看最后一条已装载条目), 让无条件
96     //             读始终保持输出寄存器内容, 防止被未写入单元的垃圾冲掉。
97     wire [ADDR_W-1:0] rd_addr = (num != 0) ? raddr : (raddr - 1'b1);
99     // ---- 存储体: 无复位, 条件写 + 无条件同步读 (BRAM 推断的关键) ----------
100    always @(posedge clk) begin
101        if (wr_en) mem[wp] <= {s_tlast, s_tdata};
102        dout_raw <= mem[rd_addr];
103    end
```
⭐ **这两行是本模块最巧妙的地方**，要把"为什么"完整走一遍：

- **问题**：因为规则 3 要求"**无条件读**"（每个时钟沿都读一次），所以 `dout_raw` **永远等于 `mem[rd_addr]` 的内容**。
- **危险场景**：假设 FIFO 里最后一条数据已经被装载到 `dout_raw`，此时 `num = 0`（没有待装载条目）。如果 `rd_addr` 停在 `raddr`（= 下一条待写入的位置），而那个位置**从未被写过**，那么下一拍 `dout_raw` 就会被那个位置的**垃圾内容**覆盖：
  - `dout_valid` 仍然是 1（因为数据还没被下游取走）；
  - 但 `m_tdata` 已经变成垃圾 → **"valid = 1 但数据错"**。
- ⭐ **注释里给出了实测后果**："导致 ch1~ch3 的事件在等待仲裁器轮询期间被垃圾覆盖（**实测 90/120 个 beat 为 X**）"。
  - **为什么会"等待仲裁器轮询"？** 因为 `pd_axis_arb` 是 beat 级轮转，某个通道可能连续几拍不被选中 → `m_tready = 0` → 数据在输出寄存器里等好几拍 → 有充足时间被垃圾覆盖。**这是一个"仲裁 + 无条件读"交互出的 bug**。
- **修法**：`rd_addr = (num != 0) ? raddr : (raddr - 1'b1)`。
  - 有数据要装载 → 用 `raddr`（正常前进）；
  - 没有数据要装载 → **回指 `raddr - 1`**，即"最后一条已经被装载过的条目地址" → **无条件读会一直把同一条数据重新读回 `dout_raw`** → 输出保持稳定 ✓。
- ⭐ **为什么回指一定正确？** 因为"最后一条被装载过的条目"是**已经被写入过的**（`wr_en` 曾经为 1 写进 `mem[raddr-1]`），所以读回来的是**真实数据**而不是垃圾 ✓。
  - 边界：若 FIFO 从空开始（`raddr = 0`），`rd_addr = 0 - 1 = ADDR_W 位全 1`（回绕到最大地址）。那个地址也没写过 → 读回来是垃圾。**但此时 `dout_valid = 0`**（从未装载过任何数据），所以 `m_tvalid = 0`，垃圾数据不会被采信 ✓ 安全。

- 第 101 行：**条件写 + 打包 `tlast`**（`{s_tlast, s_tdata}`）。
- 第 102 行：**无条件同步读**，地址用上面算好的 `rd_addr`。**与写在同一 `always` 块里，但敏感表只有 `posedge clk`（无复位）** ✓ 符合规则 1。

### 1.5 指针/计数/有效标志/溢出（第 105–129 行）

```
106    always @(posedge clk or negedge rst_n) begin
107        if (!rst_n) begin
108            wp         <= {ADDR_W{1'b0}};
109            raddr      <= {ADDR_W{1'b0}};
110            num        <= {(ADDR_W+1){1'b0}};
111            dout_valid <= 1'b0;
112            ovf_r      <= 1'b0;
113        end else begin
114            if (s_tvalid && !s_tready) ovf_r <= 1'b1;   // 满时写入 -> 丢数
116            if (wr_en) wp <= wp + 1'b1;
117            if (fetch) raddr <= raddr + 1'b1;
119            if (wr_en && !fetch)
120                num <= num + 1'b1;
121            else if (!wr_en && fetch)
122                num <= num - 1'b1;
124            if (fetch)
125                dout_valid <= 1'b1;
126            else if (rd_en)
127                dout_valid <= 1'b0;
128    end
```
- 第 114 行：**`s_tvalid && !s_tready` 即"想写但写不进" → 置 overflow（粘滞）**。注意是"想写但被拒"，不是"写进去了"。这个定义很重要：**它表示"数据被丢弃"**。
- 第 116 行：`wp` 只在真正写入时 +1。
- 第 117 行：`raddr` 只在 `fetch` 时 +1。
- ⭐ **第 119–122 行是 `num` 的三态更新**（用 `if/else if` 明确列出三种情况）：
  | 条件 | `num` 变化 | 含义 |
  |---|---|---|
  | `wr_en && !fetch` | `+1` | 写入但没装载 → 积压增加 |
  | `!wr_en && fetch` | `−1` | 没写入但装载了 → 积压减少 |
  | 两者都真 或 都假 | 不变 | 写一条同时装载一条 → 净变化 0 |
  - ⭐ **这样写比"`num <= num + wr_en - fetch`"更清晰**，且**避免了位宽与溢出的隐式问题**（`+1`/`-1` 单独出现时不会越界）。**这是一个值得学的写法：显式列出净变化的三种情况。**
  - ⚠️ **【观察】这里用了 `if/else if`，所以"两者都为真"的第三种情况被 `else` 覆盖了 —— 即 `wr_en && fetch` 时两个条件都不成立 → `num` 保持** ✓ 正确。
- 第 124–127 行：`dout_valid` 的两态更新：
  | 条件 | `dout_valid` | 含义 |
  |---|---|---|
  | `fetch` | `1` | 装载了 → 输出有效 |
  | `!fetch && rd_en` | `0` | 没装载但被取走 → 输出变空 |
  - 优先级：`fetch` 优先（`if/else if`）。⭐ **为什么 `fetch` 优先？** 因为 `fetch` 的条件里包含了"或 `m_tready`"，而 `rd_en = m_tready && dout_valid`。当"本拍取走且本拍装载下一条"时，`fetch = 1` → `dout_valid` 保持 1 → **背靠背连续传输，`valid` 不中断** ✓ 这正是注释第 37 行说的"连续流传输时 dout_valid 保持连续"。

### 1.6 本模块的实测参数（本工程实际例化值）

`pd_feature_top.v:314-317` 例化时传 `DATA_W(EV_W) = 64`、`ADDR_W(FIFO_AW) = 8` → **深度 256 条 × 64 bit**。
- 容量：`256 × 64 bit = 16384 bit = 2 KB`。
- ⭐ **容量够不够？** 一个工频周期内每通道最多约 4 个峰值事件 + 1 个统计包 = 5 条，所以 256 深足够 ✓。
- ⚠️ **但注意 `o_overflow` 在 `pd_feature_top.v:329` 是悬空的**（`.overflow ()`）—— **4 个通道的事件 FIFO 溢出完全不可观测**。这是 B09 里的一条高优先级可观测性缺口。

---

## 2. `pd_axis_arb.v`（85 行）—— beat 级轮转仲裁

### 2.1 文件头（第 1–12 行）—— "为什么不做帧级锁定"

```
2  // pd_axis_arb.v  --  多通道事件流轮询仲裁器 (beat 级轮转)
4  // 【为什么不做帧级锁定】
5  //   本设计的事件流是"稀疏"的: 一个工频周期内只有少数几个超阈值峰值事件,
6  //   相邻事件间隔可达数百微秒到数毫秒。若采用"锁定某通道直到 tlast"的帧级
7  //   仲裁, 仲裁器会在等待本帧后续事件期间阻塞其他通道, 导致其余通道 FIFO
8  //   堆积甚至溢出丢数。
9  //   因此改为 beat 级轮转: 每成功传输一个 beat 后轮转到下一通道。
10 //   事件包中已携带 ch_id[1:0] 字段, PS 侧按通道重组即可; tlast 仍用于标记
11 //   周期统计包(帧尾), 只是不同通道的帧会交织出现。
```
⭐ **这是一段极好的"设计决策记录"**，把"为什么不用另一种方案"讲得很清楚：

- **事件流是稀疏的**：一个工频周期（20 ms）内每通道只有约 4 个峰值事件 + 1 个统计包，相邻事件间隔可达**数百微秒到数毫秒**。
- **如果做"帧级锁定"**（锁定某通道直到收到 `tlast`）：仲裁器在等本帧后续事件的期间（可能几毫秒）会**阻塞其他通道** → 其他通道的 FIFO 堆积 → 溢出丢数。**帧级仲裁在稀疏事件流上是灾难性的。**
- **所以改为 beat 级轮转**：每成功传输一个 beat 就轮转到下一通道。
- ⭐ **代价与补偿**：不同通道的帧会**交织**出现 → 所以**事件包里必须有 `ch_id` 字段**（PS 侧按通道重组）→ 这正是 `pd_feature_core` 把 `ch_id` 放进事件包的原因。
  - ⚠️ **注意 `tlast` 仍然用于标记帧尾**，但"帧交织"意味着 PS 收到的 `tlast` 可能属于任意通道 → **PS 侧必须按 `ch_id` 分组处理 `tlast`**。`pd_acquisition_core.c:94` 的判据是"最后一个字必须是 cycle 类型"，那是针对**一次 DMA 传输内部**，不是跨通道的全局判断。

### 2.2 模块与端口（第 15–34 行）

```
15 module pd_axis_arb #(
16     parameter integer NUM_CH = 4,
17     parameter integer DATA_W = 64,
18     parameter integer CH_W   = 2
19 )(
20     input  wire                      clk,
21     input  wire                      rst_n,
23     // 从端 (每通道一路)
24     input  wire [NUM_CH*DATA_W-1:0]  s_tdata,
25     input  wire [NUM_CH-1:0]         s_tvalid,
26     input  wire [NUM_CH-1:0]         s_tlast,
27     output wire [NUM_CH-1:0]         s_tready,
29     // 主端 (合并后)
30     output wire [DATA_W-1:0]         m_tdata,
31     output wire                      m_tvalid,
32     output wire                      m_tlast,
33     input  wire                      m_tready
34 );
```
- **从端是"打包成总线"的形式**：`s_tdata` 是 `NUM_CH × DATA_W = 256` 位（4 路 64 位拼接），`s_tvalid`/`s_tlast` 是 4 位向量，`s_tready` 也是 4 位向量。
- ⭐ **注意 `s_tdata` 用"宽总线 + 索引切片"而不是"数组端口"** —— 因为 Verilog-2001 的模块端口**不支持数组**（SystemVerilog 才支持）。所以所有多通道接口都用"宽总线 + `+: ` 切片"的写法。**这是本工程全部多通道接口的统一风格。**

### 2.3 数据拆包与循环优先级选择（第 36–69 行）

```
36     wire [DATA_W-1:0] tdata_arr [0:NUM_CH-1];
38     genvar g;
39     generate
40         for (g = 0; g < NUM_CH; g = g + 1) begin : GEN_TDATA
41             assign tdata_arr[g] = s_tdata[g*DATA_W +: DATA_W];
42         end
43     endgenerate
```
- **把宽总线"拆"成数组**，这样后面可以用 `tdata_arr[out_ch]` 索引。**综合后是零逻辑（纯连线）** ✓。
- ⭐ **这是"用 generate 把总线转成数组"的标准手法**，因为不能直接把数组当端口。

```
45     reg [CH_W-1:0] rr_ptr;
47     // ---- 循环优先级选择器: 从 rr_ptr 起找第一个有数据的通道 ----
48     reg [CH_W-1:0] sel;
49     reg            sel_valid;
50     integer i;
51     reg [CH_W-1:0] cand;
53     always @* begin
54         sel       = rr_ptr;
55         sel_valid = 1'b0;
56         for (i = 0; i < NUM_CH; i = i + 1) begin
57             cand = (rr_ptr + i) & (NUM_CH - 1);
58             if (!sel_valid && s_tvalid[cand]) begin
59                 sel       = cand;
60                 sel_valid = 1'b1;
61             end
62         end
63     end
```
⭐ **"循环优先级（round-robin）选择器"的实现**，逐行理解：
- 第 53 行 `always @*`：纯组合逻辑（无寄存），所以仲裁结果**每拍都可能变**。
- 第 54 行 `sel = rr_ptr`：**默认值**（防止未覆盖时产生 latch）。
- 第 56 行 `for (i = 0; i < NUM_CH; i++)`：**从 `rr_ptr` 开始，依次检查 4 个候选**。
  - 第 57 行 `cand = (rr_ptr + i) & (NUM_CH - 1)`：**取模回绕**。`NUM_CH - 1 = 3`，与 3 做按位与 = `mod 4`。所以当 `rr_ptr = 3`、`i = 1` 时，`cand = (3+1) & 3 = 0` ✓ 回到第 0 个通道。
  - ⭐ **为什么用 `& (NUM_CH-1)` 而不是 `% NUM_CH`？** 因为 `NUM_CH = 4` 是**2 的幂**，`& 3` 等价于 `% 4` 但**不需要除法器**。这是"用位运算代替模运算"的标准优化（前提是模数是 2 的幂）。
  - ⭐ **注意这要求 `NUM_CH` 是 2 的幂！** 如果是 3 或 5，`& (NUM_CH-1)` 就错了。**这是一个隐含参数约束**（B09 已记录）。
- 第 58 行 `if (!sel_valid && s_tvalid[cand])`：⭐ **`!sel_valid` 是关键** —— 保证"找到第一个有效的就停止后续更新"，从而实现"优先级"。**如果用 `if (s_tvalid[cand])`（不加 `!sel_valid`），则后面的通道会覆盖前面的，变成"最后一个有效通道优先"。**
- 第 59–61 行：记录选中通道与有效标志。

```
65     wire [CH_W-1:0] out_ch = sel;
67     assign m_tdata  = tdata_arr[out_ch];
68     assign m_tvalid = sel_valid;
69     assign m_tlast  = s_tlast[out_ch];
```
- 第 67 行：**64 位 4:1 多路选择器**（组合）。输入是 `tdata_arr`（4 路 64 位），选择信号是 `out_ch`。
- 第 68 行 `m_tvalid = sel_valid`：只有当选中通道真的有数据时才 valid ✓。
- 第 69 行 `m_tlast = s_tlast[out_ch]`：**把选中通道的 `tlast` 透传出去**。⭐ 这就是"帧交织"的机制：不同通道的 `tlast` 会交替出现在主端。

### 2.4 `tready` 与轮转（第 71–83 行）

```
71     generate
72         for (g = 0; g < NUM_CH; g = g + 1) begin : GEN_RDY
73             assign s_tready[g] = (out_ch == g) && m_tready && m_tvalid;
74         end
75     endgenerate
```
- ⭐ **`s_tready[g]` 只有在"被选中 **且** 主端 ready **且** 本通道有数据"时才为 1**。
- 这是"**独热（one-hot）**的 ready 广播"：**同一时刻最多只有一个通道的 ready 为 1**。
- ⚠️ **【观察 · 重要】** 注意 `s_tready[g]` 的表达式**不依赖 `s_tvalid[g]`**。所以如果一个通道被选中但**没有数据**，`s_tready` 仍可能为 1（当 `m_tready = 1`）。不过此时 `s_tvalid = 0`，所以**不会发生真实传输**（因为下游 FIFO 的 `wr_en = s_tvalid & s_tready`）✓ 安全。
  - **但**：`pd_axis_fifo` 内部用 `s_tvalid && !s_tready` 判 overflow，这里 `s_tready` 为 1 不会误报 ✓。
  - **另一个影响**：因为 `s_tready` 是组合输出（依赖 `m_tready`、`m_tvalid`、`sel`），而 `sel` 依赖 `s_tvalid`，所以**从各通道 FIFO 的 `m_tvalid` 到本仲裁器输出的 `s_tready` 之间是一条组合路径**。在 130 MHz 下这条路径不长（一个 4:1 选择 + 少量逻辑），应该没问题。

```
77     // 每成功传输一个 beat 即轮转一次, 保证公平且不阻塞任何通道
78     always @(posedge clk or negedge rst_n) begin
79         if (!rst_n)
80             rr_ptr <= {CH_W{1'b0}};
81         else if (m_tvalid && m_tready)
82             rr_ptr <= out_ch + 1'b1;
83     end
```
- ⭐ **轮转条件 = `m_tvalid && m_tready`**（一次成功传输）。所以是"**每成功传输一个 beat 就轮转**"，而不是"每拍轮转"。
  - **为什么？** 如果每拍轮转（不看是否真的传输了），那么在 `m_tready = 0` 期间指针会空转，可能跳过有数据的通道。
  - **用"成功传输"作判据**保证：每个被服务的通道都真的传走了一个 beat → 严格公平 ✓。
- `rr_ptr <= out_ch + 1'b1`：下次从"当前选中的下一个通道"开始。⭐ **注意没有做取模**（`out_ch = 3` 时 `rr_ptr` 变成 4 = `2'b00`，因为 `CH_W = 2` 位宽溢出后自然回绕到 0）✓ **巧妙利用了位宽溢出**。
  - ⚠️ **这要求 `NUM_CH` 是 2 的幂**（与第 57 行的 `& (NUM_CH-1)` 同一个约束）。若 `NUM_CH = 3`，`rr_ptr` 会取值 0,1,2,3 而 `cand = (rr_ptr + i) & 2` 完全错乱。**记入 B09。**

### 2.5 仲裁器的公平性与"事件交织"对 PS 的影响

**公平性分析**（4 通道、`m_tready` 常高）：
- 假设 4 个通道同时有数据：`rr_ptr = 0` → 选 ch0 → `rr_ptr = 1` → 选 ch1 → `rr_ptr = 2` → 选 ch2 → `rr_ptr = 3` → 选 ch3 → `rr_ptr = 0` → …… **严格的轮询，每通道每 4 拍得到一次服务** ✓。
- 若只有 ch2 有数据：`rr_ptr` 会一直停在 ch2（因为每次传输后 `rr_ptr = 3`，下一拍从 3 开始找到 0、1 都没数据，最后找到 ch2）→ **`rr_ptr` 在 3 和 2 之间来回** ✓ 不会阻塞。

⭐ **对 PS 的影响（必须理解）**：因为不同通道的事件会**交织**，所以 PS 收到的 AXI-Stream 数据是"**多通道复用的一条流**"。PS 必须：
1. 按 `type` 区分是峰值包还是统计包；
2. **按 `ch_id` 区分通道**（且记住：峰值包的 `ch_id` 在 `[26:25]`、统计包在 `[31:30]`）；
3. 按 `ch_id` 分别累积 `evt_seq` 序列号做丢帧检测。

⚠️ **【观察 · 严重可观测性问题】** `pd_acquisition_core.c:94` 的判据是：
```c
if ((u32)(rx[words - 1U] >> 56) != 1U) return fail("AXIS packet did not end in cycle word");
```
即"一包 DMA 数据的最后一个字必须是 type=1（统计包）"。**但因为四通道事件交织，最后一个字可能属于任意通道的统计包** —— 判据本身没错（只要是统计包就行），但**它不能用来判断"四个通道的帧都完整"**。B09 已记录这个语义陷阱。

---

## 3. 本篇易错点小结

1. ⭐ **BRAM 推断 6 条规则**（无复位写块、同步读、无条件读、无 `initial`、显式 `ram_style`、一写一读），违反任一条都**静默降级**为 LUTRAM，**只报 INFO**。
2. ⭐ **资源对比必须看 `RAMB36/MUXF*` 计数，不能只看 LUT%**：一个写法不当的 FIFO 能吃掉近 5000 LUT（实测 4852 → 36）。
3. ⭐ **"无条件读"必须配"读地址回指"**：`rd_addr = (num != 0) ? raddr : (raddr - 1)`。否则"valid=1 但数据被垃圾覆盖"（实测 90/120 个 beat 为 X）。
4. ⭐ **`s_tready` 必须用 `used = num + dout_valid`**，不能用 `num` —— 否则会覆盖未取走的数据（且**静默丢数据**）。
5. **`tlast` 打包进存储体最高位**，避免第二个数组再拉一份 BRAM。
6. **`num` 的三态更新用 `if/else if` 显式列出**，比 `num <= num + wr_en - fetch` 更清晰、更安全。
7. **`dout_valid` 的更新里 `fetch` 优先**，才能实现背靠背连续传输（`valid` 不中断）。
8. **仲裁器不做帧级锁定**：稀疏事件流上帧级仲裁会造成其他通道 FIFO 溢出。**改 beat 级轮转**。
9. ⭐ **`pd_axis_arb` 隐含要求 `NUM_CH` 是 2 的幂**：`& (NUM_CH-1)` 与 `rr_ptr` 的自然回绕都依赖这一点。若 `NUM_CH = 3` 会完全错乱。
10. **`if (!sel_valid && s_tvalid[cand])` 里的 `!sel_valid` 不能删**，否则优先级反转（变成"最后一个有效通道优先"）。
11. **`tlast` 会跨通道交织**，PS 必须按 `ch_id` 分组处理。
12. **`FWFT` 参数名与行为不一致**（行为固定为 FWFT 风格），是潜在误读点。
13. ⚠️ **`pd_axis_fifo.overflow` 在 `pd_feature_top` 里悬空** → 4 通道事件 FIFO 溢出**完全不可观测**。

---

**本卷下一篇**：`A07_特征寄存器与IP顶层.md` —— `pd_axil_regs` 的完整寄存器地图（含 v3.0 新增的 6+6 档门槛、APPLY_STATUS、FRAME_ACK）与两级 IP 顶层。
