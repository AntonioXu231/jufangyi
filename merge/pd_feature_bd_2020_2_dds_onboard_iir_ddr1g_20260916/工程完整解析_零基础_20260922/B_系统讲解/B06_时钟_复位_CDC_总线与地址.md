# B06 时钟·复位·CDC·总线与地址（零基础系统讲解）

> 配套文档：B05《数据流三支路与存储布局》。本文只依据工程目录内代码与配置取证，结论均带 `文件:行号`；无法从源码确定的地方标 `【推测】`。
>
> 工程根目录：`F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916`

---

## 1. 时钟树

### 1.1 50 MHz 晶振 → MMCM → 两路输出

外部晶振：`pl_gclk_50` 引脚 **U18**（XDC `pd_feature_bd_timing.xdc:4-5`），50 MHz，网 `clk_in1_0_1` 连到 `clk_wiz_0/clk_in1`（`pd_feature_bd.bd` nets）。

时钟向导 `clk_wiz_0`（IP `xilinx.com:ip:clk_wiz:6.0`）关键参数（`pd_feature_bd.bd` components/clk_wiz_0）：

| 参数 | 值 | 含义 |
|---|---|---|
| `PRIM_IN_FREQ` | 50.000 | 输入时钟 50 MHz |
| `MMCM_CLKIN1_PERIOD` | 20.000 ns | 50 MHz |
| `MMCM_CLKFBOUT_MULT_F` | 26.000 | 反馈倍频 |
| `MMCM_DIVCLK_DIVIDE` | 1 | 输入分频 |
| `NUM_OUT_CLKS` | 2 | 两路输出 |
| `MMCM_CLKOUT0_DIVIDE_F` | 10.000 | clk_out1 分频 |
| `CLKOUT1_REQUESTED_OUT_FREQ` | 130.000 | clk_out1 = 130 MHz |
| `CLKOUT2_USED` | true | clk_out2 启用 |
| `MMCM_CLKOUT1_DIVIDE` | 50 | clk_out2 分频 |

**VCO 计算**：`50 MHz × 26 / 1 = 1300 MHz`。
- `clk_out1 = 1300 / 10 = 130 MHz`（PL 主时钟）
- `clk_out2 = 1300 / 50 = 26 MHz`（ADC/DDS 采样时钟）

### 1.2 每个输出驱动了哪些模块（照 BD nets 逐条）

**`clk_wiz_0_clk_out1`（130 MHz）驱动**（来自 `pd_feature_bd.bd` nets）：

| 模块端口 | 说明 |
|---|---|
| `rst_ps7_0/slowest_sync_clk` | 复位同步主时钟 |
| `axi_ic_ctrl/ACLK`、`S00_ACLK`、`M00..M03_ACLK` | AXI-Lite 互联时钟 |
| `axi_dma_0/s_axi_lite_aclk`、`m_axi_s2mm_aclk` | DMA 控制/数据时钟 |
| `axi_smc_hp0/aclk`、`axi_smc_hp1/aclk` | HP0/HP1 SmartConnect 时钟 |
| `processing_system7_0/M_AXI_GP0_ACLK`、`S_AXI_HP0_ACLK`、`S_AXI_HP1_ACLK` | PS 侧 AXI 时钟 |
| `ila_0/clk` | 调试 ILA 时钟 |
| `axi_rs_feature_ctrl_0/aclk` | 特征控制互联 |
| `sync_pulse_0/clk` | 周期边界同步 |
| `pd_filter_0/clk` | 滤波链时钟 |
| `pd_feature_0/clk` | 特征提取时钟 |
| `pd_ddr_0/clk` | 存档顶时钟 |

**`clk_wiz_0_clk_out2`（26 MHz）驱动**（来自 `pd_feature_bd.bd` nets）：

| 模块端口 | 说明 |
|---|---|
| `pd_dds_0/clk` | 板载 DDS 激励源时钟 |
| `pd_feature_0/adc_clk` | 特征提取 ADC 采样时钟 |
| `pd_ddr_0/adc_clk` | 存档顶 ADC 采样时钟 |

