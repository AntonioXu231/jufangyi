// =============================================================================
// pd_defines.vh  --  局部放电(DP)特征提取 IP 公共参数定义
// 器件: xc7z020clg484-2   工具: Vivado 2020.2
// =============================================================================
`ifndef _PD_DEFINES_VH_
`define _PD_DEFINES_VH_

// ---- 数据通路位宽 ----
`define PD_ADC_W        12          // ADS805E 12bit 并行输出
`define PD_PH_W         12          // 相位窗索引位宽(支持最大 2048 窗/周期)
`define PD_TS_W         24          // 时间戳位宽(单位: ADC 样本周期 50ns, 溢出 ~0.84s)
`define PD_EV_W         64          // 事件包位宽(8 字节, 与方案书 6.4 一致)

// ---- 峰值提取模式 (CFG_MODE[1:0]) ----
// 0: ABSMAX  窗口内绝对值最大者 (现有 STM32 算法最可能形态, 默认)
// 1: PP      窗口内峰峰值 (max - min)
// 2: POSNEG  正峰/负峰各输出一个事件 (改进版, 缓解 F2 段内合并)
`define PD_MODE_ABSMAX  2'd0
`define PD_MODE_PP      2'd1
`define PD_MODE_POSNEG  2'd2

// ---- 事件包类型 (EV[63:56]) ----
`define PD_EV_PEAK      8'h00       // 峰值事件
`define PD_EV_CYCLE     8'h01       // 周期统计包(帧尾, tlast=1)

// ---- 极性 ----
`define PD_POL_POS      1'b0
`define PD_POL_NEG      1'b1

// ---- 峰值事件包内 phase 字段位宽 ----
// 12: 与 B 侧 RTL 一致 (支持相位窗数 N≤2048, 已定案 2026-09-08)
// 10: A 接口文档 §6.2 曾定义 (窗号 0~1023); 若需切回只改本行。
// 位宽联动: EVT_SEQ_W = 37 - PH_FIELD_W, 保证事件包总宽恒为 64bit
//   PH_FIELD_W=10 -> evt_seq 27bit (8+16+10+1+2+27=64)
//   PH_FIELD_W=12 -> evt_seq 25bit (8+16+12+1+2+25=64)
`define PD_PH_FIELD_W   12

// ---- 默认配置值 ----
// 相位增量 = round(2^32 / N), N=1024 -> 4194304 = 0x00400000
`define PD_PH_INC_1024  32'h0040_0000
// 默认同步周期 ADC 样本数: 20MSPS / 50Hz = 400000 (首次同步前的兜底值)
`define PD_M_DEFAULT    32'd400000

`endif
