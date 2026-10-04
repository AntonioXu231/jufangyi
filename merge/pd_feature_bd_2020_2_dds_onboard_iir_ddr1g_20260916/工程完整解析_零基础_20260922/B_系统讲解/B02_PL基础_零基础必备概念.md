# B02　PL 基础：零基础必备概念

> 本篇在整套文档中的位置与阅读前提
>
> - **位置**：本篇是「工程完整解析（零基础）」系列的第二篇（B02），属于**背景铺垫**文档。它不解释本工程“做什么业务”，只讲“硬件/Verilog 里那些你必须先懂的词”。后续 B03（数据流）、B04（各 IP 接口契约）、C 系列（源码逐文件讲解）都会反复引用本篇的概念。
> - **阅读前提**：你不需要会写 Verilog，也不需要懂 Zynq，只要①知道什么是“程序/代码”、②愿意把“硬件”想象成“一张可以重新连线的电路板”。如果你已经会 C 语言，会非常好懂——因为很多地方我们用“软件里怎么做”来对比“硬件里怎么做”。
> - **怎么读**：每一节都是同一个套路——先给**一句话定义**，再说**为什么需要它（不用会怎样）**，然后用**本工程真实代码（带文件名+行号）**举例，最后指出**常见误解**，并给一句“搞错会表现为……”。读不懂某一节可以直接跳，但第 4（CDC）、第 6（流水线）、第 9（BRAM）三节是读懂本工程源码的关键。
> - **引证约定**：所有“`文件:行号`”都来自本工程目录，未引用任何外部资料。无法从源码确认的内容写成“待验证”。

---

## 1　FPGA 是什么，和 MCU/CPU 的本质区别；组合逻辑与时序逻辑；时钟沿与寄存器

### 一句话定义
FPGA 是一块“内部连线可以被电改写”的芯片——你写进去的不是“指令”，而是“电路长什么样”；CPU/MCU 则是固定电路，靠“一条条指令顺序执行”来工作。

### 为什么需要它 / 不用会怎样
- **CPU 的模型**：硬件是固定的算术单元+控制器，软件是一条指令序列。例如“算 `a*b+c`”，CPU 先取 `a`、`b` 相乘，再取 `c` 相加——**同一时刻只做一件事**，靠时钟节拍一拍一拍走。
- **FPGA 的模型**：`a*b` 和 `+c` 可以同时是电路里两块并排的乘法器/加法器，**数据一到就同时算完**。所以 FPGA 擅长“海量数据同时流过”（本工程里 4 个 ADC 通道、每秒上亿次采样就是典型的“流处理”场景），而 CPU 擅长“复杂、不规律的判断逻辑”。
- 不用 FPGA、只用 CPU 行不行？行，但本工程 4×12bit×26MSPS 的原始码流（156 MB/s，见 `pd_ddr_defines.vh:11`）要在“每来一个样本就立刻做滤波/峰值提取/打包”，CPU 根本来不及——这正是要用 PL（可编程逻辑）的原因。

### 本工程代码举例
- **组合逻辑（combinational）**：输出只由“当前输入”决定，没有时钟参与。例如 `pd_feature_core.v:180`：
  ```verilog
  wire [ADC_W-1:0] sdata = adc_data - ZERO_CODE;   // 去直流中值, 转补码
  ```
  只要 `adc_data` 变，`sdata` 立刻变，像一根直连的导线+减法器。
- **时序逻辑（sequential）**：输出由“时钟边沿触发”才更新，中间有寄存器暂存。例如 `pd_feature_core.v:161-163` 的同步边沿检测：
  ```verilog
  always @(posedge clk or negedge rst_n)
      if (!rst_n) sync_sr <= 3'b000;
      else        sync_sr <= {sync_sr[1:0], sync_in};
  ```
  只有 `clk` 上升沿到来时，`sync_in` 才被“打”进移位寄存器 `sync_sr`。
- **时钟沿与寄存器**：`posedge clk` 就是“时钟的上升沿”（从 0 跳到 1 的瞬间）。寄存器（reg）就是那个“在上升沿才更新的暂存盒”。本工程所有关键状态（FSM、累加器）都在 `posedge clk` 上更新，例如 `pd_feature_core.v:230` 的主时序块。

### 常见误解
- 误解：“FPGA 也是跑程序的，只是更快。”——不对。FPGA 烧进去的是**电路结构**，不是程序；上电后它就像一块专用芯片，没有“取指/译码”。
- 误解：“`wire` 和 `reg` 的区别是‘线’和‘寄存器’。”——只对一半。在**可综合代码**里，`reg` 若写在 `always @(posedge clk)` 里才是寄存器；若写在 `always @(*)` 里，综合出来仍可能是组合逻辑（见第 2 节）。

> 如果这里搞错，会在本工程里表现为：把本该“每拍流水线推进”的 `pd_feature_core` 状态机（`S_PEAK→S_Q1→S_Q2→S_ACC`，见第 6 节）误当成“软件函数顺序调用”，会完全看不懂为什么同一个变量要分好几拍才出结果。

---

## 2　Verilog 语言基础

### 一句话定义
Verilog 是“用来描述硬件电路”的语言：你写的不是“执行步骤”，而是“有哪些模块、哪些线、哪些在时钟边沿才变”。

下面按本工程真实用法逐条讲。

#### 2.1 module / 端口
**定义**：`module` 是一个硬件“积木”（或“芯片”），端口就是它的管脚。
**本工程**：`pd_feature_core.v:25-100` 定义了一个模块，端口分三类——`input`（输入脚）、`output`（输出脚）、`inout`（双向，本工程少见）。例如：
```verilog
module pd_feature_core #(
    parameter integer CH_ID = 0, ... )(
    input  wire                 clk,
    input  wire [ADC_W-1:0]     adc_data,
    output reg  [EV_W-1:0]      ev_tdata,
    ...
);
```
**常见误解**：把 `module` 当成“函数”。函数有返回值、会被调用；`module` 是“永远在那里的电路”，用 `u_xxx` 例化（实例化）后一直工作。

#### 2.2 parameter / localparam
**定义**：`parameter` 是“对外可配置的参数”（例化时可改）；`localparam` 是“模块内部常量”（外部改不了）。
**本工程**：
- `parameter integer CH_ID = 0`（`pd_feature_core.v:26`）——4 个通道各例化一份，靠 `CH_ID` 区分。
- `localparam S_IDLE = 4'd0, S_PEAK = 4'd1, ...`（状态编码，`pd_feature_core.v:131-133`）。综合器会把它们直接变成常数，不占逻辑。
- `localparam signed [ADC_W-1:0] SMAX = {1'b0, {(ADC_W-1){1'b1}}};`（`pd_feature_core.v:105`，值 2047）——用拼接运算符构造常数。

