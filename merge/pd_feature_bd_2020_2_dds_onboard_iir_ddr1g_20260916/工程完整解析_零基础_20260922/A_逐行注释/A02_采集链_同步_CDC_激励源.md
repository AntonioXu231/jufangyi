# A02 · 采集链：同步检测、跨时钟域与片上激励源

覆盖文件（全部在 `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/`）：

| 文件 | 行数 | 角色 |
|---|---|---|
| `adc_capture.v` | 32 | 单通道 ADC 输入打一拍（同步复位） |
| `async_fifo.v` | 120 | 通用异步 FIFO（格雷码指针 + 双同步器），**BRAM 推断范例** |
| `pd_adc_cdc.v` | 121 | ADC 采样域 → 系统时钟域的完整 CDC 前端（内部含 adc_capture + async_fifo） |
| `pd_sync_pulse.v` | 33 | 工频同步方波的 CDC 与"周期起始脉冲"生成 |
| `pd_dds_adc_source.v` | 123 | 片上四通道 DDS 激励源（板上测试模式下替代真实 ADC） |

> 本卷上一篇（A01）讲的是"位宽、地址、对齐"这类**静态约定**；本篇讲的是**数据如何从物理引脚一路走进系统时钟域**。这条链上任何一处出错，后面所有模块都拿不到正确的样本。

---

## 0. 先建立整体图景

真实板卡上，ADC 有**自己的位时钟**（本工程按 26 MHz 设计），它与 FPGA 的 130 MHz 系统时钟**没有固定相位关系**。工程里把"采集前端"分成两套并行存在的方案，由参数 `INPUT_CDC` / `adc_clk` 的接法决定用哪一套：

```
方案 A（当前 DDS 自激励实际走的路）：
  pd_dds_0 在 26 MHz 域产出 adc_data/adc_dv
     → pd_ddr_0 内部 pd_pack48 在 26 MHz 域完成 4 通道对齐 → 48 bit 字
     → 两个 48 bit 异步 FIFO（fifo_48_cdc）跨到 130 MHz
     → Path A 去 DDR 环形写；Path B 送给 pd_filter_0 / pd_feature_0
  （pd_feature_sys_top 参数 INPUT_CDC = 0，即"特征链不再自己做 CDC"）

方案 B（保留的兼容路径，INPUT_CDC = 1）：
  每通道独立 adc_capture → async_fifo(12 bit) → 按 CDC_RATIO 产生节拍
     → 12 bit 样本流交给 pd_feature_top
  （本工程 TB `tb_pd_feature_dds.v` 就是按 INPUT_CDC=1 例化的）
```

**理解这一点非常重要**：`pd_adc_cdc.v` + `adc_capture.v` + `async_fifo.v` 是**方案 B 的组件**，在当前上板配置里走的是方案 A（用 IP 核 `fifo_48_cdc` 而不是自研 `async_fifo`）。但 `async_fifo.v` 的价值在于它是整个工程**BRAM 推断的教学样板**，其注释里的三条"致命写法"经验被 `pd_axis_fifo.v`、`pd_prpd_ram.v` 反复引用。

---

## 1. `adc_capture.v`（32 行）—— 最小的一级输入寄存

```
1  `timescale 1ns / 1ps
2  // =====
3  // adc_capture.v  --  ADC 并行接口（12bit，偏移二进制）
4  // 输入 adc_data_in 在 adc_clk 上升沿被采样；仅当 adc_dv_in 有效时才认为
5  // 本拍数据有效（adc_dv_in 与 adc_data_in 同一时钟域，跟随 ADC 位时钟）。
6  //
7  // 四通道化说明：本模块保持单通道，由 pd_acq_top 用 generate 例化 4 份。
8  // =====
```
- 第 1 行 `` `timescale 1ns / 1ps ``：仿真时间单位与精度声明。（注意：文件中其余用到的 `$display` 里出现的"20MHz/100MHz"字样都是**早期命名遗留**，当前实际是 26/130 MHz。）
- 第 7 行提到 `pd_acq_top` —— **当前工程里不存在这个模块**（是历史命名）。这是注释与代码树的漂移，不影响功能。实际例化它的是 `pd_adc_cdc.v:44`。

```
9  module adc_capture #(
10     parameter WIDTH = 12
11 )(
12     input  wire              adc_clk,
13     input  wire              adc_rst_n,
14     input  wire [WIDTH-1:0]  adc_data_in,
15     input  wire              adc_dv_in,     // 本通道样本有效（adc_clk 域）
16     output reg  [WIDTH-1:0]  adc_data_out,
17     output reg               adc_data_vld
18 );
```
- 端口分四类：时钟、复位、输入数据+有效、输出数据+有效。**输入输出都带 `_vld`（valid）**，这是全工程的统一风格：数据永远和它的有效标志一起走，从不靠"每 N 拍来一个"这种隐含约定。

```
20     // 同步复位（驱动 FIFO 写使能等 RAMB 控制引脚，避免 REQP-1840 异步复位
21     // 释放瞬间毛刺损坏 BRAM 内容）。复位期间行为不变：下个时钟沿清零。
22     always @(posedge adc_clk) begin
23         if (!adc_rst_n) begin
24             adc_data_out <= 0;
25             adc_data_vld <= 1'b0;
26         end else begin
27             adc_data_out <= adc_data_in;
28             adc_data_vld <= adc_dv_in;
29         end
30     end
```
- **敏感表里只有 `posedge adc_clk`，没有 `negedge adc_rst_n`** —— 这就是"同步复位"。对比 `pd_sync_pulse.v:20` 用的是 `always @(posedge clk or negedge rst_n)`（异步复位），差异是有意的。
- 第 20–21 行的理由：**异步复位释放的瞬间，如果复位信号与时钟沿靠得很近，会形成亚稳态或窄毛刺**；而这个输出接下来要驱动 BRAM 的写使能控制引脚，代价高。所以这里选"同步复位"，让复位释放被时钟沿采样一次，代价是复位需要至少一个 `adc_clk` 周期才真正生效。
- 第 27–28 行是**两级独立寄存**：数据和有效标志各打一拍，保持拍对拍（同拍进入、同拍输出）。这个打拍的作用：
  1. 把组合路径切断（`adc_data_in` 来自引脚，引脚走线延迟 + 输入缓冲延迟都很大）；
  2. 让 `adc_data_out` 和 `adc_data_vld` **同拍有效**，下游不需要额外对齐。
- ⚠️ **观察**：第 23 行的 `if (!adc_rst_n)` 是"低有效复位"，但敏感表没有复位沿。这意味着复位**被采样**而非**立即生效**。如果调用者把 `adc_rst_n` 当作"异步复位"使用（比如用它去清一个外部状态），行为会不一致。工程内部所有调用者（`pd_adc_cdc.v:46`、`pd_ddr_wr_top.v:260`）传的都是经同步的复位，所以当前无害。

---

## 2. `async_fifo.v`（120 行）—— 全工程最重要的"BRAM 推断"教科书

这个模块表面上是个普通异步 FIFO，**真正的价值在文件头的 20 行注释**：它记录了一次"Vivado 静默把 BRAM 打散成触发器"的完整事故与修复。这一段必读。

### 2.1 文件头注释（第 1–21 行）

