# 变更记录

格式遵循“版本号 + 日期 + 类型 + 范围 + 验证证据”。未验证的工作必须明确标为“未验证”，不得写成已完成。

## [Unreleased]

### 计划中

- 暴露 `o_last_btt` / `o_sts_tdata`，或把已存在但未接线的 `dbg_ddr` 引入 AXI4-Lite 读回，以区分 `ring_err` 与 `ring_ovf`。
- 顶层快照闭环仿真、DMA/TREADY 门禁、滤波旁路态与滤波态 A/B 标定、PS FFT 与频谱服务、65 MSPS CIC 抽取架构。

## [v1.6.0] - 2026-09-20

### 移除四路 PL FFT、P1 滤波降载与事件自动快照触发

- 按最终定稿的架构裁决移除 `v1.5.0` 引入的四路 1024 点 PL FFT 实验支路（`pd_fft_input_adapter_4ch.v`、`pd_fft_bin_monitor_4ch.v`、`xfft_ch0_*` IP 及专用 testbench 与波形集）；FFT 固定由 PS 从 DDR 原始快照执行，分析支路不得对主采集链施加反压。
- 对 `pd_filter_0` 执行 P1 降载：`N_BP` 2→1、`N_NT` 6→0。综合 DSP 由 84 降为 28；实现后 Slice 由 `95.08%` 降至 `73.34%`，余量恢复约 22 个百分点。
- 在 `pd_ddr_0` 中加入事件自动快照触发：`pd_feature_0` 的 `event_accept` 驱动冻结与四槽复制；新增 `0x5C SNAP_TRIG_CTRL` 与 `0xA0 SNAP_TRIG_DROPS`，位于既有地址表末尾之后，既有偏移与手动 `SNAP_START` 语义不变。
- 新增 PS 侧冒烟测试 `pd_snapshot_poll.c`（轮询方式验证 PL/DDR/AXI-Lite 契约，并把 `DDR_STATUS[5]` 纳入错误判据）与 `pd_filter_apply.c`；新增 IIR 系数文件与 MATLAB 生成脚本，含 16.25 MSPS / 65 MSPS 预研系数。
- 数据流定案：DDR 保存原始 ADC 样点，`pd_filter_0 → pd_feature_0` 为实时判决支路，PL 保留实时 PRPD 作为产品主路径，PS PRPD 仅用于离线校核。

### 验证状态

- 行为仿真：`tb_pd_snapshot_trigger` 与 `tb_pd_filter_chain` 分别输出 `TB_PD_SNAPSHOT_TRIGGER_PASS`、`TB_PD_FILTER_CHAIN_PASS`。
- 综合：通过。Slice LUT 26,048、DSP 28、RAMB36 22、RAMB18 9；零 ERROR、零 CRITICAL WARNING。
- 实现：已完成，**但时序未收敛**。`WNS=-0.054 ns`、`TNS=-0.491 ns`（19 / 91,518 端点，全部位于 `clk_out1` 129.994 MHz 域，关键路径为 `pd_ddr_0` 环形写的写偏移到剩余量运算链）；`WHS=+0.036 ns`、`THS=0.000 ns`；`Place 30-487` = 0；布线错误 0。**当前 bitstream/XSA 含 `dbg_hub`/ILA（`timing_summary_routed.rpt` 中 1,311 处引用），仅可作调试版本，不得用于发布。**
- 上板：快照链路**通过**，`DDR_STATUS[5]` **已解释为一次性启动瞬态**。
  - 快照链路：`SNAPSHOT slot=0 base=0x20001000 len=3120000 seq=1 flags=0x1`；`SLOT_STATUS=0x00080001`（slot0 valid + `snapshot_ready`）；`SNAPSHOT_DATA first=0x118007FC last=0x81A7F67F`（非平凡）。
  - 环形写通路：`ΔWR_BYTES / ΔCMD == 1536` 精确等于 `DDR_BURST_BYTES`，两段区间均复现；`PTR` 增量与 `WR_BYTES` 增量逐段相等。
  - `DDR_STATUS[5]`：经粘滞清零复测确认为**一次性启动瞬态**（`PROGRESS[CLEARED] STS=0x00000001`，bit5=0；清零后约 45 ms 不复现）。根因是 `ring_ovf`（上游溢出）：`up_ovf`（`pd_ddr_wr_top.v:413`）与 `beat_en`（`:341`）均不受 `acq_en` 门控，采集关闭期间上游持续喂数、信用堆到 `AVAIL=4144`（≥`OVF_THRESH` 3072）；而 `pd_ddr_ring_wr.v:428` 的 `!i_acq_en` 优先分支把 `o_err` 强制清零，故 `acq_en` 为 0 时读不到，`acq_en=1` 那一拍立即置位。建议修法（本版本未实施）：用 `acq_en` 门控 `i_beat_en`/`up_ovf`，或在 `acq_en` 上升沿清 `avail_bytes`。
  - 四槽轮转与槽满拒绝：**通过**。逐槽加锁且不释放，迫使分配器走遍四槽：slot `0→1→2→3`，基数 `0x20001000` / `0x20C01000` / `0x21801000` / `0x22401000`（相邻间隔均为 12 MiB），`seq` 1/2/3/4，`len` 均 3,120,000；第 5 次请求被拒（`SLOT_STATUS[12] full=1`，`SLOT_DROPS 0→2`）；四槽释放均无 `cmd_err`。
  - 快照数据**内容**正确性：**通过**。全槽 520,000 点统计：四通道 `min` 精确等于 `2032/2040/2006/2046`（三角带下界），超出三角带上界的采样数精确等于 4/通道（每通道恰好一个 4 样点脉冲），`max` 均落在期望区间。另取前 256 个采样点 dump，经 256 候选起始相位搜索得**唯一解** `n0=60`（第二轮 `n0=59`）且 **32/32 逐位全中**。字节序、通道位分配、块内 `W0..W3` 时间顺序、基线与 shift 全部确证，链路端到端无损。