#### 2.3 wire 与 reg 的区别
**定义**：
- `wire`：连线，像电路板上的铜线，靠 `assign` 或模块端口驱动，**自己不能存值**。
- `reg`：在 `always` 块里被赋值，综合后**可能**是寄存器（时序）也可能只是组合结果（取决于敏感表）。
**本工程**：
- `wire sdata = adc_data - ZERO_CODE;`（组合，`pd_feature_core.v:180`）。
- `reg [31:0] m_cnt;`（`pd_feature_core.v:170`）写在 `posedge clk` 块里，是真正的寄存器（计数器）。
**常见误解**：“`reg` 一定是寄存器、会存一拍。”错误。写在 `always @(*)` 里的 `reg` 是组合逻辑，例如 `pd_feature_core.v:363-371` 的 `q0_code` 在一个 `always @*` 块里被赋值，它**不**存拍，只是组合多路选择的结果。

#### 2.4 `always @(*)` 组合块 与 `always @(posedge clk)` 时序块
**定义**：
- `always @(*)`：敏感表是“*”（任何输入变就重算），描述**组合逻辑**。
- `always @(posedge clk)`：只在时钟上升沿触发，描述**时序逻辑（寄存器）**。
**本工程**：
- 组合：`always @* begin case (cfg_mode) ... endcase end`（`pd_feature_core.v:365-371`，根据模式选 `q0_code`）。
- 时序：`always @(posedge clk or negedge rst_n) begin ... end`（`pd_feature_core.v:230`）主状态机。
**常见误解**：“在 `always` 里赋值用 `=` 还是 `<=` 随便。”——后果很严重，见 2.5。

#### 2.5 阻塞 `=` 与非阻塞 `<=` 的区别与误用后果
**定义**：
- **阻塞 `=`**：语句按顺序“立刻求值并赋值”，像软件变量。只在**组合块 `always @(*)`** 里用。
- **非阻塞 `<=`**：所有右侧先“同时取样”，到块结束的“时钟边沿”才一起生效。只在**时序块 `always @(posedge clk)`** 里用。
**本工程正确用法**：时序块全用 `<=`，例如 `pd_feature_core.v:657-669` 的 `S_PEAK` 拍把一堆中间量同时锁存：
```verilog
q0_sgn_r <= q0_sgn;   u_val_r <= u_val[17:0];   ...   fsm <= S_Q1;
```
**误用后果（本工程真实踩坑）**：`pd_feature_core.v:433-436` 注释专门说明——`q0_pc_r` 和 `abs_q0_r` “必须由**同一组合值** `q0_pc_n` 同步载入，否则非阻塞赋值会让两者错开一拍”。如果错误地把本该“同时更新”的两个寄存器写成依赖关系，或把 `<=` 错用在组合块里，会出现“差一拍/读到旧值”的错位 bug。
**常见误解**：“组合块里也可以用 `<=`。”——可以写但几乎总是错的：`<=` 在组合块里不会在“边沿”生效，语义混乱，仿真和综合可能不一致。

#### 2.6 `generate for`（参数化批量例化）
**定义**：在编译期按参数展开多份相同电路，相当于“硬件里的 for 循环”。
**本工程**：`pd_filter_chain.v:104-116` 用 `generate for(ch=0; ch<NUM_CH; ch=ch+1)` 把 4 通道的 IIR 滤波链（`pd_iir_biquad`）一次性铺开：
```verilog
generate for(ch=0; ch<NUM_CH; ch=ch+1) begin: GCH
    ... pd_iir_biquad #(...) u_iir(...);
end endgenerate
```
**常见误解**：“`generate` 是运行时循环。”——不对，它在**综合前**就展开成固定电路，生成的是 4 份并排的硬件。

#### 2.7 function / task
**定义**：
- `function`：纯组合、无时序、无 `@` 块，像“组合计算器”，用于算一个值（**不能**包含时序或驱动多个 reg）。
- `task`：可以包含时序、可以驱动多个 reg，像“一段可复用的操作序列”。
**本工程**：
- `function signed [15:0] sat16;`（`pd_feature_core.v:135-142`）——把 32 位数饱和截断到 16 位（见第 12 节）。被多处调用：`sat16(q0_mul >>> 8)`（`:392`）。
- `function [ADC_W:0] abs13;`（`pd_feature_core.v:114-119`）——13 位绝对值。
- `task close_window;`（`pd_feature_core.v:213-228`）——窗口收尾时锁存结果（在 `always` 块里被调用，如 `:270` `:294`）。
**常见误解**：“`function` 里可以写 `posedge clk` 或 `<=`。”——不可以，`function` 必须是纯组合，否则综合报错。

#### 2.8 `include 与 `` `define `` 宏
**定义**：
- `` `define ``：全局文本宏（编译期字符串替换），类似 C 的 `#define`。
- `` `include ``：把另一个文件原样插入，用来共享参数/宏。
**本工程**：
- `` `define PD_ADC_W 12 ``（`pd_defines.vh:9`），全工程用 `` `PD_ADC_W `` 引用 ADC 位宽。
- `pd_feature_core.v:23` 顶部 `` `include "pd_defines.vh" ``，于是模块参数能写 `parameter integer ADC_W = \`PD_ADC_W`（`:27`）。
**常见误解**：“改了 `.vh` 宏，不用重新综合。”——错。宏在**编译期**展开，改了必须重新综合才生效。

> 如果这里搞错，会在本工程里表现为：把 `pd_filter_chain.v` 的 `generate` 当软件循环去“单步调试”，或把 `sat16` function 当成会“存值”的模块，导致看不懂 12 档计数（`pd_feature_core.v:772-816`）是纯组合比较、不占用流水拍。

---

## 3　敏感表与复位风格：同步复位 vs 异步复位；为什么 BRAM 写块不能含复位

### 一句话定义
**复位**是“上电/出错时把电路清零”的机制；**异步复位**是“复位一生效立刻清零（不管时钟）”，**同步复位**是“等下一个时钟上升沿才清零”。本工程有一个硬性约束：**存储体（BRAM）的写过程块里不能出现复位**。

### 为什么需要它 / 不用会怎样
复位保证上电后状态机、计数器从已知值开始。但 7 系列 FPGA 的**块 RAM（BRAM）硬件不支持“异步复位释放时重写存储内容”**——如果写块敏感表里挂了 `rst_n`，综合器会认定这是“不支持的 RAM 风格”，然后**悄悄把整个存储体拆成几千个触发器（FF）**，既浪费资源又可能时序失败。

### 本工程代码举例
- **async_fifo.v 头部注释（`:8-21`）**明确写了旧版三个致命写法，其中第 3 条：
  > `3) 写块敏感表含 wr_rst_n -> "RAM is sensitive to async reset" ... 然后 "RAM dissolved into registers"`
- **pd_axis_fifo.v 头部（`:9-12`）**同样强调：
  > `1) 存储体的写过程块**敏感表里不能出现 rst_n**。否则报 "RAM is sensitive to asynchronous reset signal. this RTL style is not supported." 然后 "RAM dissolved into registers"。`
- **正确写法（无复位的写块）**：`async_fifo.v:97-100`
  ```verilog
  always @(posedge wr_clk) begin
      if (wr_act) mem[wr_ptr_bin[ADDR_W-1:0]] <= wr_data;
  end
  ```
  注意：这个块**没有 `or negedge rst_n`**，复位只留在**独立的指针块**里（`:110-113`、`pd_axis_fifo.v:106-129` 的 `always @(posedge clk or negedge rst_n)`）。