```
1  `timescale 1ns / 1ps
2  // =====
3  // async_fifo.v  --  通用异步 FIFO（格雷码指针 + 双同步器）
4  // 典型用法：ADC 时钟域 (wr) → 系统时钟域 (rd)
5  // 注意：本模块只保证两个时钟异步关系下的指针传递；调用方必须向每个时钟
6  // 域提供已同步释放的低有效复位。
7  // ---
8  // 【修订 v2 · Block RAM 打包】旧版有三个致命写法，导致 Vivado *静默* 把
9  // 存储体打散成触发器（12bit×64 → 768 个 FF/通道，4 通道共约 3072 FF）：
10 //   1) `initial` + for 循环整块初始化   -> 存储体被判定为"非 RAM 模板"
11 //   2) 组合读 `assign rd_data = mem[]`  -> 7 系列 BRAM 只有同步输出，
12 //      组合读会退化成 DEPTH:1 巨型多选器或分布式 RAM
13 //   3) 写块敏感表含 wr_rst_n            -> "RAM is sensitive to async reset"
14 // 修复后遵循与 pd_axis_fifo.v 相同的六条 BRAM 推断规则：
15 //   · 存储体写块【无复位】· 同步读 · 无条件读 · 无 initial · ram_style · 一写一读
16 // 结果：存储体映射为 1 个 RAMB18（双端口、读写独立时钟）。
17 //
18 // ⚠️ 时序契约变化：rd_data 由"组合输出"改为"同步输出"，
19 //    rd_en 拉高后【第 2 拍】数据才有效（多了 1 拍流水延迟）。
20 //    调用方 pd_adc_cdc 已同步加一级 rd_en_d 打拍，吞吐率不变。
```
**逐条理解这三个"致命写法"为什么致命**：

| # | 错误写法 | 为什么会被打散 | 正确写法 |
|---|---|---|---|
| 1 | `initial begin for(...) mem[i]=0; end` | Vivado 的 BRAM 识别模板要求存储体"只能通过同步写端口被写"。`initial` 块相当于"上电时并行写所有单元"，无法映射到只有一根地址线的 BRAM 端口 → 判定为"非 RAM 模板"→ 用触发器实现 | **完全不初始化存储体**（RAM 内容本来就是未定义的，靠"写入后才能读"的协议保证正确性） |
| 2 | `assign rd_data = mem[rd_ptr];` | 7 系列 BRAM 的**输出是同步的**（有输出寄存器）。组合读要求"地址一变输出立刻变"，BRAM 做不到 → 综合器只能用分布式 RAM（LUTRAM）或多选器实现 | **同步读**：`always @(posedge rd_clk) rd_data <= mem[addr];` |
| 3 | `always @(posedge wr_clk or negedge wr_rst_n)` 里含写存储体 | Vivado 报 "RAM is sensitive to asynchronous reset signal. this RTL style is not supported" → 存储体被打散到寄存器 | 存储体写块**只**用 `always @(posedge wr_clk)`，复位逻辑放到**另一个** always 块里 |

第 18–20 行是**代价**：改成同步读后，`rd_en` 拉高后的**第 2 拍**数据才有效（因为多了一级输出寄存器）。这叫"时序契约变化"——接口的时序行为变了，调用者必须同步修改。`pd_adc_cdc.v:105-109` 就是为此新增的 `rd_en_d` 打拍。

### 2.2 模块头与参数（第 22–38 行）

```
22 module async_fifo #(
23     parameter WIDTH     = 24,
24     parameter DEPTH     = 2048,  // 必须是 2 的幂
25     parameter RAM_STYLE = "block"   // "block"=BRAM / "distributed"=LUTRAM
26 )(
27     input  wire              wr_clk,
28     input  wire              wr_rst_n,
29     input  wire              wr_en,
30     input  wire [WIDTH-1:0]  wr_data,
31     output wire              full,
32
33     input  wire              rd_clk,
34     input  wire              rd_rst_n,
35     input  wire              rd_en,
36     output wire [WIDTH-1:0]  rd_data,
37     output wire              empty
38 );
```
- **两个时钟、两个复位**：写侧一套（`wr_clk/wr_rst_n/wr_en/wr_data/full`），读侧一套（`rd_clk/rd_rst_n/rd_en/rd_data/empty`）。这是标准异步 FIFO 接口形态。
- `DEPTH` 注释写"必须是 2 的幂"——**为什么？** 见 2.3 的格雷码指针：格雷码"每次只变 1 位"这个性质只在深度为 2 的幂、指针按二进制连续计数时成立。`pd_ddr_wr_top.v` 里用的是 IP 核，`pd_adc_cdc.v:58` 例化时传 `DEPTH(64)`。
- `RAM_STYLE` 是给综合器的提示属性（默认 "block"=BRAM，"distributed"=LUTRAM）。注意它只是**提示**，写法不对时综合器会无视它（这正是第 8–16 行事故的本质）。

```
40     localparam ADDR_W = $clog2(DEPTH);
41     localparam PTR_W  = ADDR_W + 1;
```
- `ADDR_W` = 地址位宽（DEPTH=64 → 6）。
- **`PTR_W = ADDR_W + 1`**：指针比地址**多一位**。多出来的最高位用来区分"写指针绕了一圈还是没绕"。这是异步 FIFO 的经典技巧，也是后面"判满要最高两位都取反"的根源。

### 2.3 存储体声明与格雷码（第 43–53 行）

```
43     // ---- 存储体: 禁止 initial 初始化, 禁止 ram_style 缺省 ---
44     (* ram_style = RAM_STYLE *) reg [WIDTH-1:0] mem [0:DEPTH-1];
```
- 第 44 行是存储体：一个"位宽 WIDTH、深度 DEPTH"的存储器。`(* ram_style = ... *)` 是 Verilog 属性（attribute）语法，等价于给综合器一个"请用 BRAM"的标记。
- **没有 `initial` 块，没有复位分支** —— 严格遵守 2.1 表格里的正确写法。

```
46     reg  [PTR_W-1:0] wr_ptr_bin = 0;
47     reg  [PTR_W-1:0] rd_ptr_bin = 0;
48     wire [PTR_W-1:0] wr_ptr_gray;
49     wire [PTR_W-1:0] rd_ptr_gray;
50
51     // 二进制→格雷
52     assign wr_ptr_gray = wr_ptr_bin ^ (wr_ptr_bin >> 1);
53     assign rd_ptr_gray = rd_ptr_bin ^ (rd_ptr_bin >> 1);
```
- **格雷码的作用**：二进制 `0111 → 1000` 会同时翻转 4 位。如果这 4 位被异步采样、且采样时刻正好落在跳变中途，读到的可能是 `0111`、`1111`、`0000` 等任意组合——指针就"错"了。格雷码保证**相邻两个值只差 1 位**，所以即使采样到中途，也只可能读到"前一个值"或"后一个值"，**绝不会读到不存在的中间值**。
- 转换公式 `g = b ^ (b >> 1)`，这是标准写法（`^` 是逐位异或）。

### 2.4 双同步器（第 55–70 行）

```
55     // 双同步：写指针的格雷码 → 读时钟域
56     // ASYNC_REG: 让两级同步器紧邻布局, 消除 TIMING-10 告警并压低亚稳态概率
57     (* ASYNC_REG = "TRUE" *) reg [PTR_W-1:0] wr_ptr_gray_sync1 = 0;
58     (* ASYNC_REG = "TRUE" *) reg [PTR_W-1:0] wr_ptr_gray_sync2 = 0;
59     always @(posedge rd_clk) begin
60         if (!rd_rst_n) {wr_ptr_gray_sync1, wr_ptr_gray_sync2} <= 0;
61         else           {wr_ptr_gray_sync2, wr_ptr_gray_sync1} <= {wr_ptr_gray_sync1, wr_ptr_gray};
62     end
```
- **这是 CDC 的核心结构**：把一个时钟域的信号，用另一时钟域的两级触发器连续采两拍。
- 第 61 行的移位写法要读懂。`{a, b} <= {c, d}` 是"拼接赋值"，等价于 `a <= c; b <= d;`，而且**同一拍同时生效**（非阻塞赋值的语义）。所以这一行实际是：
  - `wr_ptr_gray_sync2 <= wr_ptr_gray_sync1`
  - `wr_ptr_gray_sync1 <= wr_ptr_gray`
  即 `wr_ptr_gray` → `sync1` → `sync2`，两级串联，每级一拍。
- **为什么必须两级？** 一级触发器在采样异步信号时，可能进入"既不是 0 也不是 1"的**亚稳态**；它需要一定时间才能自行稳定。给它多一个时钟周期去稳定，第二级采到时就已经是干净的 0/1 了。这就是"两级同步器降亚稳态概率"的含义。
- `(* ASYNC_REG = "TRUE" *)` 属性告诉 Vivado："这两个寄存器是同步器链，请把它们**放得尽量靠近**，并且不要把第一级的输出扇出到别处。" 好处是①消除 `TIMING-10` 类告警；②距离近 → 走线短 → 亚稳态窗口更短。
- ⚠️ **格雷码在此处至关重要**：如果传的是二进制指针（多位同时变），两级同步器也无法保证"多位同时稳定"，可能采到非法组合。这就是"格雷码 + 两级同步器"必须成对使用的原因。
- 第 65–70 行是对称的：把读指针的格雷码同步到写时钟域，用于判满。

### 2.5 判空与**判满**（第 72–92 行）—— 全文件最精彩的一段

```
72     // 读时钟域的 empty 判定
73     wire [PTR_W-1:0] wr_ptr_gray_in_rd = wr_ptr_gray_sync2;
74     assign empty = (wr_ptr_gray_in_rd == rd_ptr_gray);
```
- **判空很简单**：读指针追上写指针就是空。因为两边都是格雷码，直接相等比较即可（同步后的写指针 vs 本地读指针，都在读时钟域，安全）。

```
76     // 写时钟域的 full 判定
77     //
78     // ⚠️ 经典陷阱：格雷码判"满"必须【最高两位都取反】，只反转最高位是错的。
79     //    设指针 PTR_W=N+1 位、深度 2^N，「满」= wr 领先 rd 整一圈 (2^N)：
80     //        bin_wr[PTR_W-1]   = ~bin_rd[PTR_W-1]
81     //        bin_wr[PTR_W-2:0] =  bin_rd[PTR_W-2:0]
82     //    转成格雷码（g = b ^ (b>>1)）后变成：
83     //        gray_wr[PTR_W-1]   = ~gray_rd[PTR_W-1]    <- 最高位取反
84     //        gray_wr[PTR_W-2]   = ~gray_rd[PTR_W-2]    <- 次高位【也要】取反
85     //        gray_wr[PTR_W-3:0] =  gray_rd[PTR_W-3:0]  <- 其余位相等
86     //    只反转最高位时 full 永远不会拉高（脚本遍历 4096 种指针组合验证：
87     //    错误判据命中 0/4096，正确判据命中 4096/4096），FIFO 会静默溢出。
88     wire [PTR_W-1:0] rd_ptr_gray_in_wr = rd_ptr_gray_sync2;
89     wire [PTR_W-1:0] full_cmp_gray     = {~rd_ptr_gray_in_wr[PTR_W-1],
90                                           ~rd_ptr_gray_in_wr[PTR_W-2],
91                                            rd_ptr_gray_in_wr[PTR_W-3:0]};
92     assign full = (wr_ptr_gray == full_cmp_gray);
```
**这段推导必须自己会走一遍**，因为它是"格雷码 FIFO 最常见的 bug"：

- 设指针宽度 `PTR_W = N+1`（比地址多一位），实际深度 `2^N`。
- "FIFO 满"的定义是：**写指针比读指针整整领先一圈（2^N）**。
- 用二进制表示，这个条件就是：**最高位相反，其余位相同**：
  `bin_wr[N] = ~bin_rd[N]`，`bin_wr[N-1:0] = bin_rd[N-1:0]`。
  （直观理解：读指针在"第 0 圈"，写指针走到"第 1 圈"的同一位置，就差满一圈。）
- 现在把两边都转成格雷码。注意格雷码的定义 `g[k] = b[k] ^ b[k+1]`，所以**某一位 `b[k]` 被取反，会影响 `g[k]` 和 `g[k+1]` 两位**。
  - 最高位 `b[N]` 取反 → `g[N]` 取反（`g[N] = b[N]`，最高位无更上位），同时 `g[N-1]` 也受影响……
  - 严谨推导（注释给出的结论）：`gray_wr[N] = ~gray_rd[N]`、`gray_wr[N-1] = ~gray_rd[N-1]`、其余位相等。
- **如果只反转最高位**（很多网上代码的错误写法），那么比较**永远不会成立** → `full` 永远为 0 → FIFO 会一直写下去把存储体覆盖，**静默丢数据**。注释里说"脚本遍历 4096 种指针组合：错误判据命中 0/4096，正确判据命中 4096/4096"——这是一个非常标准的"用穷举验证判据"的做法。
- 第 88–92 行就是正确判据的实现：把同步过来的读指针格雷码的**最高两位取反**，其余位保持，再与写指针格雷码比较。

### 2.6 写/读使能与存储体操作（第 94–118 行）

```
94     wire wr_act = wr_en && !full;
95     wire rd_act = rd_en && !empty;
```
- **先算"实际动作"**：`wr_act` 表示"这一拍真的要写入"（有写请求且不满），`rd_act` 同理。把"请求"和"实际发生"分开，是所有 FIFO 的标准写法，避免"满了还在写"或"空了还在读"。

```
97     // ---- 存储体写: 敏感表【不含复位】，否则 BRAM 推断失败 ---
98     always @(posedge wr_clk) begin
99         if (wr_act) mem[wr_ptr_bin[ADDR_W-1:0]] <= wr_data;
100    end
```
- **敏感表里只有 `posedge wr_clk`**，没有复位沿、也没有复位分支 —— 这是 2.1 表格第 3 条的修复。
- ⚠️ 关键细节：**写地址用的是 `wr_ptr_bin[ADDR_W-1:0]`（去掉最高位）**。因为最高位是"圈数标志"，不是存储体地址。

```
102    // ---- 存储体读: 无条件同步读（BRAM 推断的关键，禁止门控）--------
103    reg [WIDTH-1:0] rd_data_reg;
104    always @(posedge rd_clk) begin
105        rd_data_reg <= mem[rd_ptr_bin[ADDR_W-1:0]];
106    end
107    assign rd_data = rd_data_reg;
```
- **"无条件同步读"**：`always` 块里没有 `if`，每个时钟沿都读一次。
- **为什么必须无条件？** 注释在 `pd_axis_fifo.v:18-21` 里给了实测数据：门控读（`if (fetch) dout <= mem[rp]`）→ 352 个 LUT 的 LUTRAM；无条件读 → 1 个 RAMB36。原因是 Vivado 的 BRAM 推断模板要求"读端口始终在被使用"，加了 `if` 就变成"带使能的非标准模板"。
- 代价：无条件读意味着"输出寄存器的内容永远等于 `mem[当前读地址]`"。如果读地址乱走（例如停在未写过的单元），输出就会被垃圾覆盖。**这个代价在 `pd_axis_fifo.v` 里用"读地址回指"解决了**（见 A06）。

```
109    // ---- 指针: 复位留在独立块里，不污染存储体 ---
110    always @(posedge wr_clk) begin
111        if (!wr_rst_n) wr_ptr_bin <= 0;
112        else if (wr_act) wr_ptr_bin <= wr_ptr_bin + 1'b1;
113    end
114
115    always @(posedge rd_clk) begin
116        if (!rd_rst_n) rd_ptr_bin <= 0;
117        else if (rd_act) rd_ptr_bin <= rd_ptr_bin + 1'b1;
118    end
```
- 复位被**单独放进指针的 always 块**里，与存储体块完全分离。这就是 2.1 表格第 3 条的实现方式。
- 指针各自在自己的时钟域递增，**只有当真正发生读写时才递增**。

### 2.7 `async_fifo` 的可复用结论

> 在 Vivado 里写 BRAM，必须同时满足这 6 条，缺一条就会被静默降级：
> ① 存储体写块**无复位**；② **同步读**；③ 读**无条件**；④ **无 `initial`**；⑤ 显式 `ram_style`；⑥ 一写一读（simple dual port）。
> 而且**降级只报 INFO 级提示，没有 warning** —— 极容易漏掉。验证方法：看综合/实现报告里的 `RAMB36/RAMB18` 数目，而不是看 LUT。

---

## 3. `pd_adc_cdc.v`（121 行）—— 把方案 B 组装起来

```
1  // =====
2  // pd_adc_cdc.v  --  单通道 ADC 采集 + 跨时钟域 FIFO (SAMPLE_HZ ADC -> CLK_HZ 系统时钟)
3  // ---
4  // 定位 (对齐《pd_feature IP 接口契约 v2》§2 末段):
5  //   pd_feature_top 为单时钟域设计, 期望 adc_data(12bit offset binary) +
6  //   adc_dv(每 CDC_RATIO 拍一个样本) 已在系统时钟域内。真实 ADC 随采样时钟
7  //   变化, 与本模块异步, 需在其前端插一级异步 FIFO 做 CDC。
8  //
9  //   本模块即该 CDC 前端 (取 project_2 的 adc_capture + async_fifo 思路, 但
10 //   保持 12bit offset binary, 不做 Q1.23 转换 —— pd_feature_core 内部会自行
11 //   完成 offset-binary -> 补码 -> 电荷标定):
12 //     adc_data/adc_dv(adc_clk 域, SAMPLE_HZ)
13 //        -> adc_capture(打拍) -> async_fifo(adc_clk->clk)
14 //        -> CLK_HZ 域按 CDC_RATIO(=CLK_HZ/SAMPLE_HZ) 生成节拍 adc_dv_100
15 //        -> adc_data_100 / adc_dv_100 (契约视图: 每 CDC_RATIO 拍一个样本)
16 //
17 // 速率切换 (只改 CLK_HZ / SAMPLE_HZ 两个参数):
18 //     130M/65M -> 2:1   (生产目标)
19 //     130M/26M -> 5:1   (当前默认, 测试)
20 //     130M/13M -> 10:1  (测试)
21 //   注意: SAMPLE_HZ 必须整除 CLK_HZ, 否则节拍不齐 (仿真会用 $display 报错)
22 // =====
```
**这 22 行注释说清了三件事**：
1. 为什么需要这个模块：`pd_feature_top` 是**单时钟域设计**（期望 `adc_dv` 是"每 N 拍一个脉冲"），而真实 ADC 是异步的 → 必须插异步 FIFO。
2. 为什么**不做码制转换**：数据保持 12 bit offset binary 直通；"offset binary → 补码 → 电荷标定"三步由 `pd_feature_core` 内部完成（见 `pd_feature_core.v:180` 与 `:375-393`）。这是**职责划分**：CDC 只负责"搬数据过时钟域"，不负责"理解数据"。
3. 速率切换只需改两个参数，且 `SAMPLE_HZ` 必须整除 `CLK_HZ`。当前 130/26 = 5。

```
25 module pd_adc_cdc #(
26     parameter integer ADC_W     = 12,
27     parameter integer DEPTH     = 64,           // 必须是 2 的幂 (格雷码指针)
28     parameter integer CLK_HZ    = 130000000,    // 系统时钟 (Hz), PL 主时钟
29     parameter integer SAMPLE_HZ = 26000000      // ADC 采样率 (Hz), 必须整除 CLK_HZ
30 )(
31     input  wire              adc_clk,     // ADC 位时钟 (20MHz)
32     input  wire              adc_rst_n,   // 异步复位 (低有效), 两域共用
33     input  wire              clk,         // 系统时钟 (100MHz)
34     input  wire              rst_n,       // 系统域异步复位 (低有效)
35     input  wire [ADC_W-1:0]  adc_data,    // offset binary, adc_clk 域
36     input  wire              adc_dv,      // 样本有效, adc_clk 域
37     output reg  [ADC_W-1:0]  adc_data_100,// 100MHz 域 12bit offset binary
38     output reg               adc_dv_100   // 100MHz 域样本有效 (每 5 拍一个)
39 );
```
- 参数是**整数频率值**（Hz），不是分频比。设计者选择"从频率反推分频比"，好处是 BD 里改 `SAMPLE_HZ` 就自动改变节拍，不易写错。
- ⚠️ **【观察】端口注释与端口名都还写着 "20MHz/100MHz"**（第 31、33、37、38 行），而参数默认值已经是 130/26。这是**命名与注释漂移**：端口名 `adc_data_100` 里的 "100" 已经不再表示 100 MHz。**功能正确，但读代码时必须以参数默认值和 BD 的实际连接为准**。→ 收集到 B09。

### 3.1 内部三件套（第 41–69 行）

```
41     // 1) ADC 接口采样 (adc_clk 域)
42     wire [ADC_W-1:0] cap_out;
43     wire             cap_vld;
44     adc_capture #(.WIDTH(ADC_W)) u_cap (
45         .adc_clk     (adc_clk),
46         .adc_rst_n   (adc_rst_n),
47         .adc_data_in (adc_data),
48         .adc_dv_in   (adc_dv),
49         .adc_data_out(cap_out),
50         .adc_data_vld(cap_vld)
51     );
```
- 第 44–51 行：例化 `adc_capture` 做一级输入寄存（见本篇第 1 节）。注意用的是 `.WIDTH(ADC_W)` 的**命名端口例化**（推荐写法，位置无关）。

```
53     // 2) 跨时钟异步 FIFO (adc_clk -> clk), 12bit
54     //    写使能在非空且非满时; 读侧由 100MHz 域节拍控制
55     wire fifo_full, fifo_empty;
56     wire [ADC_W-1:0] fifo_dout;
57     reg  rd_en;
58     async_fifo #(.WIDTH(ADC_W), .DEPTH(DEPTH)) u_fifo (
59         .wr_clk   (adc_clk),
60         .wr_rst_n (adc_rst_n),
61         .wr_en    (cap_vld && !fifo_full),
62         .wr_data  (cap_out),
63         .full     (fifo_full),
64         .rd_clk   (clk),
65         .rd_rst_n (rst_n),
66         .rd_en    (rd_en),
67         .rd_data  (fifo_dout),
68         .empty    (fifo_empty)
69     );
```
- 第 61 行 `.wr_en(cap_vld && !fifo_full)`：**只在"样本有效"且"FIFO 未满"时写**。注意这里没有用 `async_fifo` 内部的 `wr_act`——调用者提前判了 `!full`，属于"双保险"（`async_fifo` 内部还会再判一次）。
- 第 65 行 `.rd_rst_n(rst_n)`：读侧用系统域复位。
- ⚠️ **【观察】第 54 行注释"写使能在非空且非满时"是笔误**——写侧不需要判"非空"。这种注释笔误不影响功能，但会误导读者。

### 3.2 分频节拍与参数自检（第 71–93 行）

```
71     // 3) 系统时钟域的采样节拍: 每 CDC_RATIO 拍产生一个样本有效脉冲
72     //    CDC_RATIO = CLK_HZ / SAMPLE_HZ, 必须整数
73     //    130M 下: 65M->2:1, 26M->5:1(当前默认), 13M->10:1
74     localparam integer CDC_RATIO = CLK_HZ / SAMPLE_HZ;
75     localparam [7:0]   CDC_TERM  = CDC_RATIO - 8'd1;       // 计数终值 = RATIO-1 (RATIO<=256)
```
- `CDC_RATIO` 是**整数除法**（Verilog 的 `/` 对整数是截断除法）。所以如果 `CLK_HZ` 不能被 `SAMPLE_HZ` 整除，结果会被截断而不是报错——这就是第 77–86 行的自检要解决的问题。
- `CDC_TERM = RATIO - 1`：计数器从 0 数到 `RATIO-1` 共 RATIO 拍，所以"计数终值"是 RATIO-1。

```
77     // 参数自检 (仅仿真有效, 综合忽略 $display)
78     initial begin
79         if (CDC_RATIO < 1 || (CDC_RATIO * SAMPLE_HZ) != CLK_HZ)
80             $display("[ERROR] pd_adc_cdc: CLK_HZ=%0d 与 SAMPLE_HZ=%0d 非整数比(=%0d), CDC 节拍将不齐! " +
81                      "130M 下请选: 65M(2:1) / 26M(5:1) / 13M(10:1)",
82                      CLK_HZ, SAMPLE_HZ, CDC_RATIO);
83         else
84             $display("[INFO ] pd_adc_cdc: CDC ratio = %0d:1 (CLK_HZ=%0d, SAMPLE_HZ=%0d)",
85                      CDC_RATIO, CLK_HZ, SAMPLE_HZ);
86     end
```
- **判据是 `CDC_RATIO * SAMPLE_HZ == CLK_HZ`**（"乘回去验证"），而不是判"余数为 0"——因为 Verilog 里拿不到余数，乘回去是等价的判定方式。
- `initial` 块里放 `$display`：**综合时会被忽略**（综合工具不处理 `initial` 中的系统任务），仿真时打印。这是"参数自检"的标准做法：不增加硬件开销，但在仿真里立刻暴露参数错误。

```
88     reg [7:0] ce_div;
89     wire      ce_sample = (ce_div == 8'd0);
90     always @(posedge clk or negedge rst_n) begin
91         if (!rst_n) ce_div <= 8'd0;
92         else        ce_div <= (ce_div == CDC_TERM) ? 8'd0 : ce_div + 8'd1;
93     end
```
- 这是一个**模 CDC_RATIO 的计数器**（0 → 1 → … → CDC_TERM → 0）。
- **为什么判据是 `ce_div == 0` 而不是 `== CDC_TERM`？** 两种写法都能产生"每 RATIO 拍一个有效拍"，差别只在相位（哪一个计数状态被当作"有效拍"）。选 0 作为有效拍的好处是：复位后 `ce_div = 0`，**立刻第一个周期就产生有效脉冲**，不需要额外等一个 RATIO。这种"复位即有效"的细节在系统启动时序上很重要。

### 3.3 读控制与输出寄存（第 95–119 行）

```
95     // 4) 节拍且 FIFO 非空时读
96     //    注: async_fifo v2 改为同步读(BRAM 输出寄存器), rd_en 拉高后第 2 拍
97     //        数据才有效, 故下面用 rd_en_d 再打一拍对齐。
98     always @(posedge clk or negedge rst_n) begin
99         if (!rst_n) rd_en <= 1'b0;
100        else        rd_en <= ce_sample && !fifo_empty;
101    end
```
- `rd_en` = "到了采样节拍，且 FIFO 里有数据"。
- 注意 `rd_en` 本身是**寄存输出**（`rd_en <= ...`），也就是说这个条件在下一拍才生效。这已经构成了第 1 拍延迟。

```
103    // 5) 输出寄存 (adc_dv_100 与 adc_data_100 同一拍有效; 数据保持到下次读出)
104    //    读节拍间隔 = CDC_RATIO (>=2 拍), 额外 1 拍流水延迟不影响吞吐率。
105    reg rd_en_d;
106    always @(posedge clk or negedge rst_n) begin
107        if (!rst_n) rd_en_d <= 1'b0;
108        else        rd_en_d <= rd_en;
109    end
110
111    always @(posedge clk or negedge rst_n) begin
112        if (!rst_n) begin
113            adc_data_100 <= {ADC_W{1'b0}};
114            adc_dv_100   <= 1'b0;
115        end else begin
116            adc_dv_100 <= rd_en_d;
117            if (rd_en_d) adc_data_100 <= fifo_dout;
118        end
119    end
```
- **这里的 `rd_en_d` 就是 2.1 里说的"时序契约变化"的补偿**：`async_fifo` 改成同步读后，`rd_en` 拉高后第 2 拍数据才在 `fifo_dout` 上。所以：
  - 第 1 拍：`rd_en` 寄存生效（发出读请求）；
  - 第 2 拍：BRAM 输出寄存器更新，`fifo_dout` 有效 → 此时 `rd_en_d` 正好也为 1；
  - 第 3 拍：`adc_dv_100 <= rd_en_d` 让 valid 与数据同时产出。
  
  注释里说"额外 1 拍流水延迟不影响吞吐率"——因为节拍间隔 `CDC_RATIO = 5` 拍，多 1 拍流水不影响每 5 拍产出一个样本。
- 第 117 行 `if (rd_en_d) adc_data_100 <= fifo_dout;` 是**条件写数据**（"数据保持到下次读出"）：只在有效拍更新数据，其余拍保持。这与 `async_fifo` 内部"无条件读"并不矛盾——无条件读是为了 BRAM 推断，这里条件写是为了让输出数据在非有效拍保持稳定，方便下游按 `adc_dv_100` 采样。

### 3.4 `pd_adc_cdc` 的一个真实隐患

**【观察】** 第 74 行 `localparam integer CDC_RATIO = CLK_HZ / SAMPLE_HZ;` 用的是整数除法。当前 130 MHz / 26 MHz = **5 恰好整除**，所以工作正常。但如果将来把 ADC 时钟换成"外部真实 26 MHz 晶振"，而 PL 主时钟是 130 MHz 由 MMCM 从 50 MHz 晶振倍频而来，两者实际频率比可能不是精确的 5.0000（例如 129.9998/26.0001）。此时：
- `CDC_RATIO` 仍然是 5，节拍器按 130 MHz 的 5 分频走；
- 但 ADC 实际以 26.0001 MHz 往里写；
- **写比读略快 → FIFO 会缓慢堆满**（或反过来缓慢耗空）。
- 按 5.0009 的比值估算，64 深度的 FIFO 会在约 44 ms 内耗空（这是 B09 里记录的量级）。
- 当前 DDS 模式下 MMCM 分频是精确的 26.000 MHz，所以**现在没有这个问题**；它是"接真实 ADC 时必须重新评估"的项目。

---

## 4. `pd_sync_pulse.v`（33 行）—— 工频同步信号的 CDC 与边沿提取

```
1  `timescale 1ns / 1ps
2  // =====
3  // pd_sync_pulse.v -- 外部工频同步输入的 CDC 与周期起始脉冲
4  // =====
5  module pd_sync_pulse (
6      (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME CLK, FREQ_HZ 130000000, ASSOCIATED_RESET rst_n" *)
7      (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
8      input  wire clk,
9      (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME RST, POLARITY ACTIVE_LOW" *)
10     (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst_n RST" *)
11     input  wire rst_n,
12     input  wire sync_in,
13     output wire sync_level,
14     output wire cycle_start
15 );
```
- 第 6–11 行的 `(* X_INTERFACE_PARAMETER = ... *)` 与 `(* X_INTERFACE_INFO = ... *)` 是**给 Vivado IP Integrator 看的元数据**：它们告诉 Block Design"这个端口是时钟/复位接口，频率 130 MHz，复位低有效"。有了这些属性，BD 里就能自动连线、自动做时钟域推断。**它们不改变综合结果**，只影响 BD 的界面行为。
- 这个模块**没有中间层**（不像 pd_adc_cdc 有 adc_capture + async_fifo），它是纯 CDC 同步器 + 边沿检测。

```
17     (* ASYNC_REG = "TRUE" *) reg [1:0] sync_ff;
18     reg sync_d;
19
20     always @(posedge clk or negedge rst_n) begin
21         if (!rst_n) begin
22             sync_ff <= 2'b00;
23             sync_d  <= 1'b0;
24         end else begin
25             sync_ff <= {sync_ff[0], sync_in};
26             sync_d  <= sync_ff[1];
27         end
28     end
29
30     assign sync_level  = sync_ff[1];
31     assign cycle_start = sync_ff[1] & ~sync_d;
```
- 第 25 行 `sync_ff <= {sync_ff[0], sync_in}`：又见拼接移位。等价于 `sync_ff[1] <= sync_ff[0]; sync_ff[0] <= sync_in;`。**这是一个两级同步器**（`sync_in` → `sync_ff[0]` → `sync_ff[1]`）。
- 第 26 行 `sync_d <= sync_ff[1]`：把同步后的电平**再打一拍**，得到"上一拍的同步电平"。
- 第 30 行 `sync_level`：输出的同步后电平（已在 130 MHz 域）。
- 第 31 行 `cycle_start = sync_ff[1] & ~sync_d`：**上升沿检测**的标准写法——"这一拍是高、上一拍是低"就是上升沿。输出是一个**单拍脉冲**（宽 7.69 ns @130 MHz）。
- 这个 `cycle_start` 就是契约里说的"周期起始脉冲"，它有两个下游：
  1. `pd_ddr_0/cycle_start` —— 环形写入管理器用它作为"冻结快照的起止边界"（见 A08）；
  2. `pd_feature_core` 里的 `sync_in` —— 特征核自己再做一次边沿检测（见 A04 的 `sync_rise`）。
  
  ⚠️ **注意**：BD 里 `pd_dds_0/sync_in` 直接连给 `sync_pulse_0/sync_in` **和** `pd_feature_0/sync_in`（同一根网）。也就是说 `pd_sync_pulse` 产出的 `cycle_start` 只给 DDR 支路用；特征支路自己另做边沿检测。这两条路各自检测同一个源信号，**上游是同一个 26 MHz 域的 `sync_in`**，所以不存在"两路检测时刻不同"的问题。
- ⚠️ **【观察】第 20 行用异步复位（`negedge rst_n`），但 `pd_sync_pulse` 的 `rst_n` 来自 `rst_ps7_0/peripheral_aresetn`（130 MHz 域）**。因为整个模块都在 130 MHz 域，异步复位在这里是安全且常见的（复位释放由工具自动处理，P&R 会保证复位沿不落在时钟沿附近）。这与 `adc_capture.v` 选同步复位的原因不同——那里是因为输出驱动 BRAM 控制引脚。

---

## 5. `pd_dds_adc_source.v`（123 行）—— 片上"假 ADC"

这是当前上板测试的**核心前提**：没有模拟前端，用 PL 内的逻辑生成"看起来像四通道 ADC 输出"的数据流。

```
1  `timescale 1ns / 1ps
2  // =====
3  // pd_dds_adc_source.v -- synthesizable on-chip four-channel ADC test source
4  // ---
5  // Board-test mode only. The source runs from the internally generated 26 MHz
6  // ADC clock and emits one valid sample on every clock edge.
7  // It emits four low-amplitude background waveforms plus one different PD pulse
8  // per channel in every 20 ms (50 Hz) mains cycle.
9  //
10 // This verifies: DDS source -> 26 MHz to 130 MHz CDC -> pd_ddr/pd_feature
11 // -> AXIS. It deliberately does not verify physical ADC pins or I/O timing.
12 // =====
```
- 第 5 行："仅用于板上测试模式。源跑在内部生成的 26 MHz ADC 时钟上，**每个时钟沿都输出一个有效样本**。" → 因为它与采样时钟同源，所以 `adc_dv` 可以恒为 1（见第 107 行）。
- 第 10–11 行明确了**它能验证什么、不能验证什么**：能验证"DDS 源 → 26→130 MHz CDC → DDR/特征链 → AXIS"整条数字通路；**不能**验证物理 ADC 引脚与 I/O 时序。这是非常值得学习的"验证边界声明"。

```
13 module pd_dds_adc_source #(
14     parameter integer CLK_HZ          = 26000000,
15     parameter integer SAMPLE_HZ       = 26000000,
16     parameter integer SYNC_HZ         = 50,
17     parameter integer ADC_W           = 12,
18     parameter integer NUM_CH          = 4
19 )(
20     input  wire                     clk,
21     input  wire                     rst_n,
22     output reg  [NUM_CH*ADC_W-1:0]  adc_data,
23     output reg  [NUM_CH-1:0]        adc_dv,
24     output reg                      sync_in
25 );
26
27     localparam integer CLK_PER_SAMPLE = CLK_HZ / SAMPLE_HZ;
28     localparam integer SYNC_SAMPLES   = SAMPLE_HZ / SYNC_HZ;
```
- 第 27 行：`CLK_PER_SAMPLE = 26M/26M = 1`。BD 里 `pd_dds_0/clk` 接的就是 `clk_out2`（26 MHz），所以"每个时钟一个样本"。
- 第 28 行：`SYNC_SAMPLES = 26M/50 = 520,000` —— **一个 50 Hz 工频周期内的 ADC 样本数**。这个数字是整个工程的关键标尺：一份 3,120,000 字节的快照正好 = 520,000 × 6 字节 = 恰好一个工频周期（见 A01 第 2.5 节）。

### 5.1 三个函数（第 35–71 行）

```
35     function integer tri_wave;
36         input [7:0] phase;
37         integer magnitude;
38         begin
39             magnitude = phase[7] ? (127 - phase[6:0]) : phase[6:0];
40             tri_wave = magnitude - 64;
41         end
42     endfunction
```
- **三角波函数**，输入是 8 bit 相位（0…255）。
- 第 39 行：`phase[7]` 是最高位。若为 0（相位 0…127）→ `magnitude = phase[6:0]`（0…127，上升）；若为 1（相位 128…255）→ `magnitude = 127 - phase[6:0]`（127…0，下降）。
- 第 40 行：`tri_wave = magnitude - 64` → 值域 **−64 … +63**，是一个以 0 为中心的对称三角波。注意它是**有符号**的（`integer` 类型），后续加基线用。

```
44     function integer pd_pulse;
45         input [18:0] sample_pos;
46         input [18:0] centre;
47         begin
48             if (sample_pos == centre)
49                 pd_pulse = 720;
50             else if (sample_pos == centre + 19'd1)
51                 pd_pulse = 600;
52             else if (sample_pos == centre + 19'd2)
53                 pd_pulse = 460;
54             else if (sample_pos == centre + 19'd3)
55                 pd_pulse = 300;
56             else
57                 pd_pulse = 0;
58         end
59     endfunction
```
- **放电脉冲函数**：在 `sample_pos == centre` 起连续 4 个采样点产生幅度 720/600/460/300 的衰减脉冲，其余样本为 0。
- **这就是"模拟一个局部放电脉冲"**：真实的 PD 脉冲是纳秒-微秒级的快速尖峰，用一个 4 点衰减序列来近似（在 26 MSPS 下 4 点 = 154 ns）。
- 注意幅度是**逐步衰减**的（720 → 600 → 460 → 300）而不是方波，这样更接近真实脉冲的拖尾形状。

```
61     function [11:0] saturate_u12;
62         input integer sample;
63         begin
64             if (sample < 0)
65                 saturate_u12 = 12'd0;
66             else if (sample > 4095)
67                 saturate_u12 = 12'hFFF;
68             else
69                 saturate_u12 = sample[11:0];
70         end
71     endfunction
```
- **饱和函数**：把可能越界的整数值压到 12 bit 无符号范围 `[0, 4095]`。
- **为什么需要它？** 基线 + 三角波 + 脉冲可能超过 4095（例如 ch0 基线 2048 + 脉冲 720 + 三角波 15 ≈ 2783，不会越界；但 ch3 基线 2062 + 720 + 三角波 + 15 ≈ 2797 也不会）。**当前参数下其实不会越界**，但函数仍然保留——因为如果调整幅度参数，越界就会发生，而"饱和"比"回绕"（从 4095 跳到 0）安全得多。这是防御性设计。
- **为什么用无符号 12 bit？** 因为 `pd_pack48.v` 与下游都按"offset binary"（偏移二进制）理解：**0x800 = 2048 = 零电平**，0 = 最负，4095 = 最正。所以 DDS 输出必须是无符号偏移二进制，不能是补码。

### 5.2 四个通道的组合（第 73–81 行）

```
73     wire sample_ce = (tick_div == CLK_PER_SAMPLE - 1);
74     wire [11:0] ch0_next = saturate_u12(2048 + (tri_wave(phase0) >>> 2) +
75                                          pd_pulse(cycle_sample, 19'd65000));
76     wire [11:0] ch1_next = saturate_u12(2056 + (tri_wave(phase1) >>> 2) +
77                                          pd_pulse(cycle_sample, 19'd195000));
78     wire [11:0] ch2_next = saturate_u12(2038 + (tri_wave(phase2) >>> 1) +
79                                          pd_pulse(cycle_sample, 19'd325000));
80     wire [11:0] ch3_next = saturate_u12(2062 + (tri_wave(phase3) >>> 2) +
81                                          pd_pulse(cycle_sample, 19'd455000));
```
逐通道拆解（这就是"每通道参数表"）：

| 通道 | 基线（DC 偏移） | 三角波幅度 | 脉冲位置（一个周期内的样本序号） | 脉冲幅度 |
|---|---|---|---|---|
| ch0 | **2048** | `>>>2` → ±16 | 65,000 | 720/600/460/300 |
| ch1 | **2056** | `>>>2` → ±16 | 195,000 | 600/… |
| ch2 | **2038** | `>>>1` → ±32 | 325,000 | 460/…（基线最低但幅度最大） |
| ch3 | **2062** | `>>>2` → ±16 | 455,000 | 300/… |

- `>>> 2` 是**算术右移 2 位** = 除以 4。三角波值域 ±64 → ±16。`>>> 1` = 除以 2 → ±32。
- **为什么四通道要设不同基线（2048/2056/2038/2062）？** 因为要模拟真实四通道前端的**直流偏置不一致**，同时让每个通道的快照统计量（min/max/mean）具有**可区分的特征值**——这样 PS 侧 dump 数据时能一眼看出"哪一块是哪一路"，也能验证通道解包（`{ch3,ch2,ch1,ch0}` 顺序）没搞错。工程里已实测的对照组是 **min = 2032/2040/2006/2046**（= 基线 − 16/16/32/16），与这里的参数完全自洽。
- **为什么四个脉冲位置错开（65k/195k/325k/455k）？** 这样每个通道的放电脉冲落在工频周期的不同相位上（65,000/520,000 = 12.5%、37.5%、62.5%、87.5%），便于验证相位窗索引（`ph_idx`）计算的正确性。四个位置间隔恰好 130,000 = 520,000/4，即相位相差 90°。
- **每个通道每个工频周期只产生 1 个脉冲** → 一份快照（恰好一个周期）里每通道恰好 4 个脉冲。

### 5.3 参数自检（第 83–88 行）

```
83     initial begin
84         if ((CLK_PER_SAMPLE != 1) || (CLK_PER_SAMPLE * SAMPLE_HZ != CLK_HZ))
85             $error("pd_dds_adc_source requires a 26 MHz DDS clock and 26 MSPS sample rate");
86         if ((SYNC_SAMPLES != 520000) || (SYNC_SAMPLES * SYNC_HZ != SAMPLE_HZ))
87             $error("pd_dds_adc_source requires a 50 Hz / 26 MSPS integer ratio");
88     end
```
- 用 `$error` 而不是 `$display`：仿真时 `$error` 会**直接报错并中止**，属于"硬失败"。
- 第 84 行要求 `CLK_PER_SAMPLE == 1`，即 DDS 自己的时钟必须就是采样率。第 86 行要求 `SYNC_SAMPLES == 520000`，即采样率/工频必须精确等于 520,000 —— **这个自检把"26 MSPS / 50 Hz"这个基准锁死了**，如果改了采样率忘了改别处，仿真会立刻报错。

### 5.4 主时序（第 90–122 行）

```
90     always @(posedge clk or negedge rst_n) begin
91         if (!rst_n) begin
92             tick_div    <= 3'd0;
93             cycle_sample <= 19'd0;
94             phase0      <= 8'd0;
95             phase1      <= 8'd53;
96             phase2      <= 8'd107;
97             phase3      <= 8'd179;
98             adc_data    <= {NUM_CH{12'd2048}};
99             adc_dv      <= {NUM_CH{1'b0}};
100            sync_in     <= 1'b0;
101        end else begin
```
复位值：
- `phase0..3` 的初相 **0 / 53 / 107 / 179** —— 四个通道三角波起点不同，避免四路波形完全同相（更接近真实四路前端的随机相位）。
- `adc_data <= {NUM_CH{12'd2048}}` —— 用**复制运算符**把 2048 复制 4 份，得到 48 bit 的 `{2048,2048,2048,2048}`。这是 Verilog 里写重复常量的标准写法。

```
102            adc_dv  <= {NUM_CH{1'b0}};
103            sync_in <= 1'b0;
104            if (sample_ce) begin
105                tick_div <= 3'd0;
106                adc_data <= {ch3_next, ch2_next, ch1_next, ch0_next};
107                adc_dv   <= {NUM_CH{1'b1}};
108                sync_in  <= (cycle_sample == 19'd0);
109
110                phase0 <= phase0 + 8'd3;
111                phase1 <= phase1 + 8'd5;
112                phase2 <= phase2 + 8'd7;
113                phase3 <= phase3 + 8'd11;
114                if (cycle_sample == SYNC_SAMPLES - 1)
115                    cycle_sample <= 19'd0;
116                else
117                    cycle_sample <= cycle_sample + 19'd1;
118            end else begin
119                tick_div <= tick_div + 3'd1;
120            end
121        end
122    end
```
逐段解释：

- **第 102–103 行：每拍先把 `adc_dv` 和 `sync_in` 清 0**（"default 赋值"模式）。这样只有第 107–108 行显式置 1 时才是 1 拍脉冲。**这是"单拍脉冲"的标准生成手法**：无条件清零 + 条件置位。
- **第 104 行 `if (sample_ce)`**：只在分频计数器满时产生一个样本。当前 `CLK_PER_SAMPLE = 1`，所以 `sample_ce` 恒为 1（`tick_div` 复位后为 0，`0 == 1-1` 成立），即每个时钟都产样本。
- **第 106 行 `adc_data <= {ch3_next, ch2_next, ch1_next, ch0_next}`** —— **这是全工程通道顺序的源头**：ch3 在最高位、ch0 在最低位，与 `pd_ddr_defines.vh:32` 的字段定义 (`[47:36] ch3 … [11:0] ch0`) 完全一致。注意 `ch0_next` 只取 `[11:0]`，因为 `saturate_u12` 的返回值已经是 12 bit。
- **第 108 行 `sync_in <= (cycle_sample == 19'd0)`**：在一个工频周期开始的第一拍（`cycle_sample == 0`）拉高 `sync_in`，持续**一个时钟周期**。这就是给 `pd_sync_pulse` 和 `pd_feature_core` 的"周期起始"信号。
  - ⚠️ **【观察】这里 `sync_in` 是一个 26 MHz 域的单拍脉冲，而 `pd_sync_pulse` 在 130 MHz 域用两级同步器采它。** 26 MHz 的脉冲宽度 = 38.46 ns，130 MHz 周期 = 7.69 ns，所以脉冲宽度是 5 个 130 MHz 周期，两级同步器**能**可靠采到（不会漏）。这是"窄脉冲跨时钟域用同步器"的可行前提：**源脉冲宽度必须大于目的时钟周期 + 亚稳态窗口**。这里恰好满足。
- **第 110–113 行相位步进：3 / 5 / 7 / 11**。相位是 8 bit（0…255），所以累加步长决定三角波的周期长度：
  - 三角波一个完整周期需要 256 个相位步；
  - ch0：256/3 ≈ 85.33 个样本/周期 → 26 MSPS / 85.33 ≈ **304,700 Hz**
  - ch1：256/5 = 51.2 样本 → 26M/51.2 ≈ **507,800 Hz**
  - ch2：256/7 ≈ 36.57 样本 → 26M/36.57 ≈ **710,900 Hz**
  - ch3：256/11 ≈ 23.27 样本 → 26M/23.27 ≈ **1,117,200 Hz**
  → **四通道三角波频率约 305 / 508 / 711 / 1117 kHz**。这是"验证滤波链通带（100 kHz–1 MHz）"的刻意设计：四个频率分布在通带内不同位置。
  - ⚠️ **【观察】因为 256 不能被 3、5、7、11 整除，三角波每个周期的相位起点会漂移**（不是严格的周期信号，存在最大 1 个相位步的抖动）。这在"验证数据通路"层面无影响，但如果要拿它当"精确单音"去做频谱标定，就会看到谱线展宽。这是 B09 里"不能直接用 DDS 做幅频标定"的原因之一。
- **第 114–117 行 `cycle_sample` 计数**：从 0 数到 `SYNC_SAMPLES - 1`（519,999）后归零，形成一个精确的 520,000 样本周期。`cycle_sample` 是 19 bit（520,000 < 2¹⁹ = 524,288，刚好够）。

### 5.5 `pd_dds_adc_source` 的可复用结论

> 写片上激励源时，做到这四件事会省下大量调试时间：
> ① **给出可预测的数值特征**（本例：四通道基线不同 → min/max 可区分；脉冲位置错开 → 相位可验证）；
> ② **参数自检用 `$error` 硬失败**，把关键假设锁死；
> ③ **声明验证边界**（能验什么、不能验什么）；
> ④ **脉冲/有效信号用"清零 + 条件置位"生成**，保证是干净的 1 拍脉冲。

---

## 6. 本篇的 10 条易错点

1. `async_fifo` 的存储体写块**不能含复位**、读必须**无条件同步读**、**不能 `initial`** —— 违反任一条，Vivado 会**静默**降级为 LUTRAM，只报 INFO。
2. 格雷码 FIFO 判满**必须最高两位都取反**。只反最高位 → `full` 永远为 0 → 静默丢数。
3. `async_fifo` 改成同步读后，`rd_en` 到数据有效是**第 2 拍**；调用者必须自己补 `rd_en_d` 打拍（`pd_adc_cdc.v:105`）。
4. `adc_capture` 用**同步复位**（为了不毛刺驱动 BRAM 控制引脚），`pd_sync_pulse` 用异步复位 —— 两者风格不同是有意的。
5. `pd_adc_cdc.v` 里端口名与注释仍写着 "100MHz / 20MHz"，实际是 130/26 MHz —— **以参数默认值与 BD 连接为准**。
6. `CDC_RATIO = CLK_HZ / SAMPLE_HZ` 是整数除法；只有当比值**精确整除**时节拍才严丝合缝。接真实异步 ADC 时需重新评估（FIFO 缓慢堆满/耗空）。
7. `ce_div` 判据用 `== 0`（而不是 `== CDC_TERM`），效果是**复位后立刻产生第一个有效拍**。
8. `pd_dds_adc_source` 的相位步进 3/5/7/11 不能整除 256 → 三角波**存在相位抖动**，不是严格单音，不适合做精确频谱标定。
9. `sync_in` 是 26 MHz 域的 1 拍脉冲（38.46 ns ≈ 5 个 130 MHz 周期），能被子同步器可靠采到；**若采样率提高，必须重算脉冲宽度**。
10. DDS 输出必须是**偏移二进制（0x800 = 零）**，不能是补码；四通道拼接顺序必须是 `{ch3,ch2,ch1,ch0}`。

---

**本卷下一篇**：`A03_滤波链_IIR带通.md` —— 从 26 MSPS 原始码流到可选的一节二阶 IIR 带通。
