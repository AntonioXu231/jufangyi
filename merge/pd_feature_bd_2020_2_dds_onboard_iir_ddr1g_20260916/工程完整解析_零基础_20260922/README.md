# 工程完整解析 · 零基础版

> 分析对象：`F:\xinya\v5\merge\pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916`
> 分析范围：**仅该目录内的代码与配置**（RTL / 头文件 / XDC / Block Design / IP 配置 / PS 侧 C 程序 / 仿真模型）。
> 建立日期：2026-09-22。

---

## 0. 一句话说清这个工程在干什么

用一片 Zynq-7020（PL + ARM 双核 SoC），把 **4 路 12 bit、26 MSPS 的 ADC 采样**同时做三件事：

1. **存档**：把**原始样点**无损写进 DDR 的环形缓冲区，并按 50 Hz 工频周期切出一段完整波形快照，搬进"四槽"快照区；
2. **判决**：实时提取每个相位窗的峰值/极性，用阈值判出放电脉冲，形成 PRPD 图谱与 n/I/P/Q 统计，并以 8 字节事件包通过 AXI-Stream 送出；
3. **分析**：ARM（PS）把快照取回自己的 DDR，做去直流 + Hann 窗 + 1024 点定点 FFT，再通过网口把结果给 PC。

**当前工程处于"片上 DDS 自激励上板测试"状态**：`pd_dds_0` 是 PL 内用 Verilog 写的四通道三角波 + 放电脉冲发生器，通过 Block Design 直接顶替真实 ADC，用来在**没有模拟前端**的情况下验证整条数字通路。

---

## 1. 交付物结构

```
工程完整解析_零基础_20260922/
├── README.md                      ← 本文件：索引 + 阅读路线 + 分析口径
├── A_逐行注释/                     ← 第一卷：逐行中文注释（对着源码看）  14 篇 / 9,683 行
│   ├── A01_公共头文件与全局约定.md                pd_defines.vh / pd_ddr_defines.vh / pd_align24_check.v / XDC
│   ├── A02_采集链_同步_CDC_激励源.md              adc_capture / async_fifo / pd_adc_cdc / pd_sync_pulse / pd_dds_adc_source
│   ├── A03_滤波链_IIR带通.md                      pd_filter_chain / pd_iir_biquad
│   ├── A04a_特征提取核心_上_窗口与量化.md           pd_feature_core（第 1–578 行）
│   ├── A04b_特征提取核心_下_状态机与帧统计.md       pd_feature_core（第 580–918 行）
│   ├── A05_PRPD与数据打包.md                      pd_prpd_ram / pd_pack48 / pd_pack192
│   ├── A06_事件FIFO与轮询仲裁.md                  pd_axis_fifo / pd_axis_arb
│   ├── A07_特征寄存器与IP顶层.md                  pd_axil_regs / pd_feature_top / pd_feature_sys_top
│   ├── A08_DDR环形写入.md                         pd_ddr_ring_wr（信用机制 / 回绕 / 冻结锁存）
│   ├── A09_快照子系统_拷贝_槽管理_触发.md          pd_ddr_snap_copy / pd_ddr_slot_mgr / pd_snapshot_trigger
│   ├── A10_DDR顶层_寄存器与BD适配.md              pd_ddr_wr_top / pd_ddr_axil / pd_ddr_bd_adapter
│   ├── A11_仿真测试平台.md                        4 个 testbench
│   ├── A12_PS侧服务_采集核心_解包_频谱_TCP.md      ps_service/*（采集核心 / 解包 / 定点 FFT / TCP）+ pd_snapshot_poll.c
│   └── A15_PS侧_旧版采集服务与调试探针逐行注释.md   sw/*.c 全部历史版本（10 个 C + 5 份说明文档）
└── B_系统讲解/                     ← 第二卷：系统讲解（**先读这一卷，再读 A 卷**）  9 篇 / 2,836 行
    ├── B01_零基础起点_这个工程到底是什么.md
    ├── B02_PL基础_零基础必备概念.md                 12 个必备硬件概念
    ├── B03_PS基础_从概念到交互.md                   从"什么是 Zynq"到 cache/lwIP 完整铺垫
    ├── B04_系统架构与模块划分.md                     16 个 BD 组件 + 21 个自研模块 + 层次树
    ├── B05_数据流三支路与存储布局.md                 三条支路逐级拆解 + 1 GB DDR 统一分配图
    ├── B06_时钟_复位_CDC_总线与地址.md               3 个时钟域 / 5 个 CDC 点 / 地址映射总表
    ├── B07_关键知识点深入.md                         18 个核心技术点（每个含 是什么/为什么/本工程例/后果）
    ├── B08_复用手册_构建_仿真_上板与改造.md          构建流程 + 参数改动表 + 3 条改码纪律 + 复用清单
    └── B09_风险_已知问题与未闭合项.md                分级风险清单 + 优先级行动清单 + 上板确认清单
```

**合计 24 篇、12,661 行、约 932 KB。**