- **为什么连同步复位也不行**：BRAM 的写端口物理上只有地址/数据/使能，没有“写时顺带清存储”的硬件。哪怕同步复位，只要写在同一个 `always` 块的敏感表/条件里，综合仍可能判成非标准模板。`adc_capture.v:17-25` 注释点出：连 FIFO 写使能这类 **RAMB 控制引脚都要用同步复位**，避免“异步复位释放瞬间毛刺损坏 BRAM 内容”——可见复位与 BRAM 的耦合很敏感。

### 常见误解
- 误解：“复位写在独立块里，存储块就一定安全。”——还要满足“写块敏感表无复位 + 同步读 + 无条件读 + 无 `initial` + `ram_style`”，见第 9 节。
- 误解：“异步复位更快更可靠。”——对寄存器 yes；对 BRAM 写块，它是**致命**的（会触发静默降级）。

> 如果这里搞错，会在本工程里表现为：`async_fifo` / `pd_axis_fifo` 综合后 BRAM 数 = 0，反而是几千个 FF，且 `pd_axis_fifo` 头部实测（`:32-34`）从“0 BRAM + 16764 FF”退化——这正是旧版踩过的坑。

---

## 4　时钟域与跨时钟域（CDC）

### 一句话定义
**时钟域**＝“由同一个时钟驱动的一群寄存器”。**CDC（跨时钟域）**＝数据要从一个时钟域送到另一个**频率/相位没关系**的时钟域；直接连会出错，必须加“同步器”或“异步 FIFO”。

### 为什么需要它 / 不用会怎样
两个时钟没有固定相位关系，发送方的“数据变化时刻”和接收方的“采样时刻”可能只差一瞬——接收端寄存器可能采到“正在跳变中的中间电平”，进入**亚稳态**（见第 5 节），导致整条链路数据错乱、时好时坏（最难调的 bug 之一）。

本工程里至少有 3 处 CDC：
1. ADC 采样时钟（26 MHz，`adc_clk_0`）→ 系统 130 MHz：`pd_adc_cdc.v` 用 `async_fifo` 做跨域。
2. 工频同步输入 `sync_in`（板级方波，异步）：`pd_sync_pulse.v` 用两级同步器。
3. 外部 ADC 时钟与 `clk_wiz_0` 输出的 130 MHz 在 BD 边界被 XDC 显式声明为异步（`pd_feature_bd_timing.xdc:14-16`）。

### 本工程代码举例
#### 两级同步器（单比特信号）
`pd_sync_pulse.v:17-31`：把异步的 `sync_in` 用两级寄存器打拍，再取“上升沿”：
```verilog
(* ASYNC_REG = "TRUE" *) reg [1:0] sync_ff;
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) {sync_ff, sync_d} <= 0;
    else begin sync_ff <= {sync_ff[0], sync_in}; sync_d <= sync_ff[1]; end
end
assign cycle_start = sync_ff[1] & ~sync_d;
```
两级打拍把“亚稳态概率”压到极低（第一级可能 metastable，第二级基本稳定）。

#### 异步 FIFO + 格雷码指针
`async_fifo.v` 是 CDC 主力：
- 二进制指针转格雷码：`wr_ptr_gray = wr_ptr_bin ^ (wr_ptr_bin >> 1);`（`async_fifo.v:52-53`）。
- 格雷码跨域用双同步器（`async_fifo.v:55-70`），并打 `ASYNC_REG` 属性（`:57-58`）。
- **为什么“判满要最高两位都取反”**：见 `async_fifo.v:78-92` 的推导。要点——`full` 表示“写指针领先读指针整整一圈（2^N）”。在二进制下是“最高位相反、其余相同”；转成格雷码后，因为 `g = b ^ (b>>1)`，最高位的取反会“传染”到次高位，所以**必须最高两位都取反**：
  ```verilog
  wire [PTR_W-1:0] full_cmp_gray = {~rd_ptr_gray_in_wr[PTR_W-1],
                                    ~rd_ptr_gray_in_wr[PTR_W-2],
                                     rd_ptr_gray_in_wr[PTR_W-3:0]};
  assign full = (wr_ptr_gray == full_cmp_gray);   // async_fifo.v:89-92
  ```
  注释还做了验证：“只反转最高位时 full 永远不会拉高（遍历 4096 种指针组合：错误判据命中 0/4096，正确判据命中 4096/4096），FIFO 会静默溢出。”（`:86-87`）

#### ASYNC_REG 属性的作用
`async_fifo.v:56` 注释：“`ASYNC_REG`: 让两级同步器紧邻布局，消除 TIMING-10 告警并压低亚稳态概率。”即在 `.v` 里用 `(* ASYNC_REG = "TRUE" *)` 标记同步链（`async_fifo.v:57-58, 65-66`、`pd_sync_pulse.v:17`），告诉综合器“这两级要摆在一起、走线最短”。

### 常见误解
- 误解：“多打几拍同步器就能跨任意数据总线。”——单比特可以；**多比特总线**（如 12/24/48 位数据）不能用多级同步器（各位到达时间不同会错帧），必须用**异步 FIFO + 格雷码指针**。
- 误解：“格雷码指针能直接比较满/空。”——不能直接比二进制意义，必须按第 78-92 行的“满=最高两位取反”规则。

> 如果这里搞错，会在本工程里表现为：若 `pd_adc_cdc` 的异步 FIFO 判满写错，ADC 数据会**静默覆盖丢失**且仿真看不出（注释 `:86-87` 说命中 0/4096），表现为“偶尔丢样本、PRPD 图谱缺角”。

---

## 5　亚稳态、建立/保持时间、时序收敛基本概念（WNS/TNS/WHS、critical path、布线延迟）

### 一句话定义
- **建立时间（setup）/保持时间（hold）**：寄存器要求“数据在时钟沿**之前**多久就稳定（setup）”且“在时钟沿**之后**多久不能变（hold）”，否则输出不可靠（亚稳态）。
- **亚稳态**：寄存器输入不满足建立/保持，输出在 0 和 1 之间“悬停”一段未知时间。
- **时序收敛**：综合+实现后，所有路径都满足建立/保持，工具报“通过”。
- **WNS/TNS/WHS**： Worst/Total Negative Slack（建立负裕量最坏值/总和）、Worst Hold Slack（保持最坏裕量）。**负数=没收敛**（会出错）；越接近 0 越危险。
- **critical path（关键路径）**：整片设计里“最紧张、决定最高频率”的那条组合链。
- **布线延迟占比**：一条路径的延迟 = 逻辑延迟 + 连线延迟；130 MHz 下连线（routing）常常占大头。

### 为什么需要它 / 不用会怎样
频率越高，时钟周期越短（130 MHz → 周期约 7.69 ns）。一条组合链若“逻辑层数太多”，信号在一个周期内走不完，就建立违例（WNS 变负），芯片实际跑起来会算错数。本工程目标 130 MHz，所以“长组合链”是头号敌人。