- 顶层快照闭环仿真、DMA S2MM 与 `m_axis` TREADY 门禁、滤波旁路态与滤波态 A/B 标定、PS FFT 与频谱服务、四槽长期轮转、GIC 中断服务、65 MSPS CIC 抽取架构、真实 AD9226 与上位机：均**未执行**；不得据此版本声明上述项目已经验证。

## [v1.5.0] - 2026-09-17

### 四路 FFT 实验集成与 PL 资源评估归档

- 在 `pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916` 中加入四路 1024 点定点 FFT 实验支路，包括 FFT 输入适配器、四个 XFFT IP、可编程目标 bin 幅值监测及专用行为仿真。
- 新增 `tb_pd_fft_chain`，以四路受控正弦输入检查 AXI-Stream 握手、1024 点帧边界、`TLAST`、bin 序号、帧计数和目标 bin 幅值。
- 归档 PL 资源不足分析及软硬件划分结论：滤波、FFT 与选频支路不参与 PD 实时触发，后续将以 `v1.4.0` 为基线迁移至 PS 快照后处理。
- 保留并归档 DDR 自动快照控制面的 `snapshot_ready` 状态改进；既有寄存器地址与手动快照语义不变。

### 验证状态

- 行为仿真：`tb_pd_fft_chain` 输出 `FFT_CHAIN_TEST_PASS frames=3/3/3/3 mag=256/256/257/257`。
- 综合：完成；综合资源报告 Slice LUT 为 48,712 / 53,200（91.56%）。
- 实现：失败。`Place 30-487` 报告当前四路 FFT 实验结构无法在 xc7z020clg400-2 放置；本版本仅作可复现的实验归档，不能用于 bitstream 或上板。

## [v1.4.0] - 2026-09-16

### DDR 四槽自动快照管理

- 在 `pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916` 工程中加入 `pd_ddr_slot_mgr`：为冻结后的环形 DDR 数据分配四个 12 MiB 快照槽，并执行 `FREE -> RESERVED -> COPYING -> VALID -> LOCKED -> FREE` 生命周期。
- 保持既有 `pd_ddr_snap_copy` 与单个 `dm_cp` DataMover 实例；自动快照与原有手动 `SNAP_START` 共用同一复制引擎，且手动启动优先。
- 扩展 `pd_ddr_axil` 的自动快照使能、槽锁定/释放、状态、序号、丢弃计数及每槽描述符寄存器；不改变既有 AXI4-Lite 地址与旧手动快照语义。
- 使用 24 字节对齐的槽基址 `0x2000_1000` 起始，避免 192-bit 数据块跨槽边界；移除槽管理器中的直接 `%24` 余数运算，改为复用平衡的 `pd_align24_check`，修复该控制路径的时序热点。

### 验证状态

- 行为仿真：Vivado 2020.2 下 `tb_pd_ddr_slot_mgr` 输出 `TB_PD_DDR_SLOT_MGR_PASS seq=5 drops=2`；覆盖槽分配、锁定/释放、满槽拒绝和超长请求拒绝。
- 综合、实现：全工程后布局物理优化报告 WNS `+0.007 ns`、TNS `0.000 ns`、WHS `+0.036 ns`、THS `0.000 ns`。
- 自动快照控制面端到端仿真、bitstream、XSA、PS 驱动和上板回归：未执行；不得据此版本声明上述项目已经验证。
- 仍存在 Clock Wizard 输入端与 PS FCLK 的重复 primary-clock 方法学告警，已定位，尚未在本版本处理。