### 1.3 PS 的 FCLK0 只驱动诊断 ILA

`processing_system7_0_FCLK_CLK0` 网只连接 **`ila_clk_reset_diag_0/clk`**（`pd_feature_bd.bd` nets）。`ila_clk_reset_diag_0` 是用于观测 `locked`/`reset`/中断电平的诊断 ILA。

**结论**：PL 主时钟（130 MHz）源自片外 50 MHz 晶振经 MMCM，不再是 PS 的 FCLK0。`FCLK_CLK0` 在本工程里只给诊断 ILA 用，其探针采 `lock`/`reset` 电平做观测，不产生功能性的 CDC 数据传送（`pd_feature_bd_timing.xdc:18-21` 注释佐证）。

---

## 2. 三个时钟域的关系

| 时钟域 | 频率 | 驱动范围 |
|---|---|---|
| 26 MHz 域 | `clk_out2` | DDS/ADC 端口（`pd_dds_0`、`pd_feature_0/adc_clk`、`pd_ddr_0/adc_clk`） |
| 130 MHz 域 | `clk_out1` | 全部 RTL 逻辑 + 全部 AXI（互联/DMA/HP/SmartConnect/各 IP 的 `clk`） |
| PS FCLK0 域 | PS PLL 产生 | 仅 `ila_clk_reset_diag_0`（诊断 ILA） |

### 2.1 为什么需要两组 `set_clock_groups -asynchronous`

XDC（`pd_feature_bd_timing.xdc`）有两段：

```
set_clock_groups -asynchronous \
  -group [get_clocks adc_clk] \
  -group [get_clocks -of_objects [get_pins -hierarchical *clk_wiz_0*/clk_out1]]   # :14-16

set_clock_groups -asynchronous \
  -group [get_clocks clk_fpga_0] \
  -group [get_clocks -of_objects [get_pins -hierarchical *clk_wiz_0*/clk_out1]]   # :22-24
```

- 第 1 组：`adc_clk`（26 MHz 外部采样时钟，端口 `adc_clk_0`，XDC `:7`）与 `clk_out1`（130 MHz）异步。这两域之间唯一的合规跨域点是 `pd_ddr_wr_top` 里的两个独立异步 FIFO（`u_fifo48`/`u_fifo48_feature`，`pd_ddr_wr_top.v:278,297`）。
- 第 2 组：`clk_fpga_0`（PS FCLK0）与 `clk_out1`（130 MHz）异步，因为二者分别源自 PS PLL 与片外 50 MHz 晶振（`pd_feature_bd_timing.xdc:18-21`）。FCLK0 仅驱动诊断 ILA，无功能性 CDC，故只需声明异步、避免工具误判为同步路径。

**必要性**：Vivado 时序分析默认把所有时钟当"同源同步"做建立/保持检查。跨异步时钟域的路径（如 FIFO 写侧 26M / 读侧 130M）若被当成同步路径，会报海量伪路径违例甚至错误优化。用 `set_clock_groups -asynchronous` 告诉工具"这两组之间不要做跨时钟时序分析"，跨域正确性改由异步 FIFO/双同步器（带 `ASYNC_REG`）保证。

### 2.2 XDC 不能写 Tcl 的 `if`（约束取证）

`pd_feature_bd_timing.xdc:11-13` 原文注释：

```
# XDC accepts constraint commands only. Do not wrap this in Tcl conditionals:
# `if` is rejected by the XDC parser and causes the asynchronous FIFO crossings
# to be analyzed as unrelated synchronous paths.
```

**含义**：XDC 不是通用 Tcl 脚本，解析器只接受约束命令（如 `create_clock`、`set_clock_groups`）。一旦用 `if` 包裹，`if` 被拒、其后约束不生效，异步 FIFO 跨域会被**误当作同步无关路径**分析，CDC 正确性彻底失效。因此这两条 `set_clock_groups` 必须裸写、不可条件化。

---

## 3. 复位体系

### 3.1 proc_sys_reset 的两路输出

