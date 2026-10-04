# A03 · 滤波链：四通道 IIR 带通（含完整 AXI-Lite 从机）

覆盖文件：
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_iir_biquad.v`（77 行）—— 单个二阶节
- `pd_feature_bd_2020_2.srcs/sources_1/imports/rtl/pd_filter_chain.v`（117 行）—— 四通道链路 + 寄存器

> **设计者自己把两个文件都写成了"极简风格"**：注释很少、一行塞多个语句、用 `;` 把多条语句压在一行。这是为了在 130 MHz 下保持代码紧凑便于审查，但**对初学者很不友好**。本篇的工作就是把它们"摊开"。

---

## 0. 这个模块在系统里的位置和当前状态

```
pd_ddr_0/o_feat_data (48 bit, 4×12 bit, 130 MHz 域)
        │
        ▼
   ┌─────────────────────────────────────────────────┐
   │  pd_filter_0  (pd_filter_chain)                 │
   │   4 通道 × N_STAGE 个 pd_iir_biquad             │
   │   每个节 5 个系数 (b0,b1,b2,a1,a2), CW=18 bit   │
   │   AXI4-Lite 从机: 写系数 / 写控制 / 读影子寄存器  │
   └─────────────────────────────────────────────────┘
        │  filt_data / filt_dv
        ▼
   pd_feature_0 (pd_feature_sys_top, INPUT_CDC=0)
```

**当前 BD 的实际参数**（来自 `.bd` 的 `pd_filter_0` CONFIG 段，必须以此为准，不能看 RTL 的 parameter 默认值）：

```
NUM_CH = 4      ADC_W = 12      DW = 16      CW = 18      FW = 16
N_BP   = 1      N_NT  = 0       （即 N_STAGE = 1，只有 1 个带通节，没有陷波节）
CLK_HZ = 130000000   SAMPLE_HZ = 26000000
C_S_AXI_ADDR_WIDTH = 16   C_S_AXI_DATA_WIDTH = 32
```

**关键状态：默认是"位精确全旁路"**。复位后 `fctrl_bypass = 1` 且 `bypass_mask = 4'b1111`，`filt_data` 与 `filt_dv` 都是 `adc_data`/`adc_dv` 的**逐位透传**。所以**当前工程里这个滤波器实际不起作用**，除非 PS 显式写寄存器打开它。这不是 bug，是刻意的"默认安全"设计（旁路状态最容易验证：逐位相同）。

---

## 1. `pd_iir_biquad.v`（77 行）—— 一个 5 拍的二阶 IIR 节

### 1.1 数学背景：二阶 IIR（biquad）是什么

一个二阶 IIR 节的差分方程是：

```
y[n] = b0·x[n] + b1·x[n−1] + b2·x[n−2] − a1·y[n−1] − a2·y[n−2]
```

- `x` 是输入，`y` 是输出，`b0/b1/b2` 是前馈系数，`a1/a2` 是反馈系数。
- **"IIR"（无限冲激响应）** 指输出被反馈回输入侧，所以一个冲激产生的响应理论上永不归零。
- **"二阶"** 指方程里最高延迟是 2 拍（`x[n-2]`、`y[n-2]`）。
- 一个 biquad 可以实现：低通、高通、带通、带阻、全通。本工程只用它做**带通**。
- 系数是定点的：`CW = 18` 位，`FW = 16` 位小数（见下面 `ONE_C` 的推导）。

### 1.2 文件头（第 1–4 行）

```
1 `timescale 1ns / 1ps
2 // Five-clock, second-order IIR section for the 130 MHz / 26 MHz DDS prototype.
3 // PH is deliberately asserted as 5: the arithmetic schedule has three
4 // multiply/accumulate phases plus one output phase.
5 module pd_iir_biquad #(
6     parameter integer DW = 16,
7     parameter integer CW = 18,
8     parameter integer FW = 16,
9     parameter integer PH = 5
10 )(
11     input wire clk, input wire rst_n, input wire clr, input wire dv,
12     input wire signed [DW-1:0] din,
13     input wire signed [CW-1:0] c_b0, c_b1, c_b2, c_a1, c_a2,
14     output reg signed [DW-1:0] dout, output reg dout_dv
15 );
```
- 第 2 行 "Five-clock" = **每 5 个时钟处理 1 个样本**。这是本模块最重要的契约。
- 第 3–4 行解释 `PH=5`：算术调度需要"3 个乘加阶段 + 1 个输出阶段"（再加 1 个空闲/交接阶段 = 5）。
- 参数：`DW = 16` 数据位宽、`CW = 18` 系数位宽、`FW = 16` 小数位、`PH = 5` 相位数。
- 端口：`clr` 是"清状态"（复位滤波器记忆），`dv` 是"本拍输入有效"。

**为什么 5 拍一个样本？** 因为 `b0·x[n] + b1·x[n−1] + b2·x[n−2] − a1·y[n−1] − a2·y[n−2]` 需要 5 次乘法和 4 次加法。如果全塞在一拍里做，130 MHz 下组合路径太长（`16×18` 的乘法器 + 32 bit 累加 + 饱和判断）。所以设计成"一拍一个乘加"的**串行乘累加**结构。

**代价**：`26 MSPS` 时每秒需要 26 M 个样本 × 5 拍 = 130 M 拍/秒，而时钟正好 130 MHz → **零余量**。这就是"本 IIR 在 26 MSPS 下刚刚够，在 65 MSPS 下不够"的根本原因（65 MSPS 需要 325 MHz 时钟）。

### 1.3 位宽推导（第 16–25 行）

```
16    localparam integer PW = DW + CW;
17    localparam integer AW = PW + 3;
18    localparam signed [DW-1:0] MAXV = {1'b0,{(DW-1){1'b1}}};
19    localparam signed [DW-1:0] MINV = {1'b1,{(DW-1){1'b0}}};
```
- `PW = DW + CW = 16 + 18 = 34`：**乘积位宽**。两个 16 bit 与 18 bit 有符号数相乘，结果最大 34 bit（16+18），位宽不会溢出。
- `AW = PW + 3 = 37`：**累加器位宽**，比乘积宽 3 位。**为什么要 +3？** 因为要把 5 个乘积相关项加起来（最大约 4 个乘积同号），位宽需 +2；再加 1 位用于后续的舍入加法（第 48 行的 `+ (1 <<< (FW-1))`）可能进位。取 +3 是保守的余量。
- `MAXV = {1'b0, {15{1'b1}}}` = 0_1111_1111_1111_1111 = **+32767**（16 bit 有符号最大值）。
- `MINV = {1'b1, {15{1'b0}}}` = 1_0000_0000_0000_0000 = **−32768**（16 bit 有符号最小值）。
  - ⚠️ 注意 `MINV` 用的是 `{1'b1, {(DW-1){1'b0}}}`，即"符号位 1 + 15 个 0"，这正是 −32768 的补码表示（补码里 −2¹⁵ = 1000_0000_0000_0000）。**这是饱和上限/下限的常量**，后面第 50–53 行用它做饱和判断。

```
20    reg [2:0] phase;
21    reg signed [DW-1:0] x1, x2, y1, y2;
22    reg signed [PW-1:0] prod_a, prod_b;
23    reg signed [AW-1:0] acc;
24    reg signed [DW-1:0] mul_a, mul_b;
25    reg signed [CW-1:0] mul_ca, mul_cb;
```
逐条解释这组寄存器（**理解它们是理解整个模块的关键**）：

| 寄存器 | 位宽 | 含义 |
|---|---|---|
| `phase` | 3 bit | 相位计数器 0…4，驱动整个调度 |
| `x1, x2` | 16 bit 有符号 | 输入延迟：`x1 = x[n−1]`，`x2 = x[n−2]` |
| `y1, y2` | 16 bit 有符号 | 输出延迟：`y1 = y[n−1]`，`y2 = y[n−2]` |
| `prod_a, prod_b` | 34 bit 有符号 | **乘法流水寄存器**（第 1 级流水：乘法结果） |
| `acc` | 37 bit 有符号 | **累加器**（第 2 级流水：累加结果） |
| `mul_a, mul_b` | 16 bit 有符号 | 参与乘法的**数据操作数**（组合选择） |
| `mul_ca, mul_cb` | 18 bit 有符号 | 参与乘法的**系数操作数**（组合选择） |

**为什么要分成 `mul_a/mul_ca`（操作数）与 `prod_a/prod_b`（乘积）？** 这是流水线的两级：
- `mul_a/mul_ca` 是**组合逻辑的选择结果**（按 `phase` 选要用哪一对操作数）；
- `prod_a = mul_a * mul_ca` 是**乘法器**（DSP48E1）的输出，被寄存一拍；
- 这样"选择操作数"和"乘法"分在不同拍，缩短了单拍组合路径。

### 1.4 参数自检（第 27–29 行）

```
27    initial begin
28        if (PH != 5) $error("pd_iir_biquad requires PH=5");
29    end
```
- **硬性自检**：`PH` 必须等于 5。因为下面的调度逻辑是按 5 个相位写死的，改 `PH` 会被静默忽略（`phase == 3'd4` 之类的判断写死了）。自检把"改参数无效"这个隐患暴露在仿真阶段。