## [v1.3.0] - 2026-09-16

### PS DDR 1 GB 可寻址映射备份

- 新建完整工程快照 `pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916`，保留已有 DDS、ILA、IIR、`pd_ddr_0` 与 AXI DMA 内容。
- 将 Zynq PS7 DDR 可见范围从 `0x0010_0000 - 0x1FFF_FFFF` 扩展为 `0x0010_0000 - 0x3FFF_FFFF`；DDR3 颗粒型号保持 `MT41K256M16 RE-125`，单颗颗粒位宽保持 16 Bit，PS 总线仍为 DQ=32、DQS=4。
- 将 `pd_ddr_0` 的 HP0/HP1 三条恢复映射脚本由 512 MB 同步为 1 GB，防止后续恢复 BD 时缩回旧范围。

### 验证状态

- Block Design：已在 Vivado 2020.2 中保存，且 PS7 属性读回为 `PCW_DDR_RAM_BASEADDR=0x00100000`、`PCW_DDR_RAM_HIGHADDR=0x3FFFFFFF`；Address Editor 中 AXI DMA 与 `pd_ddr_0` 的 HP0/HP1 映射已手动调整为 1 GB。
- 综合、实现、bitstream、XSA 和上板：本次地址映射变更后均未执行；不得将此前版本的时序或上板结果作为本版本验证结论。

## [v1.2.0] - 2026-09-16

### IIR 陷波滤波集成与控制路径时序收敛

- 在新快照 `pd_feature_bd_2020_2_dds_onboard_iir_20260916` 中保存四通道、每通道 2 个带通 + 6 个陷波二阶 IIR 的实现；缺省为全局旁路和四通道旁路，保持既有 DDS → DDR → 特征提取 → DMA 数据行为。
- 新增 `pd_filter_0` AXI4-Lite 从设备（`0x4002_0000`），在 `pd_ddr_0` 与 `pd_feature_0` 之间接入滤波链；控制路径采用独立 AW/W 握手、WSTRB 写掩码和系数流水寄存器。
- 在特征提取控制口前加入官方 AXI Register Slice，消除控制读返回关键路径，使最终实现时序收敛。

### 验证状态

- 行为仿真：Vivado 2020.2 下 `tb_pd_filter_chain` 输出 `TB_PD_FILTER_CHAIN_PASS`；覆盖默认旁路、独立 AW/W 次序、读回与 WSTRB。
- OOC 综合：IIR 滤波核在 130 MHz 约束下 WNS `+0.976 ns`、TNS `0.000 ns`，使用 64 DSP。
- 全工程实现：后布局物理优化报告 WNS `0.000 ns`、TNS `0.000 ns`、WHS `+0.017 ns`、THS `0.000 ns`；该版本尚未重新生成 bitstream、导出新 XSA 或完成滤波上板回归。

## [v1.1.0] - 2026-09-15

### 板内 DDS、ILA 与变长 DMA 包接收验证

- 在 `pd_feature_bd_2020_2_dds_onboard_20260914` 快照中保存板内 DDS 测试、130 MHz AXI-Stream ILA 观测、XSA 和相关脚本。
- 加入 PS 侧 `dma_s2mm_probe.c`：以 `TLAST` 分包、65528 B 最大 BTT 和非缓存 DDR 缓冲区持续接收 S2MM 数据，并记录实际包长分布。
- 保留本轮 ILA/DMA 调试交接记录，说明固定 128 B BTT 的历史失败与修复后的验证边界。

### 验证状态

- 上板：连续完成超过 72,000 个变长包，`DMASR=0x00001002`，无 DMA 错误、无零长度包、无超过 BTT 的包；实际包长为 8 B 至 8224 B。
- ILA：反复捕获 `TREADY==1`，并观察到 `type=0x00/TLAST=0` 的峰值事件和 `type=0x01/TLAST=1` 的周期统计包。
- 仿真、综合与实现：本次发布未重新运行；沿用已生成的、已上板使用的 bitstream/XSA，详细边界见变更说明。

## [v1.0.0-baseline] - 2026-09-14

### 基线导入

- 导入 Zynq-7020 局放采集、特征提取、DDR 与 AXI 相关 RTL、约束、仿真、Block Design 和 IP 配置。
- 导入接口契约与项目学习路线资料。

### 验证状态

- 历史工程中包含综合/实现与 DDS 仿真产物；它们是已有记录，并非本次重新执行的验证结果。