`rst_ps7_0`（`proc_sys_reset`）输入：`ext_reset_in = processing_system7_0/FCLK_RESET0_N`、`dcm_locked = clk_wiz_0/locked`（`pd_feature_bd.bd` nets）。其输出（均同步到 `clk_out1`）：

- **`peripheral_aresetn`** 驱动（来自 `pd_feature_bd.bd` nets）：
  `axi_dma_0/axi_resetn`、`axi_rs_feature_ctrl_0/aresetn`、`pd_dds_0/rst_n`、`sync_pulse_0/rst_n`、`pd_filter_0/rst_n`、`pd_feature_0/rst_n`、`pd_ddr_0/rst_n`、以及 `ila_clk_reset_diag_0/probe1`（仅观测）。
- **`interconnect_aresetn`** 驱动（来自 `pd_feature_bd.bd` nets）：
  `axi_ic_ctrl/ARESETN`、`S00_ARESETN`、`M00..M03_ARESETN`、`axi_smc_hp0/aresetn`、`axi_smc_hp1/aresetn`。

即：外设/逻辑复位走 `peripheral_aresetn`，AXI 互联结构复位走 `interconnect_aresetn`，二者分步释放，避免互联未就绪时从设备误动作。

### 3.2 工程内部的三层复位

以 `pd_feature_top.v:131-133` 为例：

```
assign core_rst_n = {(NUM_CH){rst_n}}        // ① 全局 rst_n（来自 peripheral_aresetn）
                   & {(NUM_CH){~w_gctrl[1]}}  // ② 全局软复位 GCTRL[1]
                   & ~w_sw_rst;               // ③ 每通道软复位 CTRL[1]
```

- ① **全局 `rst_n`**：片级硬复位（PS FCLK_RESET0_N → MMCM locked → peripheral_aresetn）。
- ② **全局软复位 `GCTRL[1]`**：PS 写 `pd_axil_regs` 全局寄存器 `0x1000` 的 bit1（`pd_axil_regs.v:44` `[1]全局软复位`），整体复位所有通道。
- ③ **每通道软复位 `CTRL[1]`**：PS 写各通道 `0x00` 的 bit1（`pd_axil_regs.v:15` `[1]sw_rst(W1P)`），仅复位单通道。

### 3.3 DDR 支路的 `ddr_rst_n`

`pd_ddr_wr_top.v:238`：
```
wire ddr_rst_n = rst_n & ~sw_rst;   // 低有效，含软件复位
```
- `rst_n`：130 MHz 域的全局异步复位（来自 peripheral_aresetn）。
- `sw_rst`：来自 `pd_ddr_axil` 的 DDR 软复位（`pd_ddr_axil.v:77` `o_sw_rst`，对应 `DDR_CTRL[1]`，`pd_ddr_axil.v:13`）。
- 注意：`fifo_48_cdc` 的 `rst` 是高有效异步（`fifo_rst = ~rst_n`，`pd_ddr_wr_top.v:237`），而 DataMover 是低有效同步（`ddr_rst_n` 经 `s_axi_aresetn` 送各 IP）。

### 3.4 坑：1bit 信号与多位按位运算会被零扩展

`pd_feature_top.v:128-129` 注释原文：
```
// 注意: rst_n 是 1bit, 必须显式复制成 NUM_CH 位, 否则 Verilog 会按零扩展
//       导致只有 bit0 有效(其余通道被永久复位) —— 这是一个容易踩的坑。
```
`rst_n` 只有 1 bit。若写成 `core_rst_n = rst_n & ~w_gctrl[1] & ~w_sw_rst;`，右侧运算按 1 bit 零扩展成 `NUM_CH` 位时，**只有 bit0 为 `rst_n`、其余位恒为 0（即"永久复位"）**。正确写法是 `{(NUM_CH){rst_n}}` 把 1 bit 显式复制成 `NUM_CH` 位（`:131`）。同理 `w_gctrl[1]`、`w_sw_rst`（已是 `NUM_CH` 位向量）参与按位与，结果是 `NUM_CH` 位，正确。

---

## 4. 跨时钟域（CDC）完整清单

本工程所有跨域点及其处理方式（逐一列出）：