### 本工程代码举例（全部来自源码注释里给出的真实时序数据）
- **长组合链导致失败**：`pd_feature_core.v:109-113` 注释原实现把 `absp_max/min` 与 `q0_code/q0_sgn` 的 case 多路选择串成“12 级、含 4×CARRY4 的长链，130 MHz 下成为 pd_feature_core 的头号失败路径”。
- **Σq·u 累加预检 WNS = -1.056 ns**：`pd_feature_core.v:533-539` 注释“原实现单拍串行 [q×u 乘法 + 64bit 累加]（130M 预检 WNS -1.056ns，关键路径全落在 acc_sum_qu 链）”，于是拆成两拍。
- **另一处最差路径 -0.707 ns、布线占 70%**：`pd_feature_core.v:454` 注释“成为 BD 级实现中 pd_feature_core 的最差路径（-0.707ns，布线占 70%）”。
- **次紧路径 +0.275 ns**：`pd_feature_core.v:441` 注释“实测 130M WNS 仅 ~0.275ns”。
- **17 级 CARRY4 离群项 +0.111 ns**：`pd_feature_core.v:518-524` 把 `(acc_cycles+1) >= K` 这种“32bit 加法+比较”重写成等效的“16bit 比较+16 输入或”，CARRY4 从 17 降到约 4。
- **吞吐余量**：`pd_pack192.v:14-17` 说明 130 MHz 域 “2 clk/样本”，而块打包只需 5 clk/块，余量充足——背后就是“组合链不能长”的约束。

### 常见误解
- 误解：“仿真是绿的就代表时序没问题。”——仿真只验证逻辑，不验证物理延迟；时序要靠**实现后的时序报告**（WNS/TNS）判定。
- 误解：“加流水线（第 6 节）能解决一切。”——能降关键路径，但会增加延迟、复杂度，且不能改善 hold（保持）违例。

> 如果这里搞错，会在本工程里表现为：把 `pd_feature_core` 里任何“为 130 MHz 提频而拆拍”的改动（如 `:109-113`、`:533-539`）改回单拍组合，综合实现后 WNS 变负，板子上表现为**偶发**的特征值算错、难以复现。

---

## 6　流水线（pipelining）

### 一句话定义
流水线＝把一条“太长的组合路径”切成几段，每段之间插寄存器，让数据**一拍一拍**往前流——用“延迟增加、控制变复杂”换“单拍路径变短、频率能拉高”。

### 为什么需要它 / 不用会怎样
第 5 节说了：130 MHz 下一条组合链不能太长。如果不切，要么降频（吞吐掉），要么时序不收敛（算错）。切开后，每一段逻辑变少，频率就能上去；代价是**结果晚几拍才出来**，且状态机要记得“现在流到第几段”。

### 本工程代码举例
#### 例 1：`pd_feature_core` 的 S_PEAK→S_Q1→S_Q2→S_ACC
`pd_feature_core.v:127-133` 把原来“一拍算完”的量化链拆成多级流水，注释写：
> “`h_max -> 模式选择 -> q0_sgn -> DSP 乘 -> sat16 -> 累加` 这条 34 级组合路径拆成每拍 <=10 级，使 100MHz 时序可收敛。”
实际状态机（`:655-694`）：
- `S_PEAK` 拍（`:655-670`）：采样中间量 `q0_sgn_r <= q0_sgn; u_val_r <= u_val[17:0]; ...; fsm <= S_Q1;`
- `S_Q1` 拍（`:673-678`）：乘法 `q0_mul_r <= q0_sgn_r * $signed({1'b0, cfg_scale}); fsm <= S_Q2;`
- `S_Q2` 拍（`:681-690`）：量化 `q0_pc_r <= q0_pc_n;`（sat16 + 绝对值）
- `S_ACC` 拍（`:694-721`）：发事件 + 累加（原 S_PEAK 的动作）
注释（`:395-403`）明确：这些寄存器只在 FSM 推进时更新，值全部源于 S_PEAK 时刻已稳定的输入，“仅延迟 2 拍”。

#### 例 2：`pd_iir_biquad` 的 phase 0..4
`pd_iir_biquad.v:2-4` 头注释：“Five-clock, second-order IIR section... PH is deliberately asserted as 5”。内部用一个 3 位 `phase` 计数器（`:20`）在 `posedge clk` 上推进 `0→1→2→3→4→0`（`:55-60`），把“3 次乘加 + 1 次输出”分到 5 拍完成（`:67-74`）。这样单个二阶节只占一份 DSP48 资源、频率能到 130 MHz（`pd_iir_biquad.v:6-9` 参数 `PH=5`）。

#### 代价（本工程里的体现）
- **延迟增加**：S_PEAK→S_ACC 多了 2~3 拍（`:395-403` 注释“仅延迟 2 拍”）；IIR 输出晚 5 拍（`dout_dv <= (phase == 3'd4)`，`pd_iir_biquad.v:74`）。
- **控制复杂度**：必须维护 FSM/phase，且“换档/快照”要等边界（见第 7 节 APPLY 在 `S_CYC` 才生效，`pd_feature_core.v:875-879`）。

### 常见误解
- 误解：“流水线是‘并行加速’，所以延迟变短。”——相反，**单条数据的延迟变长**，只是“吞吐率（每秒处理条数）”变高。
- 误解：“插寄存器就一定更快。”——寄存器本身也有建立/保持要求；乱插可能只是把关键路径挪到别处。

> 如果这里搞错，会在本工程里表现为：把 `pd_feature_core` 的 `q0_pc_r`/`abs_q0_r` 误以为“同一拍算出”（注释 `:433-436` 强调必须由同一组合值 `q0_pc_n` 同步载入），否则事件包相位/电荷会和统计值错开一拍，PS 侧解析出“乱序事件”。

---

## 7　AXI4-Lite：它是什么、和“寄存器读写”的关系；AW/W/B/AR/R 五通道；WSTRB 字节选通；读回一致 ≠ 配置生效

### 一句话定义
AXI4-Lite 是 ARM/Zynq 世界里 PS（CPU）读写 PL 内部“控制寄存器”的**标准总线协议**。你可以把它理解成：“CPU 想往 PL 的某个地址写一个数 / 读一个数”的规矩。它本质就是**寄存器读写**，只是被标准化成 5 条独立通道。

### 为什么需要它 / 不用会怎样
没有标准总线，PS 每接一个 PL 模块都要自己画一套“地址→数据”线。AXI4-Lite 让所有 IP 都用同一套握手，Vivado 的 AXI 互联（`axi_ic_ctrl`、`axi_smc_hp0/1`）能自动把 PS 的地址请求路由到对应 IP。本工程 `pd_filter_chain`（滤波系数）、`pd_feature_0`（特征提取配置）都挂在这条总线上。

