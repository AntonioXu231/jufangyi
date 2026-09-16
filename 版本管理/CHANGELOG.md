# 变更记录

格式遵循“版本号 + 日期 + 类型 + 范围 + 验证证据”。未验证的工作必须明确标为“未验证”，不得写成已完成。

## [Unreleased]

### 计划中

- 暂无。

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