| # | 跨域点 | 源域→目的域 | 处理方式 | 证据 |
|---|---|---|---|---|
| 1 | Path A 采样字 `u_fifo48` | ADC 26M → clk 130M | 异步 FIFO（BRAM，格雷码指针 + 双同步器 `ASYNC_REG`） | `pd_ddr_wr_top.v:278-290`；`async_fifo.v` |
| 2 | Path B 采样字 `u_fifo48_feature` | ADC 26M → clk 130M | 同上，独立 FIFO | `pd_ddr_wr_top.v:297-309` |
| 3 | ADC 域复位释放 `adc_rst_sync` | rst_n(130M) → adc_clk 域 | 2 级同步器（`ASYNC_REG="TRUE"`） | `pd_ddr_wr_top.v:243-250` |
| 4 | （备用）`pd_adc_cdc` 逐通道 FIFO | ADC → clk | `async_fifo` + CDC 分频 | `pd_adc_cdc.v:58`；**本 BD `INPUT_CDC=0`，未实例化** |
| 5 | 异步 FIFO 指针传递 | wr_clk ↔ rd_clk | 格雷码 + 两级 `ASYNC_REG` 同步器 | `async_fifo.v:55-70` |
| 6 | 中断 `xlconcat_0` → `IRQ_F2P` | 130M 各模块 → PS | 电平信号（irq 为组合 OR 的**电平保持**，非脉冲），无时序 CDC 风险 | `pd_ddr_wr_top.v:777-781`；`pd_feature_bd.bd` nets |
| 7 | `sync_pulse_0` 周期边界 | 外部 `sync_in`（异步）→ clk 130M | `sync_pulse` 同步器【推测】 | `pd_feature_bd.bd` nets；`pd_ddr_wr_top.v:50` `cycle_start` |

**确认的有效 CDC 跨域点数量 = 5**（#1、#2、#3、#5、#6；逐通道 FIFO #4 在本 BD 因 `INPUT_CDC=0` 未实例化，故不计入实际跨域；`sync_pulse` #7 为【推测】）。核心是 #1/#2 两个 48bit 异步 FIFO，其余都是复位/指针/中断层面的二级同步。

### 4.1 为什么多通道同步数据必须"先对齐再跨域"

`pd_feature_sys_top.v:121-124` 注释原文：
```
// INPUT_CDC=0 用于统一采集架构：pd_pack48 在 ADC 域完成一次四通道
// 对齐，随后由公共 48-bit FIFO 把 Path-B 送到这里。这样特征链不再对
// 四通道分别做 CDC，避免各通道 FIFO 空标志独立同步造成的样本错位。
```

**解释**：四通道共用同一采样时钟，正常 4 路 `dv` 同拍（`pd_pack48.v:47` `dv_all = &i_adc_dv`）。若对每个通道**分别**插一个独立异步 FIFO，各 FIFO 的"空"标志在 130M 域各自独立二级同步，会因亚稳态/同步延迟差导致四通道样本**错位**（某一通道早一拍/晚一拍到达）。正确做法：先在 ADC 域用 `pd_pack48` 把 4 通道拼成 1 个 48bit 字（完成"一次四通道对齐"），再只用**一个** 48bit 异步 FIFO 跨域，四通道作为一个整体跨域，自然不可能错位。这正是本工程的实际方案（`pd_ddr_wr_top` 里 Path B 也用单个 `u_fifo48_feature` 整字跨域）。

---

## 5. 异步 FIFO 的两个必须知道的结论

### 5.1 判满必须最高两位都取反

`async_fifo.v:78-92` 原文注释与代码：
```
// ⚠️ 经典陷阱：格雷码判"满"必须【最高两位都取反】，只反转最高位是错的。
wire [PTR_W-1:0] full_cmp_gray = {~rd_ptr_gray_in_wr[PTR_W-1],
                                  ~rd_ptr_gray_in_wr[PTR_W-2],
                                   rd_ptr_gray_in_wr[PTR_W-3:0]};
assign full = (wr_ptr_gray == full_cmp_gray);
```
**原因**："满"= 写指针领先读指针整整一圈（2^N）。在格雷码下，这要求**最高位与次高位都取反、其余位相等**（`:82-86`）。只反转最高位时 `full` 永远拉不高——脚本遍历 4096 种指针组合验证：错误判据命中 0/4096、正确判据命中 4096/4096（`:86-87`），即"只反转最高位 FIFO 会静默溢出"。