### 1.5 操作数选择（第 31–40 行）

```
31    always @* begin
32        mul_a = {DW{1'b0}}; mul_b = {DW{1'b0}};
33        mul_ca = {CW{1'b0}}; mul_cb = {CW{1'b0}};
34        case (phase)
35            3'd0: begin mul_a=din; mul_ca=c_b0; mul_b=y1; mul_cb=c_a1; end
36            3'd1: begin mul_a=x1;  mul_ca=c_b1; mul_b=y2; mul_cb=c_a2; end
37            3'd2: begin mul_a=x2;  mul_ca=c_b2; end
38            default: begin end
39        endcase
40    end
```
这是一个**组合选择器**（`always @*`），按相位决定这一拍要算哪两项乘积。**先把默认值清零**（第 32–33 行）是防止 `case` 未覆盖时产生 latch（组合逻辑必须"每条路径都赋值"，否则综合器会推断出锁存器）。

逐相位对应关系（**这就是"3 拍完成 5 项乘加"的核心调度表**）：

| phase | `mul_a×mul_ca` | `mul_b×mul_cb` | 对应差分方程的项 |
|---|---|---|---|
| 0 | `din × b0` | `y1 × a1` | `b0·x[n]` 与 `a1·y[n−1]` |
| 1 | `x1 × b1` | `y2 × a2` | `b1·x[n−1]` 与 `a2·y[n−2]` |
| 2 | `x2 × b2` | （0） | `b2·x[n−2]` |
| 3 | （0） | （0） | 累加收尾 |
| 4 | （0） | （0） | 输出 + 更新延迟链 |

注意：`mul_b/mul_cb` 在 phase 2 之后被清零，而 `prod_b` 仍然保留上一次的值（因为是寄存的），这会在累加时引入一个需要处理的细节——见 1.7。

### 1.6 组合运算与舍入/饱和（第 41–53 行）

```
41    wire signed [PW-1:0] mul_p_a = mul_a * mul_ca;
42    wire signed [PW-1:0] mul_p_b = mul_b * mul_cb;
```
- 两个乘法器。**`mul_a/mul_ca` 被声明为 `reg` 但在 `always @*` 里赋值，所以综合出来是组合逻辑**；乘法本身是组合的（`wire`），其结果在下一拍被寄存进 `prod_a/prod_b`。
- **这是"两级流水"结构**：选择（组合）→ 乘法（组合）→ 寄存。寄存器 `prod_a/prod_b` 把乘法结果隔离出来，从而"乘法器输出 → 累加器输入"之间的路径被一拍寄存器切断。

```
43    wire signed [AW-1:0] pa_ext = {{(AW-PW){prod_a[PW-1]}},prod_a};
44    wire signed [AW-1:0] pb_ext = {{(AW-PW){prod_b[PW-1]}},prod_b};
```
- **符号扩展**：把 34 bit 的乘积扩展到 37 bit。写法 `{{(AW-PW){prod_a[PW-1]}}, prod_a}` 的意思是"把符号位复制 `(37-34)=3` 份，再拼上原值"。这是 Verilog 里做**有符号数符号扩展**的标准写法。
- **为什么必须手动扩展？** 因为如果直接写 `prod_a + acc`，Verilog 会把较窄的操作数**零扩展**（不是符号扩展），负数会算错。这是有符号算术里最经典的坑。

```
45    wire signed [AW-1:0] sum1 = pa_ext - pb_ext;
46    wire signed [AW-1:0] sum2 = acc + pa_ext - pb_ext;
47    wire signed [AW-1:0] sum3 = acc + pa_ext;
```
**这三个和对应三个"乘加阶段"，但注意符号是 `−pb_ext`** —— 因为差分方程里反馈项是 `− a1·y[n−1] − a2·y[n−2]`，负号在选择阶段没有体现，而是在加法阶段统一减掉。这样做的好处：**系数可以按"低通/带通设计公式给出的正值"直接填入，不需要在系数里带负号**（更符合常见滤波器设计工具的输出格式）。

| 和 | 表达式 | 用在哪一拍 |
|---|---|---|
| `sum1` | `pa − pb` | `phase==1` 时载入 `acc`（清空并开始新样本的累加） |
| `sum2` | `acc + pa − pb` | `phase==2` 时载入 `acc` |
| `sum3` | `acc + pa` | `phase==3` 时载入 `acc`（此时 `pb` 已清零，只加 `b2·x[n−2]`… 见下方说明） |

⚠️ **【观察 · 值得仔细推演】** 累加序列与"预期 5 项"的对应关系需要按 `prod_a/prod_b` 的**流水延迟**来算。因为 `prod_a <= mul_p_a` 是**寄存**，所以：
- 在 phase 0 拍，`mul_a/mul_ca` 选出 `din×b0`，`mul_p_a` 组合得到结果，**但 `prod_a` 要到 phase 1 拍才更新为这个值**；
- 在 phase 1 拍，`acc <= sum1 = pa_ext − pb_ext`，此时 `pa` 是 phase 0 选的 `din×b0`、`pb` 是 phase 0 选的 `y1×a1` → `acc = b0·x[n] − a1·y[n−1]`；
- 在 phase 2 拍，`prod_a` 更新为 phase 1 选的 `x1×b1`、`prod_b` 更新为 `y2×a2` → `acc <= acc + x1·b1 − y2·a2`；
- 在 phase 3 拍，`prod_a` 更新为 phase 2 选的 `x2×b2`、`prod_b` 更新为 0（因为 phase 2 时 `mul_b/mul_cb` 被清 0）→ `acc <= acc + x2·b2`；
- 合计：`acc = b0·x[n] + b1·x[n−1] + b2·x[n−2] − a1·y[n−1] − a2·y[n−2]`。**与差分方程完全一致。**

  **这解释了为什么代码同时写了 `sum1/sum2/sum3` 三个不同的和**：因为 `phase==1` 要"从零开始累加"（不能加旧的 `acc`，那个 `acc` 已经是上一个样本残留的），而 `phase==2/3` 要"接着累加"。如果用 `sum2` 在 phase 1 用，就会把上一个样本的累加结果混进来。