### 本工程代码举例
#### 五通道（写：AW/W/B；读：AR/R）
`pd_filter_chain.v:17-33` 列出了全部端口：
- **AW**（Address Write）：`S_AXI_AWADDR` + `S_AXI_AWVALID/AWREADY`（`:17-19`）——“我要写到哪个地址”。
- **W**（Write data）：`S_AXI_WDATA` + `S_AXI_WSTRB` + `WVALID/WREADY`（`:20-23`）——“要写的数据（和字节选通）”。
- **B**（Write response）：`S_AXI_BRESP` + `BVALID/BREADY`（`:24-26`）——“写完了，回个应答”。
- **AR**（Address Read）/ **R**（Read data）：`S_AXI_ARADDR` + `RVALID/RREADY/RDATA`（`:27-33`）——“读哪个地址 / 读出的数据”。
注意：**写分 AW 和 W 两条独立通道**——地址可以先到、数据后到，IP 内部用 `aw_hold`/`w_hold` 缓冲（`:37, 42-52, 76-84`）。

#### WSTRB 字节选通（用 merge32 / mergecoef 举例）
`WSTRB[3:0]` 表示 32 位 `WDATA` 的 4 个字节里“哪些字节真正要写”。`pd_filter_chain.v:57-66` 给出 `merge32` / `mergecoef`：
```verilog
function [31:0] merge32;
    input [31:0] oldv, newv; input [3:0] strb;
    begin merge32=oldv;
      if(strb[0])merge32[7:0]  =newv[7:0];   // 字节0
      if(strb[1])merge32[15:8] =newv[15:8];  // 字节1
      if(strb[2])merge32[23:16]=newv[23:16]; // 字节2
      if(strb[3])merge32[31:24]=newv[31:24]; // 字节3
    end
endfunction
```
用法（`:90-94`）：写滤波系数时按 `cfg_strb` 选择性覆盖 `sh_coef[...]` 的某些字节，`mergecoef` 把 18 位系数对齐进 32 位并套用 `merge32`。

#### 为什么“读回一致”不等于“配置生效”（影子寄存器 / APPLY）
`pd_filter_chain.v:67-95` 设计了**两级系数**：
- `sh_coef[]`（shadow，影子）——CPU 写进来先存这里；
- `ac_coef[]`（active，生效）——真正驱动 IIR 滤波器的系数。
只有发出 `apply` 时，才把影子搬去生效（`:89-95`）：
```verilog
if(cfg_valid && wa<12'h010) case(wa[3:2])
    0: if(cfg_strb[0]) begin fctrl_bypass<=...; if(cfg_data[1]) begin apply_p<=1; coef_dirty<=0; end ... end
endcase
...
if(apply_p) begin for(i=0;i<N_CELL;i=i+1) ac_en[i]<=sh_en[i];
                for(i=0;i<N_COEF;i=i+1) ac_coef[i]<=sh_coef[i]; end
```
**含义**：CPU 写 `sh_coef` 后**立刻读回**能读到刚写的值（读回一致），但滤波器**还在用旧的 `ac_coef`**，直到你写“APPLY”位（`cfg_data[1]`）才一次性换档。这样保证“一帧之内系数恒定”（`pd_feature_core.v:420-424` 注释同款思路：门槛在周期边界 `S_CYC` 才换，避免帧内统计不自洽）。
本工程 `pd_feature_core` 也有同样机制：`cfg_apply` 只置 `apply_pending`（`:632`），真正换档在 `S_CYC` 拍（`:875-879`），端口 `st_apply_pending` 暴露“是否仍有未生效配置”（`:99, 427`）。另外 `FRAME_STAT` 是只读、需 `FRAME_ACK` 显式清（`:629-630`）——读它并不能清除溢出标志。

### 常见误解
- 误解：“CPU 写进去，硬件下一拍就用上了。”——对有 shadow/apply 的寄存器，**不是**，要等 APPLY 且在合法边界才生效。
- 误解：“WSTRB 是‘地址选通’。”——它是**字节**选通，决定 32 位里哪些字节落地。

> 如果这里搞错，会在本工程里表现为：PS 写完新滤波系数后立刻以为生效，结果实测滤波特性仍是旧系数（因为没写 APPLY 位）——这是 `pd_filter_chain.v:89-95` 明确防的“读回一致≠配置生效”陷阱。

---

## 8　AXI4-Stream：valid/ready 握手；TLAST/TKEEP；背压逐级回传

### 一句话定义
AXI4-Stream 是“**单向、不停流的数据流**”协议，专门搬样本/事件这类连续数据。核心只有两条握手线：**valid**（我这边数据有效了）和 **ready**（你那边能收吗）。一拍数据只有在 `valid && ready` 同时为 1 时才真正传输。

### 为什么需要它 / 不用会怎样
PL 内部大量是“流”（ADC 样本流、特征事件流、去 DDR 的码流）。用 Stream 协议，上下游天然能“背压”（下游忙就拉低 ready，上游自动停），**不会丢数**。本工程 4 通道原始数据→DDR（`pd_pack192`→AXI DataMover）、特征事件→DMA，全是 Stream。

### 本工程代码举例
#### valid/ready 基本规则：谁不能等谁
铁律：**valid 不能等 ready**（不能因为对端没说 ready 就把 valid 一直 HIGH 占着），但 **ready 可以等 valid**；数据只在 `valid&ready` 那拍走。
- `pd_pack192.v:56-57`：
  ```verilog
  wire in_acc  = i_s48_valid & o_s48_ready;   // 上游→本模块：收下
  wire out_acc = m_axis_tvalid & m_axis_tready; // 本模块→下游：发出
  ```
- **o_s48_ready 与 m_axis_tready 的背压回传**：`pd_pack192.v:33` 输出 `o_s48_ready` 给上游，`pd_pack192.v:38` 输入 `m_axis_tready` 来自下游。注释（`:19-20`）：
  > “`m_axis_tready` 拉低时停止发 beat，`wcnt` 饱和在 4 并使 `s48_ready=0`，反压逐级传到上游异步 FIFO，绝不丢样本。”
  即：`o_s48_ready = (wcnt != 4)`（`:61`）——收满 4 个字就暂停收上游；下游 `m_axis_tready=0` 时 `m_axis_tvalid` 保持但不前进（`:64-67`），上游自然被反压停住。
- 同款握手在 `pd_axis_fifo.v:82-87`：`s_tready = (used != DEPTH)`，`wr_en = s_tvalid && s_tready`，把背压从 DMA/仲裁器一路回头传到生产者。

#### TLAST / TKEEP
- **TLAST**：一帧/一个包的最后一个 beat 拉高。本工程 `pd_pack192.v:69` 注释：“`m_axis_tlast = 1'b0`；由 DataMover 的 BTT 决定长度，不依赖 tlast”——即打包模块不自己发 tlast，长度由 DMA 命令（BTT）定。相反，`pd_feature_core.v:844` 周期统计包 `ev_tlast <= 1'b1`（帧尾），`pd_axis_fifo.v:54, 89-90` 也把 tlast 和数据一起打包进 BRAM。
- **TKEEP**：字节有效掩码（类似 WSTRB 但用于流）。`pd_pack192.v:39, 68` `m_axis_tkeep = {AXI_DW/8{1'b1}}`——恒全有效，因为 192 bit 100% 利用（`pd_ddr_defines.vh` 说明 192bit 块无填充）。