### 5.2 `rd_en` 到数据有效是第 2 拍（同步读）

`async_fifo.v:18-20` 注释：
```
// ⚠️ 时序契约变化：rd_data 由"组合输出"改为"同步输出"，
//    rd_en 拉高后【第 2 拍】数据才有效（多了 1 拍流水延迟）。
```
调用方必须补打拍。证据 `pd_adc_cdc.v:95-109`：
- `:99-101`：`rd_en <= ce_sample && !fifo_empty;`（第 1 拍发 `rd_en`）
- `:105-109`：`rd_en_d <= rd_en;` 再打一拍，且 `adc_dv_100 <= rd_en_d;`（第 2 拍才把 `fifo_dout` 锁存到 `adc_data_100`）
- 注释 `:96-97` 明确"`rd_en` 拉高后第 2 拍数据才有效，故用 `rd_en_d` 再打一拍对齐"。

即：同步读（BRAM 输出寄存器）使数据比 `rd_en` 晚 **2 拍** 有效，调用方须用 `rd_en_d` 对齐数据，吞吐率不受影响（读节拍间隔 = `CDC_RATIO ≥ 2` 拍，额外 1 拍流水被吸收）。

---

## 6. 总线与地址映射总表

### 6.1 AXI-Lite（PS M_AXI_GP0 → axi_ic_ctrl → 4 个从机）

来自 `pd_feature_bd.bd` addressing（PS `M_AXI_GP0` 的 `Data` 空间）：

| 从机 | 地址块 | 基址 | 范围 | 对应逻辑 |
|---|---|---|---|---|
| `pd_ddr_0/s_axi` | `SEG_pd_ddr_0_reg0` | **0x4000_0000** | 64K | DDR 环形/快照寄存器（`pd_ddr_axil`） |
| `pd_feature_0/s_axi` | `SEG_pd_feature_0_reg0` | **0x4001_0000** | 64K | 特征提取寄存器（`pd_axil_regs`） |
| `pd_filter_0/S_AXI` | `SEG_pd_filter_0_reg0` | **0x4002_0000** | 64K | 滤波链寄存器（`pd_filter_chain`） |
| `axi_dma_0/S_AXI_LITE` | `SEG_axi_dma_0_Reg` | **0x4040_0000** | 64K | AXI DMA 寄存器 |

互联：`axi_ic_ctrl` 有 `S00`（接 PS GP0）与 `M00..M03`（接上面 4 个从机），其 `ACLK`/`ARESETN` 均来自 130 MHz 域（第 1、3 节）。

### 6.2 AXI4 数据路径（写在 DDR 的总线）

来自 `pd_feature_bd.bd` addressing + interface_nets：

| HP 口 | 主设备 | 目的 |
|---|---|---|
| **HP0**（`S_AXI_HP0`） | `pd_ddr_0/m_axi_wr` + `axi_dma_0/M_AXI_S2MM` | 环形区写（Path A）+ 事件 DMA 写（Path B） |
| **HP1**（`S_AXI_HP1`） | `pd_ddr_0/m_axi_rd` + `pd_ddr_0/m_axi_cw` | 快照拷贝读环形区 + 写快照区（内存→内存环回） |

- `pd_ddr_0/m_axi_wr` → HP0（`pd_feature_bd.bd` addressing：`SEG_processing_system7_0_HP0_DDR_LOWOCM`，offset 0x0，range 1G）。
- `axi_dma_0/M_AXI_S2MM` → `axi_smc_hp0/S00_AXI` → HP0（interface_nets `axi_dma_0_M_AXI_S2MM`）。
- `pd_ddr_0/m_axi_rd`、`m_axi_cw` → HP1（`pd_feature_bd.bd` addressing）。