```
48    wire signed [AW-1:0] rounded = acc + (1 <<< (FW-1));
49    wire signed [AW-1:0] shifted = rounded >>> FW;
```
- **舍入（rounding）**：`acc` 现在是 Q(FW) 的定点数，要右移 `FW = 16` 位取出整数部分。直接右移 = **截断（向下取整）**，会引入**系统性负偏置**（average 偏小半个 LSB）。加 `1 <<< (FW-1)` = 加 2¹⁵ = 半个最低位，再右移，就是**四舍五入**。
- `<<<` 是**算术左移**（对有符号数保持符号），`>>>` 是**算术右移**（右移时补符号位，负数右移后仍是负数）。对正数而言 `<<<`/`>>>` 和普通 `<<`/`>>` 一样；对负数，`>>>` 才是"除以 2 的幂"（`>>` 会补 0，把负数变正）。
- ⚠️ **这个舍入是"有偏"的简化版**：`+0.5 LSB 再截断` 对**负数**并不等价于四舍五入（例如 −1.6 + 0.5 = −1.1 → 截断到 −2，而正确四舍五入是 −2；−1.4 + 0.5 = −0.9 → 截断到 −1，正确是 −1 ✓）。严格来说需要"对称舍入"（`round-half-away-from-zero`）。对滤波器而言这点偏差可忽略，但**做定点精度分析时要知道这里用的是"加半再截断"**。

```
50    wire signed [AW-1:0] max_ext = {{(AW-DW){MAXV[DW-1]}},MAXV};
51    wire signed [AW-1:0] min_ext = {{(AW-DW){MINV[DW-1]}},MINV};
52    wire signed [DW-1:0] y_next = (shifted > max_ext) ? MAXV :
53                                   (shifted < min_ext) ? MINV : shifted[DW-1:0];
```
- 第 50–51 行：把 16 bit 的 `MAXV`/`MINV` 符号扩展成 37 bit，以便与 `shifted` 比较。
- 第 52–53 行：**饱和（saturation）**。若结果超过 +32767 → 钳到 +32767；小于 −32768 → 钳到 −32768；否则取低 16 位。
- **为什么必须饱和而不是直接截断？** 直接取低 16 位等于"回绕"（wrap-around）：一个很大的正数会突然变成一个大负数，在反馈系统（IIR）里这会造成**持续的自激振荡**，把整个滤波器搞崩。饱和虽然也不理想（削顶失真），但至少是**有界且方向正确**的。

### 1.7 相位状态机（第 55–60 行）

```
55    always @(posedge clk or negedge rst_n) begin
56        if (!rst_n || clr) phase <= 3'd0;
57        else if (dv) phase <= 3'd1;
58        else if (phase == 3'd4) phase <= 3'd0;
59        else if (phase != 3'd0) phase <= phase + 3'd1;
60    end
```
逐条：
- 第 56 行：复位或 `clr` → `phase = 0`（空闲态）。
- 第 57 行：**若本拍 `dv = 1`（有新样本），无条件把 `phase` 拉到 1，开始处理这个样本。**
- 第 58 行：若 `phase` 已到 4（最后一个相位），回 0。
- 第 59 行：`phase` 不为 0 时递增。
- `phase == 0` 且 `dv == 0` 时：**`phase` 保持 0**（第 59 行的 `phase != 3'd0` 条件不成立，且前两个条件也不成立 → 保持）。

**时序图（连续两个样本间隔 ≥ 5 拍时的正常情况）**：
```
拍号:      0    1    2    3    4    5    6    7    8    9   10 ...
dv:        1    0    0    0    0    1    0    0    0    0    1
phase:     0→1  1→2  2→3  3→4  4→0  0→1  1→2  2→3  3→4  4→0  0→1
dout_dv:   0    0    0    0    1    0    0    0    0    1    0
```
- `dout_dv` 在 `phase == 4` 那一拍被置 1（第 74 行），也就是**从 `dv` 到 `dout_dv` 恰好延迟 4 拍**，第 5 拍输出有效。
- 因此"**每 5 个时钟处理一个样本**"的契约成立，且是**流水线式**的（背靠背样本只需 5 拍间隔，不是 5 拍延迟 + 5 拍处理）。

⚠️ **【观察 · 关键隐患】** 第 57 行 `else if (dv) phase <= 3'd1;` **没有"`phase == 0` 才响应"的保护**。如果 `dv` 的间隔小于 5 拍（例如 2 拍一个样本），那么：
- 第 1 拍 `dv=1` → `phase` 变 1；
- 第 3 拍 `dv=1`（间隔 2 拍）→ 按第 57 行优先级，`phase` **被强制拉回 1**，而不是先走完 2→3→4；
- 结果：`phase` 永远在 1 和 2 之间来回，**永远到不了 4**，于是 `dout_dv` **恒为 0**，滤波器输出完全消失（静默失效）。

**这条推论是"65 MSPS 不能直接用本 IIR"的精确机制**：65 MSPS 在 130 MHz 下是每 2 拍一个样本 < 5 拍，会永久锁在 `phase = 1`。这是本工程最值得记住的结论之一，也是"要么把时钟提到 325 MHz（不可达），要么在进滤波器之前先做 CIC 抽取降速"的原因。

### 1.8 数据通路与延迟链（第 61–76 行）

```
61    always @(posedge clk or negedge rst_n) begin
62        if (!rst_n || clr) begin
63            prod_a<={PW{1'b0}}; prod_b<={PW{1'b0}}; acc<={AW{1'b0}};
64            x1<={DW{1'b0}}; x2<={DW{1'b0}}; y1<={DW{1'b0}}; y2<={DW{1'b0}};
65            dout<={DW{1'b0}}; dout_dv<=1'b0;
66        end else begin
67            prod_a <= mul_p_a; prod_b <= mul_p_b;
68            if (phase == 3'd1) acc <= sum1;
69            else if (phase == 3'd2) acc <= sum2;
70            else if (phase == 3'd3) acc <= sum3;
71            if (phase == 3'd4) begin
72                x2 <= x1; x1 <= din; y2 <= y1; y1 <= y_next; dout <= y_next;
73            end
74            dout_dv <= (phase == 3'd4);
75        end
76    end
```
逐行：
- 第 67 行：**乘法器结果无条件每拍寄存**（不需要 `if`，因为 `mul_p_a/mul_p_b` 是组合值，一直有效；寄存只是提供流水）。注意这里**没有** `if (phase==...)` 门控——无条件寄存是流水线的基本要求（否则会产生"该打拍的时候没打拍"的错位）。
- 第 68–70 行：**三个乘加阶段各自把对应的和写入 `acc`**。用 `if/else if` 链而不是 `case`，效果相同但更短。
- 第 71–73 行：`phase == 4` 时**一次性完成 5 件事**：
  - `x2 <= x1`（`x1` 变成 `x[n−2]`）
  - `x1 <= din`（新输入成为 `x[n−1]`）
  - `y2 <= y1`、`y1 <= y_next`（输出延迟链推进）
  - `dout <= y_next`（输出）
  ⚠️ 这 5 条都是**非阻塞赋值**，所以它们**同时**生效，互相读到的是"旧值"——这正是延迟链需要的语义（`x2` 拿到的是旧的 `x1`，不是刚更新的 `x1`）。**如果这里错用阻塞赋值 `=`，`x2` 会先拿到新 `x1`，延迟链就整体少一拍。**