> 📌 **命名说明**：A 卷原本计划的 `A13_PS核心` 与 `A14_PS_TCP服务` 两篇**已合并为 `A12`**（因为两者共用同一套 `pd_hw_map.h` 契约，拆开会产生大量重复）；PS 侧历史版本与探针编为 `A15`。**编号不连续是有意的（A12 → A15）**，不是文件缺失。

---

## 2. 阅读路线（建议按顺序）

| 阶段 | 读什么 | 你会得到什么 |
|---|---|---|
| **1. 建立直觉** | `B01` → `B02` | 知道 PL 和 PS 各是什么、这个工程为什么这样切、12 个必备硬件概念 |
| **2. 看全局** | `B03` → `B04` → `B05` → `B06` | 能画出 BD 框图、说清三条数据支路、记住时钟/CDC/地址 |
| **3. 啃 PL 代码** | `A01` → `A02` → `A03` → `A04a` → `A04b` → `A05` → `A06` → `A07` | 把采集链、滤波链、特征核、打包、事件汇聚全部搞懂 |
| **4. 啃 DDR 支路** | `A08` → `A09` → `A10` | 环形写入、冻结快照、四槽管理、三条拷贝路径 |
| **5. 进 PS** | `A12` → `A15` | 看懂 ARM 侧如何读寄存器、搬 DMA、做 FFT、跑 TCP |
| **6. 学会验证** | `A11` → `B08` | 会跑仿真、会判通过、会构建/上板 |
| **7. 复盘** | `B07` → `B09` | 掌握 18 个硬知识点；知道工程还有哪些坑与未闭合项 |

⭐ **两个实用读法**：
1. **每篇先看末尾的"易错点小结"（如果有），再回头读正文** —— 易错点往往就是这一篇真正想让你记住的东西；
2. **A 卷每篇开头都有一张"覆盖文件表"**，标了行数与角色。**对着源码读，不要脱离源码读。**

---

## 3. 分析口径与免责声明

1. **只依据本目录内的代码与配置**（RTL / 头文件 / XDC / Block Design / IP 配置 / PS 侧 C 程序 / 仿真模型）。本目录内的 `*.md`（如 `最终定稿_…md`、`PL_PS验证判据与执行清单_…md`）属于"工程自带的说明"，只在需要解释设计意图时引用，**不作为功能事实的来源** —— **功能事实一律以 RTL / `.bd` / C 代码为准。**
2. **所有结论都带"文件:行号"**。凡是没有行号支撑的判断，都标为 `【推测】` 或 `【待验证】`。
3. **不修改任何源码**。本套文档是纯分析成果，放在被分析工程树内的一个独立子目录里，**不参与综合与实现**。
4. ⭐ **本套文档最重要的一条阅读纪律**：本工程源码里的注释分两类 —— **"描述现状"** 与 **"描述反例 / 曾经踩过的坑"**。后者数量很多（`pd_axil_regs.v:10-12`、`pd_axis_fifo.v:5-34`、`pd_prpd_ram.v:9-12`、`async_fifo.v:8-20`、`pd_feature_core.v:336-338` 与 `:448-456`、`pd_ddr_ring_wr.v:256-259`、`pd_ddr_snap_copy.v:174-176`、`pd_ddr_wr_top.v:154-155`、`pd_feature_top.v:128-129`）。
   **把它们误读成"现状"会产生大量假缺陷** —— 本次分析已实际发生两次，已在 `B09` 里更正（R-5 与 Y-7 两条已撤销并移入 🟢）。**判据：出现"注意 / 若…会… / 这是一个容易踩的坑 / 勿改回"等措辞的，基本都是反例说明。**
5. 工具产物（`.gen/`、`.cache/`、`.runs/`、`.hw/`、`*.log`）**不属于"代码"**，不逐行分析；只在整个说明需要（如器件型号、生成时钟频率、时序/资源数字）时作为旁证引用，并**注明来源报告**。

---

## 4. 快速速查（先记住这 10 个数，读文档会顺很多）