### 6.3 中断（`xlconcat_0` 3 路合成 → IRQ_F2P）

来自 `pd_feature_bd.bd` nets：

| 源 | → `xlconcat_0` 输入 |
|---|---|
| `pd_feature_0/irq` | `In0` |
| `axi_dma_0/s2mm_introut` | `In1` |
| `pd_ddr_0/irq` | `In2` |

`xlconcat_0/dout` → `processing_system7_0/IRQ_F2P`（3 路合成单中断线送 PS）。

---

## 7. AXI-Lite 从机的三种写法对比（本工程内部不一致）

本工程三个 AXI-Lite 从机的"AW/W 握手"实现彼此不同，必须指出。

### 7.1 `pd_axil_regs`（要求 AW/W 同时有效，不合规范）

- 证据：`pd_axil_regs.v:260` `if (~axi_awready && S_AXI_AWVALID && S_AXI_WVALID && aw_en)`，`:281` `else if (~axi_wready && S_AXI_WVALID && S_AXI_AWVALID && aw_en)`。
- 行为：`AWREADY` 与 `WREADY` 只在 **AW 与 W 同一拍同时有效** 时才拉高；`slv_wren` 要求四个信号同时有效（`:287`）。
- **风险**：标准 AXI4-Lite 允许 AW 与 W **乱序到达**（AXI 规范允许分别握手）。本写法要求二者同拍，若 PS 总线或跨bar先发 AW 后发 W（常见），会被 `aw_en` 锁死、直到 BREADY 才释放（`:262-266`），导致写超时或挂死。这是与规范相悖的脆弱写法。

### 7.2 `pd_filter_chain`（`aw_hold`/`w_hold` 缓存）

- 证据：`pd_filter_chain.v:37` `reg aw_hold, w_hold`；`:42-47` `aw_hs`/`w_hs` 独立判定；`:51-52` `AWREADY=!aw_hold && !bvalid && !cfg_valid`、`WREADY=!w_hold && ...`；`:80-81` 分别锁存 `awaddr_hold`/`wdata_hold`；`:44` `write_fire=!bvalid && (aw_hold||aw_hs) && (w_hold||w_hs)`。
- 行为：AW、W 各自独立接收并缓存（`aw_hold`/`w_hold`），任一先到先存，等两者齐了再 `write_fire`。
- **风险**：正确且规范。缺点：`cfg_valid` 期间 `AWREADY/WREADY` 都拉低（`:51-52`），若 `cfg_valid` 卡住会短暂阻塞；但 `cfg_valid` 每拍清（`:79`），无死锁。

### 7.3 `pd_ddr_axil`（`aw_done`/`w_done` 缓存）

- 证据：`pd_ddr_axil.v:180` `reg aw_done, w_done`；`:200-202` AW 到达锁 `waddr<=awaddr[11:0]`、`aw_done<=1`；`:207-210` W 到达 `w_done<=1`；`:215-219` `aw_done&&w_done` 后 `wr_fire` 写寄存器并清零。
- 行为：与 `pd_filter_chain` 思路一致——AW/W 独立缓存为 `aw_done`/`w_done`，齐了才写。`waddr` 只取页内 12 位（`awaddr[11:0]`，`:201`），即只看页内偏移。
- **风险**：正确且规范。与 `pd_axil_regs` 形成鲜明对比——同样功能，`pd_ddr_axil` 用 `aw_done/w_done` 而非"同拍有效"，更鲁棒。

**小结**：三种写法里，`pd_axil_regs` 的"同拍有效"最危险（不符合 AXI 乱序握手），`pd_filter_chain` 与 `pd_ddr_axil` 的 `hold`/`done` 缓存写法才是正确的。驱动 PS 软件（`pd_snapshot_poll.c` 等）用 `Xil_Out32` 先 AW 后 W 通常能工作，但切换到更通用的 AXI 主设备时 `pd_axil_regs` 可能失败。

---

## 8. 地址解码的坑

### 8.1 `pd_axil_regs` 用 `addr[15:12]` 做区域码 → 各区必须 4KB 对齐