- 第 74 行：`dout_dv <= (phase == 3'd4)` —— 输出有效脉冲。注意这一行**不在** `if (phase==4)` 里，而是无条件执行：`phase==4` 时输出 1，其余拍输出 0。这样 `dout_dv` 自然成为单拍脉冲。

### 1.9 `pd_iir_biquad` 的契约总结（**必背**）

| 项目 | 值 | 依据 |
|---|---|---|
| 处理速度 | **5 时钟/样本**（背靠背） | 第 2–4 行注释 + 相位调度 |
| 输入→输出延迟 | **4 拍**（`dv` 到 `dout_dv`） | 第 57/74 行推演 |
| 数据位宽 | 16 bit 有符号 | `DW` |
| 系数位宽 | 18 bit，**16 位小数（Q2.16）** | `CW`、`FW`；`ONE_C = 1<<16` |
| 饱和 | `[−32768, +32767]` | 第 18–19、52–53 行 |
| 舍入 | 加半再截断 | 第 48–49 行 |
| **前提** | `dv` 间隔 **≥ 5 拍** | 第 57 行推演（否则永久锁在 `phase=1`） |

---

## 2. `pd_filter_chain.v`（117 行）—— 四通道链路 + 完整的 AXI-Lite 从机

### 2.1 文件头与参数（第 1–34 行）