### 常见误解
- 误解：“ready 为 0 时 valid 必须变 0。”——错。valid 可以保持（表示“我准备好了，你啥时 ready 啥时取”），这正是背压的工作方式；只是这拍不传输。
- 误解：“没收到 ready 数据就丢了。”——错，只要 valid 保持且 ready=0，数据**留住**，等 ready 再来。

> 如果这里搞错，会在本工程里表现为：把 `pd_pack192` 的 `o_s48_ready`（`pd_pack192.v:61`）理解成“永远为高”，下游 DMA 偶发反压时 192bit 块会被覆盖、原始样本**静默丢失**，违背契约“原始无损”（`pd_pack192.v:19-20`）。

---

## 9　存储资源：分布式 RAM（LUTRAM）vs 块 RAM（BRAM）；静默降级；本工程如何保证落到 BRAM

### 一句话定义
FPGA 里有两类片上存储：**分布式 RAM（LUTRAM）**＝用查找表（LUT）凑出来的小块存储；**块 RAM（BRAM）**＝芯片里专用的存储块（本器件 xc7z020 每片 36 Kb/块，称 RAMB36）。BRAM 更大更快、不占逻辑资源；LUTRAM 灵活但占 LUT、容量小。

### 为什么需要它 / 不用会那样
本工程 FIFO（`async_fifo`、`pd_axis_fifo`）和 PRPD 图谱都要较大存储。若被综合器“静默降级”成 LUT 或触发器，会**爆逻辑资源、时序变差**，而且**没有任何 warning**（只有 INFO 级提示），极难发现。

### 本工程代码举例：综合器“静默降级”与修复
- **旧版静默降级的证据**：`async_fifo.v:8-16` 头部注释列了三个致命写法，旧版把 12bit×64 存储体打散成 768 个 FF/通道（4 通道约 3072 FF）；`pd_axis_fifo.v:32-34` 给出实测对比：
  ```
  原始(组合读+异步复位): 4852 LUT + 16764 FF + 2210 MUXF7 + 1105 MUXF8 + 0 BRAM
  最终(同步读+地址回指):   36 LUT +    27 FF +    0 MUXF7 +    0 MUXF8 + 1 RAMB36
  ```
- **本工程为了让存储体落到 BRAM 做了什么**（六条规则，`pd_axis_fifo.v:5-37`、`async_fifo.v:14-16`）：
  1. **写块无复位**：敏感表不含 `rst_n`（`async_fifo.v:97-100`；`pd_axis_fifo.v:99-103` 的写块只有 `if (wr_en) mem[wp] <= ...`）。
  2. **同步读**：`rd_data_reg <= mem[...]` 在 `posedge rd_clk` 里（`async_fifo.v:102-107`）；`pd_axis_fifo.v:100-103` `dout_raw <= mem[rd_addr]`。
  3. **无条件读**：读永远执行，不能 `if (fetch) dout <= mem[rp]` 门控（`pd_axis_fifo.v:18-21`）。
  4. **无 `initial` 整块初始化**：旧版 `initial + for` 初始化被判定“非 RAM 模板”（`async_fifo.v:10-11`）。
  5. **`ram_style` 属性**：`(* ram_style = "block" *) reg [...] mem [...]`（`async_fifo.v:44`；`pd_axis_fifo.v:72`）。
  6. **一写一读（simple-dual-port）**：`pd_axis_fifo.v:29-30` 说明“一个物理 BRAM 端口只有一组地址线，读写地址不同→走 simple-dual-port，是 BRAM 最容易识别的结构”。
- 另有 `(* rom_style = "block" *) reg [15:0] sin_lut [0:1023];`（`pd_feature_core.v:147`）把正弦 ROM 也钉在 BRAM。
- 同步读的代价：`rd_data` 比旧版晚 1 拍（`async_fifo.v:18-20`），调用方 `pd_adc_cdc` 用 `rd_en_d` 打拍对齐（`:104-109`）。

### 常见误解
- 误解：“写了 `ram_style="block"` 就一定能用 BRAM。”——不够，还必须满足上面 1~4 条模板规则，否则综合器仍会降级（且常无 warning）。
- 误解：“同步读‘慢一拍’不划算。”——BRAM 硬件本身只有同步输出，组合读反而综合不出 BRAM（见 `pd_axis_fifo.v:11-12`）。

> 如果这里搞错，会在本工程里表现为：`async_fifo`/`pd_axis_fifo` 综合后 `BRAM` 数为 0、`FF` 数千（对照 `pd_axis_fifo.v:32-34`），资源爆满且 `pd_feature_core` 关键路径变差，但日志里只有 INFO 没有 ERROR，容易被漏掉。

---

## 10　Vivado 工程对象：Block Design / IP 核 / XCI / XDC / 综合 / 实现 / 比特流 / .xsa / ILA / 时序报告

### 一句话定义
Vivado 把“FPGA 工程”拆成若干对象：Block Design（图形化拼 IP）、IP 核（现成功能模块）、XDC（时序/管脚约束）、综合/实现（把 RTL 转成真实电路并布线）、比特流（烧录文件）、`.xsa`（导出给 PS 用的硬件描述）、ILA（在线逻辑分析仪）、时序报告（收敛判定）。

### 为什么需要它 / 不用会怎样
不懂这些对象，你就看不懂“改哪里、点哪个按钮、出哪个文件”。本工程所有名词都能在目录里找到真实对应。

### 本工程真实文件名举例（均来自工程目录）
- **Block Design**：`pd_feature_bd_2020_2.srcs/sources_1/bd/pd_feature_bd/pd_feature_bd.bd`（JSON 文本，组件树见 `:tree`，含 `processing_system7_0`、`clk_wiz_0`、`pd_filter_0`、`pd_feature_0`、`pd_ddr_0`、`pd_dds_0`、`sync_pulse_0`、`axi_dma_0`、`ila_0`、`axi_smc_hp0/1`、`axi_ic_ctrl`、`xlconcat_0`、`rst_ps7_0`、`axi_rs_feature_ctrl_0`、`ila_clk_reset_diag_0`）。
- **IP 核 / XCI**：`clk_wiz_0`（时钟）、`axi_dma_0`（PS↔PL DMA）、`pd_filter_0`/`pd_feature_0`/`pd_ddr_0`/`pd_dds_0`（本工程自研 IP，由 RTL 封装）。IP 的配置以 `.xci` 形式存在于 `.srcs/ip` 下（IP 用户文件 `pd_feature_bd_2020_2.ip_user_files`）。
- **XDC 约束**：`pd_feature_bd_2020_2.srcs/constrs_1/imports/constraints/pd_feature_bd_timing.xdc`（本篇多处引用：时钟周期、异步时钟组）。
- **综合（Synthesis）/ 实现（Implementation）/ 比特流（Bitstream）**：产物在 `pd_feature_bd_2020_2.runs/`（综合/实现各自目录）、`.bit` 由实现生成；工程文件 `pd_feature_bd_2020_2.xpr`。
- **`.xsa`**：`pd_feature_bd_wrapper.xsa`（约 1.27 MB，已导出给 PS 侧软件用，见 `sw/` 目录下的 PS 代码）。
- **ILA 调试核**：`ila_0`、`ila_clk_reset_diag_0`（Block Design 内）——上板后用 Vivado 抓实时波形，专看 `clk`/`reset` 诊断。
- **时序报告**：实现后在 `reports/` 目录与 `.runs/*/...timing_*.rpt` 生成 WNS/TNS/WHS（第 5 节概念的数据来源）。
- **重要 XDC 坑（本工程注释）**：`pd_feature_bd_timing.xdc:11-13` 写明——**不能用 `if` 等 Tcl 条件包住 `set_clock_groups`**，否则 XDC 解析器拒绝，导致异步 FIFO 跨域被当成“同步无关路径”来分析。

