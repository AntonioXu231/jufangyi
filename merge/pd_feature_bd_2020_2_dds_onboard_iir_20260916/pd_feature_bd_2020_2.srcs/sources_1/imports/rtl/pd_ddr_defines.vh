// =============================================================================
// pd_ddr_defines.vh  --  局放检测仪 DDR 环形缓存 / 冻结快照 公共定义
// -----------------------------------------------------------------------------
// 依据文件（唯一接口基准，改动须先改文档再动代码）：
//   [1] 《接口契约 v1.0》         §1 全局参数  §3 IF-3 PL<->DDR 原始数据
//   [2] 《开发目标.docx》         §2.2 数据流  §2.3.1 PL 端模块
//   [3] PG022  AXI DataMover     命令字 / 状态字位域
//
// 关键数字速查（本文件全部常量的推导依据）：
//   当前验证：4 通道 x 12bit x 26MSPS = 48bit x 26MHz = 156 MB/s 原始码流
//   192bit 块 = 4 个 48bit 字 = 24 Byte  （契约 §3.2，100% 利用率无填充）
//   AXI 位宽 64bit = 8 Byte   -> 1 块 = 3 个 AXI beat
//   130MHz x 8B = 1040 MB/s 总线能力 -> 占用率 15%，余量充足
// =============================================================================
`ifndef _PD_DDR_DEFINES_VH_
`define _PD_DDR_DEFINES_VH_

// -----------------------------------------------------------------------------
// 1) 数据通路位宽（契约 §3.1 / §3.2）
// -----------------------------------------------------------------------------
`define PD_CH_NUM        4                  // 通道数
`define PD_ADC_W         12                 // AD9226 分辨率
`define PD_SAMPLE_W      (`PD_CH_NUM * `PD_ADC_W)   // 48 : 一个采样时刻的 4 通道样本字
`define PD_WORD_PER_BLK  4                  // 4 个 48bit 字 -> 1 个块
`define PD_BLK_W         (`PD_SAMPLE_W * `PD_WORD_PER_BLK)  // 192
`define PD_BLK_BYTES     (`PD_BLK_W / 8)                   // 24
`define PD_AXI_DW        64                 // AXI / AXI-Stream 数据位宽
`define PD_AXI_BYTES     (`PD_AXI_DW / 8)   // 8
`define PD_BEAT_PER_BLK  (`PD_BLK_W / `PD_AXI_DW)          // 3 : 1 块 = 3 个 beat

// 48bit 样本字内的通道排布（契约 §3.1，ch0 在最低位）：
//   [47:36] ch3   [35:24] ch2   [23:12] ch1   [11:0] ch0
`define PD_CH0_FIELD     11:0
`define PD_CH1_FIELD     23:12
`define PD_CH2_FIELD     35:24
`define PD_CH3_FIELD     47:36

// -----------------------------------------------------------------------------
// 2) AXI DataMover 命令字位域（PG022，32bit 地址时命令宽度 = 72bit）
// -----------------------------------------------------------------------------
//   [71:68] RSVD(4)   [67:64] TAG(4)   [63:32] ADDR(32)
//   [31]    DRR       [30]    EOF      [29:24] DSA(6)
//   [23]    TYPE      [22:0]  BTT(23)
// -----------------------------------------------------------------------------
`define DM_CMD_W         72
`define DM_BTT_LSB       0
`define DM_BTT_W         23                 // BTT[22:0]，取值 1 .. 8388607
`define DM_TYPE_BIT      23                 // 1 = INCR（地址递增），0 = FIXED
`define DM_DSA_LSB       24
`define DM_DSA_W         6                  // DSA[29:24]，关闭 DRE 时填 0
`define DM_EOF_BIT       30                 // 末拍产生 TLAST
`define DM_DRR_BIT       31                 // DRE 重对齐请求
`define DM_ADDR_LSB      32
`define DM_ADDR_W        32                 // ADDR[63:32]
`define DM_TAG_LSB       64
`define DM_TAG_W         4                  // TAG[67:64]
`define DM_RSVD_LSB      68
`define DM_RSVD_W        4                  // RSVD[71:68]

// DataMover 状态字（M_AXIS_S2MM_STS / M_AXIS_MM2S_STS，8bit）
//   实测约定：0x80 = 正常完成；0x10 = 长度不匹配/错误。
//   注：请按所用 DataMover 版本 PG022 "Status Stream" 表复核后微调 o_sts_err 判据。
`define DM_STS_OK        8'h80

// -----------------------------------------------------------------------------
// 3) DDR 分区（契约 §3.4：环形采集区 与 冻结快照区 物理隔离）
// -----------------------------------------------------------------------------
// 对齐铁律（推导见 pd_ddr_ring_wr.v 头部说明）：
//   a) DataMover 关闭 DRE 时，起始地址必须 8 Byte 对齐（= AXI 数据位宽）；
//   b) 契约 §3.3 要求块 24 Byte 对齐，故基址与长度均取 24 的整数倍；
//   c) 同时满足 4KB 与 24B 对齐 -> 取 LCM(4096,24) = 12288 = 0x3000 的整数倍。
//
//   校验：0x1000_2000 / 12288 = 21846  ✓； 0x07FF_E000 / 12288 = 10922 ✓
//         0x1800_0000 / 12288 = 32768  ✓； 0x007F_E000 / 12288 = 682    ✓
//
//   RING  : 0x1000_2000 .. 0x1800_0000   (128 MiB - 8 KiB, 24B/4KB 双对齐)
//   SNAP  : 0x1800_0000 .. 0x187F_E000   (  8 MiB - 8 KiB)
//   容量校验：50Hz 一个周期 @26MSPS = 5.2e5 样本 x 6B = 3.12 MB < 8 MiB ✓
//             环形深度 134.2 MB / 156 MB/s = 860 ms ≈ 43 个工频周期，远大于 1 周期 ✓
`define DDR_RING_BASE    32'h1000_2000
`define DDR_RING_SIZE    32'h07FF_E000
`define DDR_SNAP_BASE    32'h1800_0000
`define DDR_SNAP_SIZE    32'h007F_E000

// -----------------------------------------------------------------------------
// 4) 突发参数
// -----------------------------------------------------------------------------
// 每个突发命令搬运 64 个块 = 1536 Byte = 192 个 64bit beat
//   - 192 beat < DataMover 单次 AXI4 突发上限 256 beat，可单突发完成
//   - 1536 % 24 == 0，保证每个命令起止地址始终 24B 对齐
//   - 1536 % 8  == 0，满足关闭 DRE 时的 8B 地址对齐约束
`define DDR_BURST_BLOCKS 64
`define DDR_BURST_BYTES  (`DDR_BURST_BLOCKS * `PD_BLK_BYTES)   // 1536

`endif