```
1  `timescale 1ns / 1ps
2  // Four-channel IIR band-pass/notch chain.  The AXI4-Lite slave accepts AW and
3  // W independently; reset defaults to a bit-exact external bypass.
4  module pd_filter_chain #(
5      parameter integer NUM_CH=4, ADC_W=12, DW=16, CW=18, FW=16,
6      parameter integer N_BP=1, N_NT=0, CLK_HZ=130000000, SAMPLE_HZ=26000000,
7      parameter integer C_S_AXI_DATA_WIDTH=32, C_S_AXI_ADDR_WIDTH=16
8  )(
```
- 第 2–3 行是两个关键声明：① AXI4-Lite 从机**能独立接收 AW 和 W**（这与 `pd_axil_regs.v` 不同——后者要求 AW/W 同时到，见 A07 的对比）；② 复位后默认**位精确旁路**。
- 第 5–6 行是接口参数；`N_BP`（带通节数）与 `N_NT`（陷波节数）**由 BD 配置为 1 和 0**。
- 注意 `ADC_W=12` 而 `DW=16`：输入的 12 bit 会被转换到 16 bit 内部精度，最终再压回 12 bit 输出（见第 113–114 行）。

```
9      (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME CLK, FREQ_HZ 130000000, ASSOCIATED_BUSIF S_AXI" *)
10     (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *) input wire clk,
11     (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME RST, POLARITY ACTIVE_LOW" *)
12     (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst_n RST" *) input wire rst_n,
13     input wire [NUM_CH*ADC_W-1:0] adc_data,
14     input wire [NUM_CH-1:0] adc_dv, output wire [NUM_CH*ADC_W-1:0] filt_data,
15     output wire [NUM_CH-1:0] filt_dv,
```
- 第 9–12 行的属性让 BD 自动识别时钟/复位接口。
- 第 13–15 行是数据接口：48 bit 输入（4×12）、4 位 dv、48 bit 输出、4 位 dv。**注意 `adc_data`/`adc_dv` 是"扁平总线"而不是 AXI-Stream**：没有 `tvalid/tready` 握手，只有 dv 脉冲。这是全工程内部模块间通信的统一样式（"数据 + 有效脉冲"）。

```
16     (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME S_AXI, PROTOCOL AXI4LITE, ADDR_WIDTH 16, DATA_WIDTH 32, FREQ_HZ 130000000" *)
17     (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWADDR" *) input wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_AWADDR,
...（第 17–33 行：完整的 AXI4-Lite 五通道端口：AW/W/B/AR/R，每个都带 X_INTERFACE_INFO 属性）
```
- 第 17–33 行是**标准 AXI4-Lite 从机端口**，共 5 个通道：
  - **AW（Write Address）**：`AWADDR/AWVALID/AWREADY`
  - **W（Write Data）**：`WDATA/WSTRB/WVALID/WREADY`
  - **B（Write Response）**：`BRESP/BVALID/BREADY`
  - **AR（Read Address）**：`ARADDR/ARVALID/ARREADY`
  - **R（Read Data）**：`RDATA/RRESP/RVALID/RREADY`
- 每个信号都带 `X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI <信号名>"`，这是让 BD 能把它识别成**一个 AXI 接口**（而不是 20 多个散线）的关键。属性不匹配会导致 BD 里看不到接口，或接口"维度退化"。

### 2.2 内部常量与握手信号（第 35–55 行）

```
35     localparam integer N_STAGE=N_BP+N_NT, N_CELL=NUM_CH*N_STAGE, N_COEF=N_CELL*5;
36     localparam signed [CW-1:0] ONE_C=(1 <<< FW), ZERO_C={CW{1'b0}};
```
- `N_STAGE = 1 + 0 = 1`（每通道的节数）；`N_CELL = 4×1 = 4`（全芯片的节总数）；`N_COEF = 4×5 = 20`（全芯片的系数总数）。
- `ONE_C = 1 <<< 16 = 65536`，用 18 bit 表示就是 `0x10000` —— **这就是 Q2.16 定点里的"1.0"**（16 位小数，所以 1.0 = 2¹⁶）。`ZERO_C = 0`。
- **`ONE_C` 的用途**：当某节未启用（`ac_en = 0`）时，把它当"系数为 (1, 0, 0, 0, 0)"的**恒等节**用（见第 111 行）。这样即使滤波器被旁路，数据仍然会经过这个节并**延迟固定拍数**，保证时序一致。

```
37     reg aw_hold, w_hold, bvalid, rvalid, cfg_valid;
38     reg [C_S_AXI_ADDR_WIDTH-1:0] awaddr_hold;
39     reg [31:0] wdata_hold, rdata, cfg_data;
40     reg [3:0] wstrb_hold, cfg_strb;
41     reg [11:0] cfg_addr;
42     wire aw_hs=S_AXI_AWVALID && S_AXI_AWREADY;
43     wire w_hs=S_AXI_WVALID && S_AXI_WREADY;
44     wire write_fire=!bvalid && (aw_hold || aw_hs) && (w_hold || w_hs);
45     wire [C_S_AXI_ADDR_WIDTH-1:0] waddr=aw_hold ? awaddr_hold : S_AXI_AWADDR;
46     wire [31:0] wdata=w_hold ? wdata_hold : S_AXI_WDATA;
47     wire [3:0] wstrb=w_hold ? wstrb_hold : S_AXI_WSTRB;
```
**这段是本模块最有价值的部分——"AW/W 独立接收"的实现**：

- 第 42–43 行：`aw_hs` = "AW 通道握手成功"（AWVALID 且 AWREADY），`w_hs` 同理。
- 第 44 行 `write_fire`：**当 B 通道空闲，且 AW 已到（本次到手 或 之前已缓存），且 W 已到（本次到手 或 之前已缓存）时，才算一次完整写事务**。
- 第 45–47 行：`waddr/wdata/wstrb` 用三目选择器 —— **如果 `*_hold` 标志已置位（说明该通道之前先到了、被缓存了），就用缓存值；否则用当前总线上正在到的新值。**
- ⚠️ **这就是"独立接收"的完整机制**。AXI 规范本身允许 AW 和 W **任意顺序、任意间隔**到达：
  - 如果从机要求"AW 和 W 必须同拍到达"，那么当主机先把 AW 发过来、过几拍才发 W 时，从机会因为 `AWREADY` 拉不起来而**阻塞总线**；
  - 有些 Xilinx 互连（crossbar）会缓存请求，所以这种不合规的从机"能凑合工作"；换个互连或换个主机就可能挂死。
  - `pd_axil_regs.v` 正是这种"要求 AW/W 同时有效"的写法（见 A07 的对比），而 `pd_filter_chain.v` 做对了。这是本工程内部**两种 AXI 从机风格并存**的实例。

```
51     assign S_AXI_AWREADY=!aw_hold && !bvalid && !cfg_valid;
52     assign S_AXI_WREADY =!w_hold  && !bvalid && !cfg_valid;
53     assign S_AXI_BVALID=bvalid; assign S_AXI_BRESP=2'b00;
54     assign S_AXI_ARREADY=!rvalid; assign S_AXI_RVALID=rvalid;
55     assign S_AXI_RRESP=2'b00; assign S_AXI_RDATA=rdata;
```
- 第 51–52 行：**只有"对应通道还没缓存、B 未在响应、配置转移未在途"时才拉高 ready**。用 ready 的拉低来反压主机，这是 AXI4-Lite 允许的（从机可以随时拖 ready）。
- 第 53 行 `S_AXI_BRESP = 2'b00`：**恒回 OKAY**（`2'b00`）。即本从机**永远不报错**——写一个不存在的地址、写非法值都不会有错误响应。这是一种简化（"宽容的从机"），代价是**软件无法通过 BRESP 发现地址写错**。→ 收集到 B09。
- 第 55 行 `S_AXI_RDATA = rdata`：读数据直接由组合逻辑给出。

### 2.3 字节选通合并（第 57–66 行）

```
57     function [31:0] merge32;
58         input [31:0] oldv;
59         input [31:0] newv;
60         input [3:0] strb;
61         begin merge32=oldv; if(strb[0])merge32[7:0]=newv[7:0]; if(strb[1])merge32[15:8]=newv[15:8]; if(strb[2])merge32[23:16]=newv[23:16]; if(strb[3])merge32[31:24]=newv[31:24]; end
62     endfunction
63     function [CW-1:0] mergecoef;
64         input [CW-1:0] oldv; input [31:0] newv; input [3:0] strb; reg [31:0] t;
65         begin t={{(32-CW){1'b0}},oldv}; t=merge32(t,newv,strb); mergecoef=t[CW-1:0]; end
66     endfunction
```
**这两个函数实现的是"WSTRB 字节选通"**，即 AXI 规范里的"部分写"（byte-enable write）：
- `WSTRB` 有 4 位，对应 `WDATA` 的 4 个字节。`strb[0]=1` 表示"只更新低字节，其余字节保持原值"。
- 第 61 行：`merge32` 先把 `oldv` 赋给结果，然后**按 strb 逐字节覆盖**。所以 `strb = 4'b0001` → 只改 `[7:0]`，`strb = 4'b1111` → 全部覆盖。
- **为什么需要它？** 因为有些主机（或调试脚本）会做 8 位/16 位写，例如"只改 `FCTRL` 的 bit0"。如果从机不实现 WSTRB 而直接整字覆盖，那么"读-改-写"的语义就丢了（会把同一寄存器里别的位清掉）。testbench `tb_pd_filter_chain.v:97-99` 就专门测了这个：写 `0x0000_ab00` 且 `strb=4'b0010`（只改第 2 字节）后，读回必须是 `0x0001_ab45`（保留低 16 位 `ab45` 的原有 `45`）。
- 第 63–66 行 `mergecoef`：把 18 bit 系数先零扩展到 32 bit，用 `merge32` 合并字节，再截回 18 bit。**注意这里是"零扩展"（`{{(32-CW){1'b0}}, oldv}`）而非符号扩展**——对**无符号的系数表示**是正确的；如果系数按有符号理解，零扩展会改变其数值。**这是 Q2.16 系数按"无符号 18 bit 表示"存储的隐含约定**，写系数时软件必须自己把负数转成补码形式（18 bit 补码）。

### 2.4 地址解码（第 71–74 行）

```
71     wire [11:0] wa=cfg_addr; wire [11:0] ca=wa-12'h010;
72     wire [1:0] c_ch=ca[10:9]; wire [2:0] c_stage=ca[7:5], c_reg=ca[4:2];
73     wire c_valid=(c_ch<NUM_CH)&&(c_stage<N_STAGE)&&(c_reg<=5);
74     wire [4:0] c_idx=c_ch*N_STAGE+c_stage; wire [7:0] c_base=(c_ch*N_STAGE+c_stage)*5;
```
**这是系数地址的解码规则，必须记住（软件写系数就按这个算）**：

| 字段 | 位段 | 含义 | 步长 |
|---|---|---|---|
| `c_ch` | `ca[10:9]` | 通道号 0…3 | **0x200**（512 字节/通道） |
| `c_stage` | `ca[7:5]` | 节号 0…N_STAGE−1 | **0x20**（32 字节/节） |
| `c_reg` | `ca[4:2]` | 节内寄存器号 | **0x04**（4 字节/寄存器） |

- `ca = wa − 0x010`：寄存器区从偏移 `0x010` 开始。
- `c_reg` 的含义：`c_reg == 0` 是"节使能"（`sh_en`），`c_reg == 1…5` 是"系数 b0/b1/b2/a1/a2"（因为 `c_base + c_reg − 1` 做索引）。
- **完整地址公式**：`地址 = 0x010 + ch×0x200 + stage×0x20 + reg×0x04`。
  - 例：ch0 / stage0 / b0 → `0x010 + 0 + 0 + 1×4 = 0x014`（与 testbench 第 94 行 `axil_write_w_then_aw(16'h0014, ...)` 一致 ✓）。
- `c_valid` 检查三个边界：通道号、节号、寄存器号。注意 `c_reg<=5` 是"≤5"（0…5 共 6 个）—— 因为 `c_reg==0` 是使能，1…5 是系数。
- ⚠️ **【观察】`c_stage` 只有 3 bit（`ca[7:5]`），最大表示 7**。也就是说**即使把 `N_STAGE` 配成 8 以上，地址空间也只支持 8 个节**。这是"地址图与 `N_STAGE` 无关"的硬限制（B09 已记录）。同理 `c_ch` 只有 2 bit，最大 3（正好 4 通道）。
- ⚠️ **【观察】`c_valid` 里 `c_reg<=5` 的判断与后面用的 `c_reg-1` 索引配对**：当 `c_reg==0` 时代码走 `sh_en` 分支（第 94 行前半），所以 `c_base + c_reg - 1` 只在 `c_reg>=1` 时被求值。**但 Verilog 是组合逻辑，`c_base + 0 - 1` 会被算出来（下溢）**，只是不被使用。这在仿真中可能产生 X 传播，综合时会被优化掉。属于风格瑕疵。

### 2.5 写路径：两段式流水（第 76–97 行）

```
76     always @(posedge clk or negedge rst_n) begin
77         if(!rst_n) begin aw_hold<=0; w_hold<=0; bvalid<=0; cfg_valid<=0; awaddr_hold<={C_S_AXI_ADDR_WIDTH{1'b0}}; wdata_hold<=32'd0; wstrb_hold<=4'd0; cfg_addr<=12'd0; cfg_data<=32'd0; cfg_strb<=4'd0; end
78         else begin
79             if(cfg_valid) begin cfg_valid<=0; bvalid<=1; end
80             if(write_fire) begin aw_hold<=0; w_hold<=0; cfg_valid<=1; cfg_addr<=waddr[11:0]; cfg_data<=wdata; cfg_strb<=wstrb; end
81             else begin if(aw_hs) begin aw_hold<=1; awaddr_hold<=S_AXI_AWADDR; end if(w_hs) begin w_hold<=1; wdata_hold<=S_AXI_WDATA; wstrb_hold<=S_AXI_WSTRB; end end
82             if(bvalid && S_AXI_BREADY && !cfg_valid) bvalid<=0;
83         end
84     end
```
**【设计要点】** 第 48–50 行的注释解释了为什么要两段：

> AXI 数据先被捕获进 `cfg_*` 寄存器。**这个寄存器边界把 crossbar 的宽地址/数据扇出挡在 160 项的系数写译码器之外**，在 130 MHz 控制时钟下是必要的。

意思是：AXI crossbar 输出的 `AWADDR/WDATA` 是"宽扇出、长走线"的信号。如果直接拿它们去驱动"地址解码 + 160 个系数的写使能生成"这套复杂组合逻辑，路径会很长。**先打一拍寄存器（`cfg_*`），让解码逻辑从寄存器出发，把长路径切断。** 这是"为了时序而在协议层插入一级流水"的典型手法，代价是写事务延迟增加一拍。

逐行：
- 第 79 行：`cfg_valid` 为 1 的**下一拍**置 `bvalid` —— 即"配置已捕获"后回 B 响应。
- 第 80 行：`write_fire` 时，把 `waddr/wdata/wstrb` 存入 `cfg_addr/cfg_data/cfg_strb`，并置 `cfg_valid`。
- 第 81 行：`write_fire` 不成立时，**分别缓存先到的通道**（AW 先到就存 `aw_hold`，W 先到就存 `w_hold`）。
- 第 82 行：B 握手完成后清 `bvalid`。

```
85     always @(posedge clk or negedge rst_n) begin
86         if(!rst_n) begin fctrl_bypass<=1; bypass_mask<={NUM_CH{1'b1}}; coef_dirty<=0; apply_p<=0; clear_p<=0; for(i=0;i<N_CELL;i=i+1) begin sh_en[i]<=0; ac_en[i]<=0; end for(i=0;i<N_COEF;i=i+1) begin sh_coef[i]<={CW{1'b0}}; ac_coef[i]<={CW{1'b0}}; end end
87         else begin
88             apply_p<=0; clear_p<=0;
89             if(cfg_valid && wa<12'h010) case(wa[3:2])
90                 0: if(cfg_strb[0]) begin fctrl_bypass<=cfg_data[0]; if(cfg_data[1]) begin apply_p<=1; coef_dirty<=0; end if(cfg_data[2]) clear_p<=1; end
91                 2: bypass_mask<=merge32(bypass_mask,cfg_data,cfg_strb);
92                 default: begin end
93             endcase
94             if(cfg_valid && wa>=12'h010 && c_valid) begin if(c_reg==0) begin if(cfg_strb[0]) sh_en[c_idx]<=cfg_data[0]; end else sh_coef[c_base+c_reg-1]<=mergecoef(sh_coef[c_base+c_reg-1],cfg_data,cfg_strb); coef_dirty<=1; end
95             if(apply_p) begin for(i=0;i<N_CELL;i=i+1) ac_en[i]<=sh_en[i]; for(i=0;i<N_COEF;i=i+1) ac_coef[i]<=sh_coef[i]; end
96         end
97     end
```
这段是整个模块的**配置核心**，需要慢读。

**复位默认值（第 86 行）**：
- `fctrl_bypass <= 1`（**旁路**）
- `bypass_mask <= {NUM_CH{1'b1}}` = `4'b1111`（**四通道全旁路**）
- `coef_dirty <= 0`
- 所有 `sh_en[i] <= 0`（影子使能全关）、`ac_en[i] <= 0`（生效使能全关）
- 所有 `sh_coef/ac_coef <= 0`

→ **所以复位后：输出 = 输入（逐位透传）**。这解释了 `tb_pd_filter_chain.v:83-84` 为什么能在复位后立刻检查"bypass 样本逐位相同"。

**影子寄存器（shadow）vs 生效寄存器（active）—— 这是本模块最重要的概念**：

| 名字 | 角色 | 谁写 | 谁用 |
|---|---|---|---|
| `sh_en[]` / `sh_coef[]` | **影子**（暂存区） | AXI 写直接改这里 | 回读时读这里 |
| `ac_en[]` / `ac_coef[]` | **生效**（真正驱动 DSP 乘法器） | 只有 `apply_p` 时从影子拷贝 | `pd_iir_biquad` 的系数输入 |

- 第 94 行：写系数**只改影子**，并把 `coef_dirty` 置 1（"有新配置未生效"标志）。
- 第 95 行：`apply_p`（APPLY 脉冲）到来的那一拍，**一次性把影子全部拷贝到生效**。
- **为什么这样设计？** 因为滤波器系数**不能逐个更新**：如果先改 `b0` 再改 `b1`，中间会出现"半新半旧"的系数组合，滤波器可能瞬间不稳定（自激）。用"影子 + 一次性 APPLY"，保证系数切换是**原子的**。

**⚠️ 这就是"读回一致 ≠ 配置生效"的根源**（B09 里列为高优先级易错点）：
- 回读读的是 `sh_coef`（第 102 行 `read_mux` 的实现），所以"写进去、读出来一样"只能证明**写进了影子**；
- 真正生效的证据是 **`coef_dirty` 由 1 变 0**（第 90 行：APPLY 时清 `coef_dirty`）。`FSTATUS`（`0x04`）的 bit1 就是它。
- 更细的一条：**`ac_en=0` 时该节是恒等节**（第 111 行 `ac_en[CI]?ac_coef[CB]:ONE_C`），且 `filt_dv` **照常输出**（第 115 行）。所以"写了系数但没 APPLY"在数据流上**完全不可辨**——输出照样有、照样有效，只是没滤波。**唯一判据就是 `coef_dirty` 的 1→0 跳变。**

**FCTRL 寄存器（`wa < 0x010` 时的 `wa[3:2]==0` 分支）**：
- `bit0` → `fctrl_bypass`（写 1 则全芯片旁路）
- `bit1` → 产生 `apply_p` 脉冲（**并同时清 `coef_dirty`**）
- `bit2` → 产生 `clear_p` 脉冲（清滤波器内部状态 `x1/x2/y1/y2`）

⚠️ **【观察】第 90 行的 `fctrl_bypass<=cfg_data[0]` 是"只要 `cfg_strb[0]` 就无条件执行"的**，也就是说：**即使你只想发 APPLY（写 `0x2`），`cfg_data[0] = 0` 也会顺带把 `fctrl_bypass` 写成 0（解除旁路）**。这是一个容易踩的坑：想"只 APPLY 不改旁路"是做不到的，必须同时给出想要的 bypass 值。B09 已记录。

**`bypass_mask`（`wa[3:2]==2` 即偏移 `0x08`）**：每通道 1 位的旁路掩码，经过 `merge32` 支持字节选通。

### 2.6 读路径（第 98–103 行）

```
98     wire cfg_clr=apply_p|clear_p;
99     reg [31:0] read_mux; wire [11:0] ra=S_AXI_ARADDR[11:0], rca=ra-12'h010;
100    wire [1:0] rc_ch=rca[10:9]; wire [2:0] rc_stage=rca[7:5], rc_reg=rca[4:2];
101    wire rc_valid=(rc_ch<NUM_CH)&&(rc_stage<N_STAGE)&&(rc_reg<=5); wire [4:0] rc_idx=rc_ch*N_STAGE+rc_stage; wire [7:0] rc_base=(rc_ch*N_STAGE+rc_stage)*5;
102    always @* begin read_mux=0; if(ra<12'h010) case(ra[3:2]) 0:read_mux={31'd0,fctrl_bypass}; 1:read_mux={16'd0,8'h04,6'd0,coef_dirty,(apply_p|clear_p)}; 2:read_mux=bypass_mask; 3:read_mux=SAMPLE_HZ; endcase else if(rc_valid) begin if(rc_reg==0) read_mux={31'd0,sh_en[rc_idx]}; else read_mux={{(32-CW){1'b0}},sh_coef[rc_base+rc_reg-1]}; end end
103    always @(posedge clk or negedge rst_n) begin if(!rst_n) begin rvalid<=0; rdata<=0; end else if(S_AXI_ARVALID && S_AXI_ARREADY) begin rvalid<=1; rdata<=read_mux; end else if(rvalid && S_AXI_RREADY) rvalid<=0; end
```
**读寄存器表（地址 → 含义）**：

| 偏移 | 名称 | 内容 |
|---|---|---|
| `0x00` | FCTRL | `[0] fctrl_bypass` |
| `0x04` | FSTATUS | `[1] coef_dirty`、`[0] (apply_p \| clear_p)`、**`[15:8] = 0x04` 版本号** |
| `0x08` | BYPASS_MASK | `[3:0]` 每通道旁路位 |
| `0x0C` | SAMPLE_HZ | 采样率常数（`26000000`，来自 BD 参数） |
| `0x010 + ch×0x200 + st×0x20 + reg×0x04` | 系数/使能 | `reg==0` → 节使能；`reg==1..5` → 系数 b0/b1/b2/a1/a2 |

- 第 102 行 `1:read_mux={16'd0,8'h04,6'd0,coef_dirty,(apply_p|clear_p)}`：因为位域是 `{16'd0(bit31:16), 8'h04(bit15:8), 6'd0(bit7:2), coef_dirty(bit1), apply/clear(bit0)}`，所以**版本号 0x04 落在 `[15:8]`**。⚠️ **读 FSTATUS 时必须先屏蔽掉高 16 位**，否则会把 `0x0400` 之类的版本号当成状态。B09 已记录。
- 第 103 行：`rvalid` 由 AR 握手置位、由 R 握手清零；`rdata` 在 AR 握手那一拍从 `read_mux` 寄存。**注意 `read_mux` 是组合逻辑，用的是 `S_AXI_ARADDR` 的当前值**，所以数据在 AR 握手的同时就被采样了——不需要额外的"地址寄存"阶段。

### 2.7 数据通路：generate 例化（第 104–116 行）

```
104    genvar ch,st;
105    generate for(ch=0;ch<NUM_CH;ch=ch+1) begin: GCH
106        wire [ADC_W-1:0] raw=adc_data[ch*ADC_W+:ADC_W];
107        wire signed [DW-1:0] in_s=$signed({1'b0,raw})-(1<<(ADC_W-1));
108        wire signed [DW-1:0] sd[0:N_STAGE]; wire sv[0:N_STAGE]; assign sd[0]=in_s; assign sv[0]=adc_dv[ch];
109        for(st=0;st<N_STAGE;st=st+1) begin: GST
110            localparam integer CI=ch*N_STAGE+st, CB=CI*5;
111            pd_iir_biquad #(.DW(DW),.CW(CW),.FW(FW),.PH(5)) u_iir(.clk(clk),.rst_n(rst_n),.clr(cfg_clr),.dv(sv[st]),.din(sd[st]),.c_b0(ac_en[CI]?ac_coef[CB]:ONE_C),.c_b1(ac_en[CI]?ac_coef[CB+1]:ZERO_C),.c_b2(ac_en[CI]?ac_coef[CB+2]:ZERO_C),.c_a1(ac_en[CI]?ac_coef[CB+3]:ZERO_C),.c_a2(ac_en[CI]?ac_coef[CB+4]:ZERO_C),.dout(sd[st+1]),.dout_dv(sv[st+1]));
112        end
113        wire signed [DW-1:0] y=sd[N_STAGE]; wire signed [ADC_W-1:0] yc=(y>2047)?2047:(y< -2048)?-2048:y[ADC_W-1:0]; wire bypass=fctrl_bypass|bypass_mask[ch];
114        assign filt_data[ch*ADC_W+:ADC_W]=bypass?raw:(yc+(1<<(ADC_W-1)));
115        assign filt_dv[ch]=bypass?adc_dv[ch]:sv[N_STAGE];
116    end endgenerate
```
逐行这是全文件最需要理解的部分：

- 第 105 行 `generate for(ch=0; ch<4; ch=ch+1) begin: GCH`：**用 generate 循环把同一段逻辑复制 4 份**（每通道一份）。`begin: GCH` 给这个 generate 块起名，例化路径会变成 `GCH[0].u_iir` 这样的层次名。
- 第 106 行 `raw = adc_data[ch*ADC_W +: ADC_W]`：用**索引部分选择（indexed part-select）** 取第 ch 个通道的 12 bit。`+: ` 的含义是"从 `ch*12` 开始，向上取 12 位"。
- 第 107 行 `in_s = $signed({1'b0, raw}) - (1 << (ADC_W-1))`：**偏移二进制 → 补码的转换**。
  - `raw` 是 12 bit 无符号（0…4095）；`{1'b0, raw}` 把它零扩展成 13 bit 再声明为有符号，值仍是 0…4095；
  - 减去 `1 << 11 = 2048`，得到 **−2048…+2047**；
  - 结果赋给 `[DW-1:0]`（16 bit 有符号），自动符号扩展。
  - **这就是"0x800 = 2048 = 零电平"的数学实现**。
- 第 108 行 `wire signed [DW-1:0] sd[0:N_STAGE]; wire sv[0:N_STAGE];`：**声明两个"数组线网"**，长度是节数+1。`sd` 存各级数据、`sv` 存各级有效标志。`sd[0] = in_s`、`sv[0] = adc_dv[ch]` 是链的输入。
- 第 110 行 `CI = ch*N_STAGE + st`（全局节索引，0…3）、`CB = CI*5`（该节的系数基址，0, 5, 10, 15）。
- 第 111 行：例化 `pd_iir_biquad`。
  - `.din(sd[st])` / `.dout(sd[st+1])`：**把上一级的输出接给下一级的输入**，形成串联链；
  - `.dv(sv[st])` / `.dout_dv(sv[st+1])`：有效标志同样串联；
  - `.clr(cfg_clr)`：`apply_p | clear_p` —— 即**APPLY 或 CLEAR 时都会清滤波器状态**（避免切换系数时残留旧状态造成瞬态）；
  - **系数选择 `ac_en[CI] ? ac_coef[CB+k] : (ONE_C 或 ZERO_C)`**：
    - `ac_en[CI] = 0`（该节未启用）→ `b0 = ONE_C = 1.0`，`b1=b2=a1=a2 = 0` → **恒等节**（输出 = 输入）；
    - `ac_en[CI] = 1` → 用生效系数。
  - **为什么要这样？** 因为如果直接把系数清零（全 0），输出会恒为 0，数据链就断了：`y = 0` 且 `y1=y2=0`，之后永远输出 0。用"恒等节"（b0=1）保证"未启用 = 直通"，且**延迟固定**（仍是 5 拍/样本），不改变下游时序。
- 第 113 行 `yc = (y>2047)?2047 : (y< -2048)?-2048 : y[11:0]`：**把 16 bit 结果饱和回 12 bit**（−2048…+2047）。
- 第 114 行 `filt_data = bypass ? raw : (yc + 2048)`：**补码 → 偏移二进制**（加回 2048 偏置）。`bypass` 为真时直接输出 `raw`（**逐位透传，不经任何转换**）。
- 第 115 行 `filt_dv = bypass ? adc_dv[ch] : sv[N_STAGE]`：`bypass` 时**直接用输入的 dv**（零延迟）；非旁路时用链尾的 dv（**延迟 4 拍**）。

⚠️ **【观察 · 重要的时序语义变化】** `bypass` 从 0 变 1（或反之）时，`filt_dv` 的来源会在"零延迟"与"延迟 4 拍"之间**瞬时切换**，可能存在一拍的重叠或丢失。因为 `bypass` 通常在采集停止时才改，实际风险低，但**这是一个未加保护的切换点**。→ 收集到 B09。

### 2.8 通带设计的定量说明（来自 RTL 之外的参数推导，作为理解辅助）

本工程配的是 `N_BP = 1`（一个二阶节做带通），目标频带 **100 kHz – 1 MHz**（采样率 26 MSPS）。一个二阶带通节可以写成"1 级高通 × 1 级低通"的级联近似，要求其 Q 值满足：

```
Q = f0 / BW ，其中 f0 = √(100k × 1M) ≈ 316 kHz，BW ≈ 900 kHz
→ Q ≈ 0.35 < 0.5
```

**Q < 0.5 是关键**：当 Q < 0.5 时，二阶带通的两个极点落在实轴上（不共轭），此时系统等价于"两个实极点的一阶节级联"，**一个二阶节就能实现宽band带通**。如果 Q > 0.5（窄带），一个二阶节的极点共轭、带宽做不到这么宽。

→ 结论：**"1 个二阶节做 100 kHz–1 MHz 的宽带带通"在数学上是可行的**，不需要多级级联。这也是 `N_BP=1` 够用的理论依据。

---

## 3. 寄存器地址速查（软件写系数直接查这张表）

| 偏移 | 名称 | 读写 | 位域 |
|---|---|---|---|
| `0x000` | FCTRL | W | `[0] bypass`、`[1] APPLY`(脉冲)、`[2] CLEAR`(脉冲) |
| `0x004` | FSTATUS | R | `[0] apply/clear`、`[1] coef_dirty`、`[15:8] = 0x04` |
| `0x008` | BYPASS_MASK | RW | `[3:0]` 每通道旁路 |
| `0x00C` | SAMPLE_HZ | R | 采样率 |
| `0x010 + ch*0x200 + st*0x20 + 0x00` | ST_EN | RW | `[0]` 该节使能（影子） |
| `0x010 + ch*0x200 + st*0x20 + 0x04*1` | B0 | RW | 18 bit Q2.16 |
| `… + 0x04*2 / *3` | B1 / B2 | RW | 同上 |
| `… + 0x04*4 / *5` | A1 / A2 | RW | 同上 |

**正确的配置顺序（务必按此执行，否则会静默失效）**：
1. 写 `BYPASS_MASK`/`FCTRL` 到"不旁路"（`bypass = 0`、mask 对应位 = 0）；
2. 写 5 个系数 + 节使能（此时只改影子，`coef_dirty` 变 1）；
3. 写 `FCTRL = 0x2` 发 APPLY（**注意这会同时把 bypass 写成 `cfg_data[0] = 0`，正好是我们要的"解除旁路"**）；
4. **验证 `FSTATUS[1] coef_dirty == 0`** —— 这是唯一有效的生效证据；
5. 可选：写 `FCTRL = 0x6` 做 CLEAR（清滤波器状态）。

---

## 4. 本篇的 12 条易错点

1. **`pd_iir_biquad` 要求 `dv` 间隔 ≥ 5 拍**；间隔 < 5 拍会永久锁在 `phase=1`，`dout_dv` 恒 0（静默失效）。这是 65 MSPS 不能直连本 IIR 的精确机制。
2. 系数是 **18 bit、Q2.16（16 位小数）**，`1.0 = 0x10000`；且按**无符号 18 bit 补码形式**存储（`mergecoef` 用零扩展）。
3. 差分方程里的负号（`−a1·y[n−1] − a2·y[n−2]`）**已固化在加法阶段**（`pa_ext − pb_ext`），系数按设计工具的**正值**填。
4. 必须做**符号扩展**（第 43–44 行）再相加，否则负数会算错（Verilog 默认零扩展）。
5. 输出必须**饱和**而不是截断，否则 IIR 的反馈会自激。
6. **读系数读的是影子寄存器**；"回读一致"只证明写进影子。**唯一生效判据 = `FSTATUS[1]` 由 1 变 0。**
7. `ac_en = 0` 时该节是**恒等节**（b0=1），且 `filt_dv` 照常输出 → **"写了系数没 APPLY"在数据流上不可辨**。
8. 写 `FCTRL = 0x2`（只发 APPLY）会**顺带把 `bypass` 写成 0**（解除旁路），不能"只 APPLY 不动旁路"。
9. `FSTATUS` 的 **`[15:8]` 是版本号 `0x04`**，读状态必须先屏蔽高位。
10. 系数地址步长：通道 `0x200`、节 `0x20`、寄存器 `0x04`；`c_stage` 只有 3 bit → **最多 8 节/通道**。
11. `bypass` 切换时 `filt_dv` 的延迟会在 0 拍与 4 拍之间瞬时切换（未加保护）。
12. 本模块的 AXI 从机**支持 AW/W 独立到达**（`aw_hold`/`w_hold`），但 `pd_axil_regs.v` **不支持**——同一个工程里两种风格并存，改代码时不要互相"抄错"。

---

**本卷下一篇**：`A04_特征提取核心.md` —— 全工程最大的单文件（917 行），相位窗、峰值判决、n/I/P/Q、PRPD、事件打包都在这里。