### 常见误解
- 误解：“`.bd` 和 `.v` 是一回事。”——`.bd` 是“IP 连线图 + 配置”，最终也会被展开成 RTL 参与综合；它管“谁连谁”，`.v` 管“每个模块内部怎么做”。
- 误解：“改了 RTL 不用重新跑综合/实现。”——必须重跑，否则比特流还是旧的。

> 如果这里搞错，会在本工程里表现为：在 `pd_feature_bd_timing.xdc` 里加 `if` 条件导致整段 `set_clock_groups` 被忽略（`pd_feature_bd_timing.xdc:11-13`），于是 `async_fifo` 的 ADC↔130MHz 跨域被错误当成同步路径分析，上板偶发数据错。

---

## 11　时钟资源：MMCM/PLL；50 MHz 如何变成 130 MHz 与 26 MHz

### 一句话定义
**MMCM / PLL** 是 FPGA 里的“时钟倍频/分频/移相”专用硬核。输入一个参考时钟，它能输出多个频率不同的时钟。本工程输入 50 MHz，经 `clk_wiz_0`（基于 MMCM）得到 130 MHz（系统主时钟）与 26 MHz。

### 为什么需要它 / 不用会怎样
PS/板载晶振往往只给一个固定频率（本工程 50 MHz，见 XDC `pl_gclk_50`，`pd_feature_bd_timing.xdc:6`）。但 PL 要 130 MHz 跑算法、ADC 要 26 MHz 采样——必须用时钟硬核派生，且派生时钟彼此“相位受控”，跨域处才方便用 CDC（第 4 节）。

### 本工程代码举例（数据全部来自 `pd_feature_bd.bd` 的 `clk_wiz_0` 参数）
从 `.bd` 解析得到（`/design/components/clk_wiz_0/parameters`）：
- `PRIM_IN_FREQ = 50.000`（输入参考 50 MHz）
- `MMCM_CLKFBOUT_MULT_F = 26.000`（反馈倍频 ×26）
- `MMCM_CLKOUT0_DIVIDE_F = 10.000`（输出 0 分频 ÷10）
- `MMCM_CLKOUT1_DIVIDE = 50`（输出 1 分频 ÷50）

**计算与验证**：
- MMCM 内部 VCO 频率 = `PRIM_IN_FREQ × CLKFBOUT_MULT = 50 × 26 = 1300 MHz`。
- 输出 0（clk_out0）= `VCO ÷ CLKOUT0_DIVIDE = 1300 ÷ 10 = 130 MHz` ✅（即系统主时钟，所有 PL 逻辑 `FREQ_HZ 130000000`，见 `pd_filter_chain.v:9`、`pd_sync_pulse.v:6`）。
- 输出 1（clk_out1）= `VCO ÷ CLKOUT1_DIVIDE = 1300 ÷ 50 = 26 MHz` ✅（与 ADC 采样率 26 MHz 对应；也可对照 XDC `adc_clk -period 38.461538 ns = 1/26 MHz ≈ 26 MHz`，`pd_feature_bd_timing.xdc:7`）。
- 交叉验证：XDC `create_clock -name pl_gclk_50 -period 20.000`（`pd_feature_bd_timing.xdc:6`）确认输入就是 50 MHz（周期 20 ns）。

**异步关系（依旧来自本工程 XDC）**：`adc_clk`（26 MHz，外部 `adc_clk_0` 脚）与 `clk_wiz_0/clk_out1`（130 MHz）被 `set_clock_groups -asynchronous` 显式声明异步（`:14-16`）；`clk_fpga_0`（PS FCLK0）与 `clk_wiz_0` 输出也异步（`:22-24`）。这正是第 4 节 CDC 的物理来源。

### 常见误解
- 误解：“时钟频率是‘随便设’的。”——受 MMCM 的 VCO 范围、输入/输出分频整数比等硬件限制；本工程值是算出来且整数比的（1300 整除 10 和 50）。
- 误解：“派生时钟和输入时钟是‘同步’的，可以直接跨域传数据。”——即使同源派生，只要不在同一时钟网络/相位受控，仍按异步处理（见 XDC 的 `set_clock_groups`）。

> 如果这里搞错，会在本工程里表现为：以为 130 MHz 与 26 MHz 是“同源同步”而去掉 `async_fifo`，ADC↔PL 跨域直接连线，上板后 `pd_adc_cdc` 数据错乱（对照 `pd_feature_bd_timing.xdc:14-16` 的异步声明）。

---

## 12　定点数：为什么不用浮点；Q8.8 / Q15；`>>>` 算术右移与补码；饱和截断

### 一句话定义
**定点数**＝把一个整数“约定好小数点位置”来表示小数。例如 **Q8.8**＝共 16 位、低 8 位是小数（1 LSB = 1/256）；**Q15**＝共 16 位、低 15 位是小数（1 LSB = 1/32768）。硬件里没有“小数点”，全靠约定。

### 为什么需要它 / 不用会怎样
FPGA 的 DSP 硬核做**整数乘加**极快，但做**浮点**要巨大面积且慢。本工程所有“标定、滤波、FFT”都用定点：电荷 `q(pC)` 用 Q8.8（`cfg_scale` 是 Q8.8，`pd_feature_core.v:54`），IIR 系数用 Q 格式（`pd_iir_biquad.v:6-9` `CW=18, FW=16`），PS 侧 FFT 用 Q15（`pd_spectrum.c`）。

### 本工程代码举例
#### Q8.8 与 `>>>` 算术右移、饱和
- 标定：电荷 `q(pC) = (q_signed × scale) >>> 8`，`scale` 是 Q8.8（`pd_feature_core.v:375` 注释；计算在 `:390-393`）：
  ```verilog
  wire signed [31:0] q0_mul = q0_sgn * $signed({1'b0, cfg_scale});
  wire signed [15:0] q0_pc  = sat16(q0_mul >>> 8);   // >>>8 = 除以 256, 即 Q8.8 去小数
  ```
  `>>>8` 是**算术右移**（补符号位），对负数也正确（见下）。`sat16`（`pd_feature_core.v:135-142`）把 32 位结果饱和到 16 位：
  ```verilog
  function signed [15:0] sat16;
      input signed [31:0] v;
      begin
          if      (v >  32'sd32767) sat16 =  16'sd32767;
          else if (v < -32'sd32768) sat16 = -16'sd32768;
          else                      sat16 = v[15:0];
      end
  endfunction
  ```