| 项 | 值 | 出处 |
|---|---|---|
| 器件 | `xc7z020clg400-2` | `pd_feature_bd_2020_2.xpr` |
| PL 主时钟 | **130 MHz**（`clk_wiz_0/clk_out1`） | `.bd` 中 `clk_wiz_0` 参数：50 MHz × 26 ÷ 10 |
| ADC 采样时钟 | **26 MHz**（`clk_wiz_0/clk_out2`） | 同上：50 MHz × 26 ÷ 50 |
| 单采样时刻 | 4 通道 × 12 bit = **48 bit = 6 字节** | `pd_ddr_defines.vh:21-23` |
| 一块（block） | 4 个 48 bit 字 = **192 bit = 24 字节** = 3 个 64 bit beat | `pd_ddr_defines.vh:24-29` |
| 一次 DDR 突发 | 64 块 = **1536 字节** | `pd_ddr_defines.vh:148-149` |
| 一份快照 | **3,120,000 字节** = 130,000 块 = 520,000 采样点 ≈ 一个 50 Hz 周期 | `pd_ddr_defines.vh:122-124` |
| 环形区 | `0x1000_2000` ~ `0x1800_0000`，128 MiB − 8 KiB | `pd_ddr_defines.vh:80-81` |
| 四槽快照区 | `0x2000_1000` 起，4 槽 × 12 MiB，总跨度 48 MiB | `pd_ddr_defines.vh:131-139` |
| AXI-Lite 地址 | feature `0x4000_0000` / ddr `0x4001_0000` / filter `0x4002_0000` / dma `0x4040_0000` | `.bd` `addressing` 段 |
| **当前时序** | ⭐ **WNS 0.000 / TNS 0.000 / 失配端点 0（总端点 98513）；WHS +0.017 / THS 0.000**；报告结论 `All user specified timing constraints are met.` | `pd_feature_bd_2020_2.runs/impl_1/pd_feature_bd_wrapper_timing_summary_routed.rpt:131,134`（2026-09-21 19:20 的 routed 报告） |
| **当前资源** | Slice LUT **27,965 / 53,200 = 52.57%**；Slice Registers **33,524 / 106,400 = 31.51%**；Block RAM Tile **32.5 / 140 = 23.21%**；DSP **28 / 220 = 12.73%** | `…/impl_1/pd_feature_bd_wrapper_utilization_placed.rpt:34,39,104,119` |
| **数据路径** | HP0 ← `pd_ddr_0/m_axi_wr` + `axi_dma_0/M_AXI_S2MM`；HP1 ← `pd_ddr_0/m_axi_rd` + `pd_ddr_0/m_axi_cw` | `.bd` `interface_nets` 段 |
| **中断** | `xlconcat_0` 把 3 路合成一路给 PS：`In0 = pd_feature_0/irq`、`In1 = axi_dma_0/s2mm_introut`、`In2 = pd_ddr_0/irq` → `IRQ_F2P` | `.bd` `nets` 段 |

⚠️ ⭐ **关于时序数字的读法（很重要）**：
- `WNS = 0.000` 且 `TNS Failing Endpoints = 0` 表示 **"约束被满足"，但"余量恰好为 0"**；
- ⭐ **不要把它写成"时序收敛"** —— 余量 0 意味着**任何微小扰动（温度、电压、代码/P&R 扰动）都可能让 WNS 变负**（`B09` Y-12 已列为需关注项）；
- 另注意：XDC 里那条 `create_clock ... [get_ports adc_clk_0]`（`pd_feature_bd_timing.xdc:7`）**在当前顶层无对应端口，实际空转**（`B09` Y-5）→ **所以"时序满足"的结论不覆盖真实的 ADC 时钟路径**。

---

## 5. 一页纸框图（详细版见 B04 / B05）

```
                pl_gclk_50 (50 MHz, U18)
                        │
                   clk_wiz_0 (MMCM)
          ┌─────────────┴─────────────┐
     clk_out1 = 130 MHz          clk_out2 = 26 MHz
   （全部 RTL / AXI / DDR）    （DDS 激励 + adc_clk 端口）
          │                            │
          │                    ┌───────┴────────┐
          │                    │   pd_dds_0     │ 片上四通道三角波+放电脉冲
          │                    └───────┬────────┘
          │                     adc_data[47:0] / adc_dv[3:0]
          │                            │
          │                    ┌───────┴──────────────────────────────┐
          │                    │  pd_ddr_0 (pd_ddr_bd_adapter)        │
          │                    │   pd_pack48 → 2×fifo_48_cdc(异步FIFO) │
          │                    │   ├─ Path A → pack192 → axis_data_fifo│
          │                    │   │        → dm_wr(DataMover S2MM)    │
          │                    │   │        → HP0 → DDR3 环形区         │
          │                    │   └─ Path B → o_feat_data/o_feat_dv   │
          │                    └───────────────────┬──────────────────┘
          │                                        │
          │                              ┌─────────┴──────────┐
          │                              │ pd_filter_0 (IIR)  │ 默认全旁路
          │                              └─────────┬──────────┘
          │                                        │
          │                    ┌───────────────────┴────────────────┐
          │                    │ pd_feature_0 (pd_feature_sys_top)  │
          │                    │  4×[pd_feature_core + prpd_ram +   │
          │                    │     axis_fifo] → axis_arb          │
          │                    │  AXI-Lite 寄存器文件                │
          │                    └───────────┬────────────────────────┘
          │                                │ m_axis (8B 事件包)
          │                    ┌───────────┴──────────┐
          │                    │  axi_dma_0 (S2MM)    │──► HP0 → PS DDR
          │                    │  ila_0 (AXI4S 探针)  │
          │                    └──────────────────────┘
          │
   processing_system7_0 ── M_AXI_GP0 → axi_ic_ctrl → 4 个 AXI-Lite 从机
                        ── S_AXI_HP0 ← pd_ddr_0/m_axi_wr + axi_dma_0
                        ── S_AXI_HP1 ← pd_ddr_0/m_axi_rd + m_axi_cw
                        ── IRQ_F2P ← xlconcat(feature_irq, dma_irq, ddr_irq)
```