`pd_axil_regs.v:5` 与 `:10-12` "设计教训"原文：
```
// !! 区域码取 addr[15:12], 因此各区必须按 4KB 边界对齐 !!
// [设计教训] 早期版本把全局区放在 0x0800, 但 0x0800 的 addr[15:12] = 0x0,
//   与通道区的区域码相同, 导致全局访问被误译为通道寄存器访问...
//   故全局区必须落在独立的 4KB 页。
```
- 区域划分：`0x0000~0x07FF` 通道区（`addr[15:12]=0`）、`0x1000~0x1FFF` 全局区（`=1`）、`0x2000~0x21FF` 计数器区（`=2`）、`0x4000~0x7FFF` PRPD 区（`=4`）。
- 坑：因为区域选择只看 `addr[15:12]`，任何区域基址都必须是 **4KB（0x1000）对齐**，否则会掉进别的区域码。早期把全局区放 `0x0800`（仍属 `addr[15:12]=0`）就是踩了这个坑。

### 8.2 `pd_ddr_axil` 用页内偏移，而 BD 映射到 `0x4000_0000`

- `pd_ddr_axil` 解码只看 `s_axi_awaddr[11:0]`（页内 12 位偏移，`:201`、`:254`、`:310`、`:314`），即它**不关心页号**，只认"自己在哪个 4KB 页内的偏移"。
- 但当前 BD 把该从机映射到 **`0x4000_0000`**（第 6.1 节 addressing：`SEG_pd_ddr_0_reg0` offset `0x40000000`）。
- 契约差异：该从机头注释（`pd_ddr_axil.v:8`）说"使用独立 4KB 页 0x2000（addr[15:12]=2），冻结组保持契约偏移 0x28/0x2C/0x30/0x34"。也就是说：
  - **契约文档（v3）** 假定它在页 `0x1000`（与 `pd_axil_regs` 全局区同页），冻结寄存器在 `0x1028/0x102C/0x1030/0x1034`；
  - **实际实现** 它在独立页 `0x2000`，而当前 BD 将该 AXI-Lite segment 映射到 PS 地址 `0x4000_0000`。
- 因此 PS 访问时，**实际地址 = 0x40000000 + 页内偏移**（如 `FREEZE_CTRL` 实际 = `0x40000000 + 0x34 = 0x40000034`），而契约里写的是 `0x1034`。当前 `pd_hw_map.h` 与 `pd_snapshot_poll.c` 应使用 `PD_DDR_BASE(=0x40000000) + 偏移`；若仍使用旧 BSP/XSA，必须先确认其生成宏是否匹配当前 BD。这是"契约地址（逻辑页内偏移）"与"实际地址（BD 映射后的物理页基址+偏移）"的典型差异，维护时必须以当前 BD addressing 和活动 `xparameters.h` 为准。

---

## 9. 自测 8 问

1. 片外 50 MHz 晶振（U18）经 `clk_wiz_0` 后输出哪两路时钟？各自频率与驱动范围是什么？
2. 为什么 PS 的 `FCLK_CLK0` 在本工程里"不再是 PL 主时钟"？它实际驱动了谁？
3. XDC 里两组 `set_clock_groups -asynchronous` 分别声明了哪两个域异步？为什么必须这样写？
4. XDC 为什么"不能写 Tcl 的 `if`"？如果写了会怎样？（证据 `文件:行号`）
5. `pd_feature_top.v:131-133` 的三层复位分别是什么？为什么 `rst_n` 必须写成 `{(NUM_CH){rst_n}}`？
6. 异步 FIFO 判"满"为什么要把格雷码最高**两位**都取反？只反转最高位会怎样？（证据 `文件:行号`）
7. `async_fifo` 改为同步读后，`rd_en` 到数据有效是几拍？`pd_adc_cdc` 如何补这拍？
8. 本工程三个 AXI-Lite 从机（`pd_axil_regs`/`pd_filter_chain`/`pd_ddr_axil`）的 AW/W 握手写法有何不同？哪种不合 AXI 规范、风险最大？