- 补码与去直流：`sdata = adc_data - ZERO_CODE`（`pd_feature_core.v:124, 180`，`ZERO_CODE=0x800` 即 12 位偏移二进制的“中值”），把 0x800 偏置的 ADC 码转成有符号补码。
- IIR 的饱和/截断：`pd_iir_biquad.v:18-19` 定义 `MAXV/MINV`（16 位有符号上下限），`:49` `shifted = rounded >>> FW`（FW=16，算术右移 16 位去小数），`:52-53` 用 `max_ext/min_ext` 做饱和：
  ```verilog
  wire signed [DW-1:0] y_next = (shifted > max_ext) ? MAXV :
                                 (shifted < min_ext) ? MINV : shifted[DW-1:0];
  ```
- PS 侧 Q15 乘法：`pd_spectrum.c:32-37` 的 `q15_mul` 用 64 位中间积、四舍五入后 `>>15`：
  ```c
  static s32 q15_mul(s32 a, s32 b) {
      s64 product = (s64)a * b;
      if (product >= 0) return (s32)((product + (1LL << 14)) >> 15);
      return -(s32)(((-product) + (1LL << 14)) >> 15);
  }
  ```
  `>>15` 即 Q15 去小数；`+ (1<<14)` 是“四舍五入”（加半 LSB）。

#### 为什么是算术右移（`>>>`）而不是逻辑右移（`>>`）
Verilog 里：**有符号数用 `>>>` 会补符号位**（负数右移后还是负），无符号/逻辑 `>>` 补 0。本工程全部用 `>>>`，例如 `pd_feature_core.v:392`、`pd_iir_biquad.v:49`、`:48` 的 `rounded = acc + (1 <<< (FW-1))`（`<<<` 是算术左移，保持有符号）。若错用逻辑右移，负数会变成巨大正数。

### 常见误解
- 误解：“`>>>8` 就是‘除以 256’随便用。”——只对**有符号算术右移**成立；且移完要**饱和**（否则溢出会回绕成错误极值，见 `sat16`/`MAXV/MINV` 的必要性）。
- 误解：“定点数精度够用、不用管溢出。”——定点溢出是“静默回绕”，本工程用 `sat16`/`MAXV/MINV`（`:52-53, 138-140`）专门防，否则电荷量会算飞。

> 如果这里搞错，会在本工程里表现为：把 `q0_pc = sat16(q0_mul >>> 8)`（`pd_feature_core.v:392`）的 `>>>` 错写成逻辑 `>>`，负电荷样本会变成巨大正数，PRPD 图谱出现“伪大放电点”；或漏掉 `sat16` 饱和，溢出回绕使 Σ|q|/I/P 统计全错。

---

## 自测 12 问（问题 + 参考答案要点）

1. **FPGA 和 MCU 的本质区别是什么？** 答：FPGA 烧进去的是“电路结构”，数据流过即算；MCU 是固定电路按“指令序列”顺序执行。本工程 4×12bit×26MSPS 流处理必须并行，所以用 PL。

2. **`always @(*)` 和 `always @(posedge clk)` 分别综合成什么？** 答：前者组合逻辑（导线/门），后者时序逻辑（寄存器，仅在时钟上升沿更新）。见 `pd_feature_core.v:365` vs `:161`。

3. **`=` 和 `<=` 用错会有什么后果？** 答：`<=` 非阻塞是“边沿同时取样、块末生效”，只能在时序块用；组合块误用 `<=` 会差拍/读到旧值，如 `pd_feature_core.v:433-436` 强调同源载入。

4. **为什么 BRAM 写块不能含复位？** 答：7 系列 BRAM 无“写时清存储”硬件，含复位会被综合判定非标准模板并**静默降级**成 FF（`async_fifo.v:8-21`、`pd_axis_fifo.v:9-12`）。

5. **两级同步器和异步 FIFO 分别用在什么场景？** 答：单比特异步信号（如 `sync_in`）用两级同步器（`pd_sync_pulse.v:17-31`）；多比特总线跨域（如 12/24/48 位）必须用异步 FIFO+格雷码（`async_fifo.v`）。

6. **为什么 `async_fifo` 判满要“最高两位都取反”？** 答：二进制“满=领先一圈”转格雷码后，最高位取反会传染次高位，故需 `~[PTR_W-1]` 和 `~[PTR_W-2]`（`async_fifo.v:78-92`），只翻最高位会命中 0/4096、FIFO 静默溢出。

7. **WNS 为负数意味着什么？** 答：建立时间裕量最坏值为负＝有时序路径在一个时钟周期内走不完＝可能算错。本工程 `pd_feature_core.v:533-539` 曾报 WNS -1.056 ns 而拆流水。

8. **流水线用“延迟增加”换来了什么？** 答：换“单拍组合路径变短、频率能拉高”。如 `pd_feature_core` 的 S_PEAK→S_Q1→S_Q2→S_ACC（`:127-133, 655-694`）和 `pd_iir_biquad` 的 phase 0..4（`:2-4`）。

9. **AXI4-Lite 的“读回一致 ≠ 配置生效”指什么？** 答：系数先写进 shadow（`sh_coef`），只有写 APPLY 位才搬去 active（`ac_coef`）（`pd_filter_chain.v:89-95`）；`pd_feature_core` 同款在 `S_CYC` 边界换档（`:875-879`）。

10. **AXI4-Stream 的背压是怎么回传的？** 答：下游拉低 `ready`→上游 `valid` 保持但不传→`ready` 一路回头停住生产者。`pd_pack192.v:19-20, 56-61` 用 `o_s48_ready`/`m_axis_tready` 例示。

11. **50 MHz 怎么变成 130 MHz 和 26 MHz？** 答：MMCM VCO = 50×26 = 1300 MHz；`clk_out0 = 1300/10 = 130 MHz`，`clk_out1 = 1300/50 = 26 MHz`（`pd_feature_bd.bd` 的 `clk_wiz_0` 参数；XDC `:6-7` 佐证）。

12. **Q8.8 的 `q×scale >>> 8` 为什么要配 `sat16`？** 答：`>>>8` 是算术右移去小数（Q8.8），但乘积可能超出 16 位有符号范围，不饱全会静默回绕成错误极值；`sat16`（`pd_feature_core.v:135-142`）把结果夹到 [−32768, 32767]。

---

> 备注（待验证）：本工程部分源码注释仍写“典型 100MHz / 100MHz 域”（如 `pd_feature_core.v:18`、`:19` 与 `pd_adc_cdc.v:33, 37-38` 端口注释），而 `.bd` 的 `clk_wiz_0` 与多处 `X_INTERFACE_PARAMETER FREQ_HZ 130000000` 实际为 **130 MHz**。这处“注释频率”与“真实配置频率”的不一致，建议以 BD/XDC 的 130 MHz 为准，旧注释待核实更新。
