`timescale 1ns / 1ps
// =============================================================================
// pd_ddr_wr_top.v  --  Path A：ADC -> 48bit 打包 -> 跨时钟 FIFO -> 192bit 块
//                      -> AXI-Stream FIFO -> DataMover -> PS7 HP -> DDR3 环形区
//                      + 冻结快照区拷贝链（DataMover MM2S->S2MM 环回）
// -----------------------------------------------------------------------------
// 官方 IP 使用清单（尽量把"轮子"交给 IP，自研只留真正的业务逻辑）：
//   1. ZYNQ7 Processing System (PS7)   —— DDR3 控制器本体 + S_AXI_HP0/1 从机口
//      （领航者V2 的 DDR3 挂在 PS 上，PL 经 HP 口访问；若换用 PL 侧 DDR 则改 MIG7）
//   2. fifo_generator (fifo_48_cdc)    —— 48bit 独立时钟 BRAM FIFO（ADC 域->130M 域）
//   3. axis_data_fifo (axis_dfi_64)    —— 64bit AXI-Stream 弹性缓冲（吸收突发抖动）
//   4. axi_datamover  (dm_wr)          —— S2MM：流 -> 内存，负责 AXI4 突发/拆包
//   5. axi_datamover  (dm_cp)          —— MM2S+S2MM：快照区内存到内存拷贝
//   6. SmartConnect / AXI Interconnect —— AXI4(DataMover) 转 AXI3(PS7 HP) 并仲裁
//   7. Clocking Wizard / Proc Sys Reset—— 130MHz 时钟与复位（BD 内）
//
// 本文件只做"RTL 侧"整合，AXI 主口以扁平端口引出，在 BD 里接到
// SmartConnect，再经 PS7 的 S_AXI_HP0/HP1 落到 DDR3。
//
// 数据率核对（26MSPS 当前验证档）：
//   4ch x 12bit x 26MSPS = 156 MB/s；130MHz x 8B = 1040 MB/s，占用 15%。
//   一个 50Hz 周期快照为 3.12MB，和环写叠加后仍有充分的 HP 带宽余量。
// =============================================================================
`include "pd_ddr_defines.vh"

module pd_ddr_wr_top #(
    parameter integer CH_NUM      = `PD_CH_NUM,
    parameter integer ADC_W       = `PD_ADC_W,
    parameter integer SAMPLE_W    = `PD_SAMPLE_W,     // 48
    parameter integer AXI_DATA_W  = 64,
    parameter integer AXI_ADDR_W  = 32
)(
    // ================= 时钟 / 复位 =================
    input  wire                      clk,          // 130MHz PL 主时钟（PS FCLK0）
    input  wire                      rst_n,        // 系统域低有效
    input  wire                      adc_clk,      // ADC 位时钟（当前 26M）

    // ================= ADC 输入（adc_clk 域）=================
    input  wire [CH_NUM*ADC_W-1:0]   adc_data,     // {ch3,ch2,ch1,ch0} offset binary
    input  wire [CH_NUM-1:0]         adc_dv,

    // ================= Path B（clk 域，送实时特征提取）=================
    // 与 Path A 共用 ADC 域的 pd_pack48。独立 FIFO 保证 DDR 反压或快照搬运
    // 不会改变实时特征链的样本顺序与节拍。
    output wire [CH_NUM*ADC_W-1:0]   o_feat_data,
    output wire [CH_NUM-1:0]         o_feat_dv,
    output wire                      o_feat_ovf,

    // ================= 周期边界（来自 AC/DC 周期管理）=================
    input  wire                      cycle_start,  // 1 拍脉冲

    // ================= AXI4-Lite 从机（PS M_AXI_GP0）=================
    input  wire [15:0]               s_axi_awaddr,
    input  wire                      s_axi_awvalid,
    output wire                      s_axi_awready,
    input  wire [31:0]               s_axi_wdata,
    input  wire [3:0]                s_axi_wstrb,
    input  wire                      s_axi_wvalid,
    output wire                      s_axi_wready,
    output wire [1:0]                s_axi_bresp,
    output wire                      s_axi_bvalid,
    input  wire                      s_axi_bready,
    input  wire [15:0]               s_axi_araddr,
    input  wire                      s_axi_arvalid,
    output wire                      s_axi_arready,
    output wire [31:0]               s_axi_rdata,
    output wire [1:0]                s_axi_rresp,
    output wire                      s_axi_rvalid,
    input  wire                      s_axi_rready,

    // ================= AXI4 主口 1：环形区写（-> HP0）=================
    output wire [3:0]                m_axi_wr_awid,
    output wire [AXI_ADDR_W-1:0]     m_axi_wr_awaddr,
    output wire [7:0]                m_axi_wr_awlen,
    output wire [2:0]                m_axi_wr_awsize,
    output wire [1:0]                m_axi_wr_awburst,
    output wire [2:0]                m_axi_wr_awprot,
    output wire [3:0]                m_axi_wr_awcache,
    output wire [3:0]                m_axi_wr_awuser,
    output wire                      m_axi_wr_awvalid,
    input  wire                      m_axi_wr_awready,
    output wire [AXI_DATA_W-1:0]     m_axi_wr_wdata,
    output wire [AXI_DATA_W/8-1:0]   m_axi_wr_wstrb,
    output wire                      m_axi_wr_wlast,
    output wire                      m_axi_wr_wvalid,
    input  wire                      m_axi_wr_wready,
    input  wire [1:0]                m_axi_wr_bresp,
    input  wire                      m_axi_wr_bvalid,
    output wire                      m_axi_wr_bready,

    // ================= AXI4 主口 2：快照拷贝-读环形区（-> HP1）=================
    output wire [3:0]                m_axi_rd_arid,
    output wire [AXI_ADDR_W-1:0]     m_axi_rd_araddr,
    output wire [7:0]                m_axi_rd_arlen,
    output wire [2:0]                m_axi_rd_arsize,
    output wire [1:0]                m_axi_rd_arburst,
    output wire [2:0]                m_axi_rd_arprot,
    output wire [3:0]                m_axi_rd_arcache,
    output wire [3:0]                m_axi_rd_aruser,
    output wire                      m_axi_rd_arvalid,
    input  wire                      m_axi_rd_arready,
    input  wire [AXI_DATA_W-1:0]     m_axi_rd_rdata,
    input  wire [1:0]                m_axi_rd_rresp,
    input  wire                      m_axi_rd_rlast,
    input  wire                      m_axi_rd_rvalid,
    output wire                      m_axi_rd_rready,

    // ================= AXI4 主口 3：快照拷贝-写快照区（-> HP1）=================
    output wire [3:0]                m_axi_cw_awid,
    output wire [AXI_ADDR_W-1:0]     m_axi_cw_awaddr,
    output wire [7:0]                m_axi_cw_awlen,
    output wire [2:0]                m_axi_cw_awsize,
    output wire [1:0]                m_axi_cw_awburst,
    output wire [2:0]                m_axi_cw_awprot,
    output wire [3:0]                m_axi_cw_awcache,
    output wire [3:0]                m_axi_cw_awuser,
    output wire                      m_axi_cw_awvalid,
    input  wire                      m_axi_cw_awready,
    output wire [AXI_DATA_W-1:0]     m_axi_cw_wdata,
    output wire [AXI_DATA_W/8-1:0]   m_axi_cw_wstrb,
    output wire                      m_axi_cw_wlast,
    output wire                      m_axi_cw_wvalid,
    input  wire                      m_axi_cw_wready,
    input  wire [1:0]                m_axi_cw_bresp,
    input  wire                      m_axi_cw_bvalid,
    output wire                      m_axi_cw_bready,

    // ================= 中断 / 调试 =================
    output wire                      irq,
    output wire [15:0]               dbg_ddr
);

    // =========================================================================
    // 内部信号
    // =========================================================================
    // --- 48bit 样本字（adc_clk 域 -> FIFO -> clk 域）---
    wire [SAMPLE_W-1:0] s48_a_data;   wire s48_a_valid;      // adc_clk 域
    wire [SAMPLE_W-1:0] s48_b_data;   wire s48_b_valid;      // 扇出到算法链
    wire                skew_err;
    wire [SAMPLE_W-1:0] fifo_dout;    wire fifo_empty, fifo_full;
    wire                fifo_wr_busy, fifo_rd_busy;
    wire [SAMPLE_W-1:0] feat_fifo_dout;
    wire                feat_fifo_empty, feat_fifo_full;
    wire                feat_fifo_wr_busy, feat_fifo_rd_busy;

    // --- 192bit 块 -> 64bit AXI-Stream ---
    // pk_tready   : 下游(axis_data_fifo)的接收 ready —— 单向，只由 IP 驱动
    // pk_s48_ready: pd_pack192 自身的接收 ready（块未装满）—— 单向，只由 RTL 驱动
    // 二者必须分开！若把 o_s48_ready 也接到 pk_tready 上会形成多驱动（Synth 8-6859）
    wire [AXI_DATA_W-1:0] pk_tdata,   dfi_tdata;
    wire                  pk_tvalid,  pk_tready;
    wire                  pk_s48_ready;
    wire                  dfi_tvalid, dfi_tready;

    // --- DataMover(dm_wr) 命令 / 状态 ---
    wire [`DM_CMD_W-1:0] wr_cmd_tdata;
    wire                 wr_cmd_tvalid, wr_cmd_tready;
    wire [7:0]           wr_sts_tdata;
    wire                 wr_sts_tvalid, wr_sts_tready, wr_sts_tlast;
    wire [0:0]           wr_sts_tkeep;
    wire                 dm_wr_err;

    // --- DataMover(dm_cp) 命令 / 状态 ---
    wire [`DM_CMD_W-1:0] cp_wr_cmd_tdata, cp_rd_cmd_tdata;
    wire                 cp_wr_cmd_tvalid, cp_wr_cmd_tready;
    wire                 cp_rd_cmd_tvalid, cp_rd_cmd_tready;
    wire [7:0]           cp_wr_sts_tdata,  cp_rd_sts_tdata;
    wire                 cp_wr_sts_tvalid, cp_wr_sts_tready, cp_wr_sts_tlast;
    wire                 cp_rd_sts_tvalid, cp_rd_sts_tready, cp_rd_sts_tlast;
    wire [0:0]           cp_wr_sts_tkeep,  cp_rd_sts_tkeep;
    wire                 dm_cp_mm2s_err, dm_cp_s2mm_err;

    // --- dm_cp 的流环回（MM2S 出 -> S2MM 入）---
    wire [AXI_DATA_W-1:0] cp_m_tdata;
    wire [AXI_DATA_W/8-1:0] cp_m_tkeep;
    wire                  cp_m_tlast, cp_m_tvalid, cp_m_tready;

    // --- 控制 / 状态 ---
    wire                 acq_en, sw_rst;
    wire [31:0]          snap_base, snap_size;
    wire                 freeze_trig, freeze_resume, snap_start;
    wire                 freeze_done;
    wire [31:0]          freeze_base, freeze_len;
    wire                 copy_busy, copy_done, copy_err;
    wire [31:0]          copy_chunk_cnt, copy_bytes_done;
    wire                 auto_snap_en;
    wire [3:0]           slot_lock_cmd, slot_release_cmd;
    wire                 slot_status_clear;
    wire [3:0]           slot_valid, slot_busy, slot_locked;
    wire                 slot_full, slot_cfg_err, slot_cmd_err;
    wire                 slot_req_overflow, slot_req_pending;
    wire                 slot_snapshot_ready;
    wire [1:0]           slot_last;
    wire [31:0]          slot_snapshot_seq, slot_drop_count;
    wire [31:0]          slot0_len, slot1_len, slot2_len, slot3_len;
    wire [31:0]          slot0_seq, slot1_seq, slot2_seq, slot3_seq;

    // =========================================================================
    // 1) 复位处理
    //    fifo_generator 的 rst 为**高有效异步**；DataMover 为低有效同步。
    // =========================================================================
    wire fifo_rst = ~rst_n;                     // 高有效
    wire ddr_rst_n = rst_n & ~sw_rst;           // 低有效，含软件复位

    // rst_n 属于 130 MHz 域，ADC 时钟是外部源。ADC 域复位释放必须在
    // adc_clk 上同步，避免 fifo_generator 写侧与 pd_pack48 在复位退出时
    // 进入亚稳态。
    (* ASYNC_REG = "TRUE" *) reg [1:0] adc_rst_sync;
    always @(posedge adc_clk or negedge rst_n) begin
        if (!rst_n)
            adc_rst_sync <= 2'b00;
        else
            adc_rst_sync <= {adc_rst_sync[0], 1'b1};
    end
    wire adc_rst_n = adc_rst_sync[1];

    // =========================================================================
    // 2) 4 通道 -> 48bit 样本字（契约 §3.1）
    // =========================================================================
    pd_pack48 #(
        .CH_NUM (CH_NUM),
        .ADC_W  (ADC_W)
    ) u_pack48 (
        .clk          (adc_clk),
        .rst_n        (adc_rst_n),
        .i_adc_data   (adc_data),
        .i_adc_dv     (adc_dv),
        .o_s48_data   (s48_a_data),     // Path A: -> FIFO -> DDR
        .o_s48_valid  (s48_a_valid),
        .o_ch_data    (s48_b_data),     // Path B: -> 实时算法链（此处引出备用）
        .o_ch_valid   (s48_b_valid),
        .o_ch_skew_err(skew_err)
    );

    // =========================================================================
    // 3) 跨时钟域异步 FIFO（IP: fifo_generator）
    //    48bit x 1024，独立时钟 BRAM，FWFT（首字直通）
    //    写：adc_clk 域；读：clk(130M) 域
    // =========================================================================
    wire fifo_wr_en = s48_a_valid & ~fifo_full & ~fifo_wr_busy;
    wire fifo_rd_en;

    fifo_48_cdc u_fifo48 (
        .rst        (fifo_rst),         // input 高有效异步
        .wr_clk     (adc_clk),
        .rd_clk     (clk),
        .din        (s48_a_data),       // [47:0]
        .wr_en      (fifo_wr_en),
        .rd_en      (fifo_rd_en),
        .dout       (fifo_dout),        // [47:0]
        .full       (fifo_full),
        .empty      (fifo_empty),
        .wr_rst_busy(fifo_wr_busy),
        .rd_rst_busy(fifo_rd_busy)
    );

    // Path B has a separate official asynchronous FIFO. This is the actual
    // fanout boundary: DDR backpressure cannot stall or reorder feature input.
    wire feat_fifo_wr_en = s48_b_valid & ~feat_fifo_full & ~feat_fifo_wr_busy;
    wire feat_fifo_rd_en = ~feat_fifo_empty & ~feat_fifo_rd_busy;

    fifo_48_cdc u_fifo48_feature (
        .rst        (fifo_rst),
        .wr_clk     (adc_clk),
        .rd_clk     (clk),
        .din        (s48_b_data),
        .wr_en      (feat_fifo_wr_en),
        .rd_en      (feat_fifo_rd_en),
        .dout       (feat_fifo_dout),
        .full       (feat_fifo_full),
        .empty      (feat_fifo_empty),
        .wr_rst_busy(feat_fifo_wr_busy),
        .rd_rst_busy(feat_fifo_rd_busy)
    );

    assign o_feat_data = feat_fifo_dout;
    assign o_feat_dv   = {CH_NUM{feat_fifo_rd_en}};
    assign o_feat_ovf  = s48_b_valid & (feat_fifo_full | feat_fifo_wr_busy);

    // =========================================================================
    // 4) 4x48bit -> 192bit 块 -> 3x64bit beat（契约 §3.2）
    //    FWFT：!empty 时 dout 同拍有效，故 rd_en 即 valid
    // =========================================================================
    wire [31:0] blk_cnt;
    wire        blk_done, beat_en;

    // FIFO 读使能只由 pack192 的输入容量决定。pk_tready 是 pack192 对
    // axis_data_fifo 的输出握手，不能反向限制其尚未填满的 192-bit 缓冲。
    assign fifo_rd_en = pk_s48_ready & ~fifo_empty & ~fifo_rd_busy;

    pd_pack192 #(
        .SAMPLE_W (SAMPLE_W),
        .BLK_W    (`PD_BLK_W),
        .AXI_DW   (AXI_DATA_W)
    ) u_pack192 (
        .clk          (clk),
        .rst_n        (ddr_rst_n),
        .i_s48_data   (fifo_dout),
        .i_s48_valid  (fifo_rd_en),
        .o_s48_ready  (pk_s48_ready),   // 只驱动 pk_s48_ready，不得接到 pk_tready
        .m_axis_tdata (pk_tdata),
        .m_axis_tvalid(pk_tvalid),
        .m_axis_tready(pk_tready),
        .m_axis_tkeep (),
        .m_axis_tlast (),
        .o_beat_en    (beat_en),      // 1 beat=8B 进入下游，供字节信用计数
        .o_blk_done   (blk_done),
        .o_blk_cnt    (blk_cnt)
    );

    // =========================================================================
    // 5) AXI-Stream Data FIFO（IP: axis_data_fifo）
    //    深度 512 x 64bit = 4096B > 单次突发 1536B，保证命令下发时数据已就位
    // =========================================================================
    axis_dfi_64 u_dfi (
        .s_axis_aresetn(ddr_rst_n),
        .s_axis_aclk   (clk),
        .s_axis_tvalid (pk_tvalid),
        .s_axis_tready (pk_tready),
        .s_axis_tdata  (pk_tdata),
        .m_axis_tvalid (dfi_tvalid),
        .m_axis_tready (dfi_tready),
        .m_axis_tdata  (dfi_tdata)
    );

    // =========================================================================
    // 6) AXI DataMover（S2MM）：流 -> DDR3 环形区
    // =========================================================================
    dm_wr u_dm_wr (
        .m_axi_s2mm_aclk          (clk),
        .m_axi_s2mm_aresetn       (ddr_rst_n),
        .s2mm_err                 (dm_wr_err),
        .m_axis_s2mm_cmdsts_awclk (clk),
        .m_axis_s2mm_cmdsts_aresetn(ddr_rst_n),
        .s_axis_s2mm_cmd_tvalid   (wr_cmd_tvalid),
        .s_axis_s2mm_cmd_tready   (wr_cmd_tready),
        .s_axis_s2mm_cmd_tdata    (wr_cmd_tdata),
        .m_axis_s2mm_sts_tvalid   (wr_sts_tvalid),
        .m_axis_s2mm_sts_tready   (wr_sts_tready),
        .m_axis_s2mm_sts_tdata    (wr_sts_tdata),
        .m_axis_s2mm_sts_tkeep    (wr_sts_tkeep),
        .m_axis_s2mm_sts_tlast    (wr_sts_tlast),
        .m_axi_s2mm_awid          (m_axi_wr_awid),
        .m_axi_s2mm_awaddr        (m_axi_wr_awaddr),
        .m_axi_s2mm_awlen         (m_axi_wr_awlen),
        .m_axi_s2mm_awsize        (m_axi_wr_awsize),
        .m_axi_s2mm_awburst       (m_axi_wr_awburst),
        .m_axi_s2mm_awprot        (m_axi_wr_awprot),
        .m_axi_s2mm_awcache       (m_axi_wr_awcache),
        .m_axi_s2mm_awuser        (m_axi_wr_awuser),
        .m_axi_s2mm_awvalid       (m_axi_wr_awvalid),
        .m_axi_s2mm_awready       (m_axi_wr_awready),
        .m_axi_s2mm_wdata         (m_axi_wr_wdata),
        .m_axi_s2mm_wstrb         (m_axi_wr_wstrb),
        .m_axi_s2mm_wlast         (m_axi_wr_wlast),
        .m_axi_s2mm_wvalid        (m_axi_wr_wvalid),
        .m_axi_s2mm_wready        (m_axi_wr_wready),
        .m_axi_s2mm_bresp         (m_axi_wr_bresp),
        .m_axi_s2mm_bvalid        (m_axi_wr_bvalid),
        .m_axi_s2mm_bready        (m_axi_wr_bready),
        .s_axis_s2mm_tdata        (dfi_tdata),
        .s_axis_s2mm_tkeep        ({AXI_DATA_W/8{1'b1}}),
        .s_axis_s2mm_tlast        (1'b0),
        .s_axis_s2mm_tvalid       (dfi_tvalid),
        .s_axis_s2mm_tready       (dfi_tready)
    );

    // =========================================================================
    // 7) 环形写入管理器（本项目的核心业务逻辑）
    // =========================================================================
    wire [31:0] wr_off, wrap_cnt, cmd_cnt, avail_bytes;
    wire        hiwm, ring_ovf, snap_overrun, ring_err;
    wire        ring_write_idle;
    wire [63:0] wr_bytes;
    wire [1:0]  ring_state;

    // 上游溢出：异步 FIFO 满时仍在写 / Data FIFO 反压导致丢样本
    wire up_ovf = (s48_a_valid & (fifo_full | fifo_wr_busy)) |
                  (s48_b_valid & (feat_fifo_full | feat_fifo_wr_busy));

    pd_ddr_ring_wr #(
        .AXI_ADDR_W  (AXI_ADDR_W),
        .AXI_DATA_W  (AXI_DATA_W),
        .RING_BASE   (`DDR_RING_BASE),
        .RING_SIZE   (`DDR_RING_SIZE),
        .BURST_BLOCKS(`DDR_BURST_BLOCKS),
        .AVAIL_W     (20),
        .OVF_THRESH  (3072)
    ) u_ring (
        .clk            (clk),
        .rst_n          (ddr_rst_n),
        .i_acq_en       (acq_en),
        .i_beat_en      (beat_en),        // pack192 每接收 1 个 64bit beat
        .i_up_ovf       (up_ovf),
        .m_cmd_tdata    (wr_cmd_tdata),
        .m_cmd_tvalid   (wr_cmd_tvalid),
        .m_cmd_tready   (wr_cmd_tready),
        .s_sts_tdata    (wr_sts_tdata),
        .s_sts_tvalid   (wr_sts_tvalid),
        .s_sts_tready   (wr_sts_tready),
        .i_s2mm_err     (dm_wr_err),
        .i_cycle_start  (cycle_start),
        .i_freeze_trig  (freeze_trig),
        .i_freeze_resume(freeze_resume),
        .o_wr_addr      (),
        .o_wr_off       (wr_off),
        .o_blk_idx      (),
        .o_wrap_cnt     (wrap_cnt),
        .o_cmd_cnt      (cmd_cnt),
        .o_wr_bytes     (wr_bytes),
        .o_avail_bytes  (avail_bytes),
        .o_hiwm         (hiwm),
        .o_ovf          (ring_ovf),
        .o_freeze_done  (freeze_done),
        .o_freeze_base  (freeze_base),
        .o_freeze_len   (freeze_len),
        .o_snap_overrun (snap_overrun),
        .o_last_btt     (),
        .o_sts_tdata    (),
        .o_err          (ring_err),
        .o_state        (ring_state),
        .o_write_idle   (ring_write_idle)
    );

    // =========================================================================
    // 8) AXI DataMover（MM2S+S2MM）：环形区 -> 快照区 拷贝
    //    MM2S 读出的流直接环回给自己的 S2MM，实现内存到内存搬运
    // =========================================================================
    dm_cp u_dm_cp (
        .m_axi_mm2s_aclk          (clk),
        .m_axi_mm2s_aresetn       (ddr_rst_n),
        .mm2s_err                 (dm_cp_mm2s_err),
        .m_axis_mm2s_cmdsts_aclk  (clk),
        .m_axis_mm2s_cmdsts_aresetn(ddr_rst_n),
        .s_axis_mm2s_cmd_tvalid   (cp_rd_cmd_tvalid),
        .s_axis_mm2s_cmd_tready   (cp_rd_cmd_tready),
        .s_axis_mm2s_cmd_tdata    (cp_rd_cmd_tdata),
        .m_axis_mm2s_sts_tvalid   (cp_rd_sts_tvalid),
        .m_axis_mm2s_sts_tready   (cp_rd_sts_tready),
        .m_axis_mm2s_sts_tdata    (cp_rd_sts_tdata),
        .m_axis_mm2s_sts_tkeep    (cp_rd_sts_tkeep),
        .m_axis_mm2s_sts_tlast    (cp_rd_sts_tlast),
        .m_axi_mm2s_arid          (m_axi_rd_arid),
        .m_axi_mm2s_araddr        (m_axi_rd_araddr),
        .m_axi_mm2s_arlen         (m_axi_rd_arlen),
        .m_axi_mm2s_arsize        (m_axi_rd_arsize),
        .m_axi_mm2s_arburst       (m_axi_rd_arburst),
        .m_axi_mm2s_arprot        (m_axi_rd_arprot),
        .m_axi_mm2s_arcache       (m_axi_rd_arcache),
        .m_axi_mm2s_aruser        (m_axi_rd_aruser),
        .m_axi_mm2s_arvalid       (m_axi_rd_arvalid),
        .m_axi_mm2s_arready       (m_axi_rd_arready),
        .m_axi_mm2s_rdata         (m_axi_rd_rdata),
        .m_axi_mm2s_rresp         (m_axi_rd_rresp),
        .m_axi_mm2s_rlast         (m_axi_rd_rlast),
        .m_axi_mm2s_rvalid        (m_axi_rd_rvalid),
        .m_axi_mm2s_rready        (m_axi_rd_rready),
        // ---- MM2S 流出的数据直接环回给本 IP 的 S2MM ----
        .m_axis_mm2s_tdata        (cp_m_tdata),
        .m_axis_mm2s_tkeep        (cp_m_tkeep),
        .m_axis_mm2s_tlast        (cp_m_tlast),
        .m_axis_mm2s_tvalid       (cp_m_tvalid),
        .m_axis_mm2s_tready       (cp_m_tready),

        .m_axi_s2mm_aclk          (clk),
        .m_axi_s2mm_aresetn       (ddr_rst_n),
        .s2mm_err                 (dm_cp_s2mm_err),
        .m_axis_s2mm_cmdsts_awclk (clk),
        .m_axis_s2mm_cmdsts_aresetn(ddr_rst_n),
        .s_axis_s2mm_cmd_tvalid   (cp_wr_cmd_tvalid),
        .s_axis_s2mm_cmd_tready   (cp_wr_cmd_tready),
        .s_axis_s2mm_cmd_tdata    (cp_wr_cmd_tdata),
        .m_axis_s2mm_sts_tvalid   (cp_wr_sts_tvalid),
        .m_axis_s2mm_sts_tready   (cp_wr_sts_tready),
        .m_axis_s2mm_sts_tdata    (cp_wr_sts_tdata),
        .m_axis_s2mm_sts_tkeep    (cp_wr_sts_tkeep),
        .m_axis_s2mm_sts_tlast    (cp_wr_sts_tlast),
        .m_axi_s2mm_awid          (m_axi_cw_awid),
        .m_axi_s2mm_awaddr        (m_axi_cw_awaddr),
        .m_axi_s2mm_awlen         (m_axi_cw_awlen),
        .m_axi_s2mm_awsize        (m_axi_cw_awsize),
        .m_axi_s2mm_awburst       (m_axi_cw_awburst),
        .m_axi_s2mm_awprot        (m_axi_cw_awprot),
        .m_axi_s2mm_awcache       (m_axi_cw_awcache),
        .m_axi_s2mm_awuser        (m_axi_cw_awuser),
        .m_axi_s2mm_awvalid       (m_axi_cw_awvalid),
        .m_axi_s2mm_awready       (m_axi_cw_awready),
        .m_axi_s2mm_wdata         (m_axi_cw_wdata),
        .m_axi_s2mm_wstrb         (m_axi_cw_wstrb),
        .m_axi_s2mm_wlast         (m_axi_cw_wlast),
        .m_axi_s2mm_wvalid        (m_axi_cw_wvalid),
        .m_axi_s2mm_wready        (m_axi_cw_wready),
        .m_axi_s2mm_bresp         (m_axi_cw_bresp),
        .m_axi_s2mm_bvalid        (m_axi_cw_bvalid),
        .m_axi_s2mm_bready        (m_axi_cw_bready),
        .s_axis_s2mm_tdata        (cp_m_tdata),
        .s_axis_s2mm_tkeep        (cp_m_tkeep),
        .s_axis_s2mm_tlast        (cp_m_tlast),
        .s_axis_s2mm_tvalid       (cp_m_tvalid),
        .s_axis_s2mm_tready       (cp_m_tready)
    );

    // =========================================================================
    // 9) 快照拷贝控制：freeze_done 后必须等 DataMover 写尾部全部完成。
    //    否则 MM2S 可能先于最后一条 S2MM 写读到旧数据。
    // =========================================================================
    reg freeze_done_d;
    reg legacy_auto_copy_pending;
    reg slot_freeze_req;
    reg slot_req_overflow_pulse;
    wire legacy_auto_copy_launch;
    wire manual_copy_launch;
    wire slot_copy_start;
    wire [AXI_ADDR_W-1:0] slot_copy_src_addr, slot_copy_dst_addr;
    wire [31:0] slot_copy_len;
    wire slot_req_ack, slot_req_drop;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            freeze_done_d    <= 1'b0;
            legacy_auto_copy_pending <= 1'b0;
            slot_freeze_req <= 1'b0;
            slot_req_overflow_pulse <= 1'b0;
        end else begin
            freeze_done_d <= freeze_done;
            slot_req_overflow_pulse <= 1'b0;
            if (freeze_resume) begin
                legacy_auto_copy_pending <= 1'b0;
                slot_freeze_req <= 1'b0;
            end else begin
                if (~freeze_done_d & freeze_done) begin
                    if (auto_snap_en) begin
                        if (slot_freeze_req)
                            slot_req_overflow_pulse <= 1'b1;
                        else
                            slot_freeze_req <= 1'b1;
                    end else begin
                        legacy_auto_copy_pending <= 1'b1;
                    end
                end
                if (!auto_snap_en)
                    slot_freeze_req <= 1'b0;
                if (legacy_auto_copy_launch)
                    legacy_auto_copy_pending <= 1'b0;
                if (slot_req_ack || slot_req_drop)
                    slot_freeze_req <= 1'b0;
            end
        end
    end
    wire snap_len_err = (freeze_len > snap_size);
    // 快照目标必须完全落在编译期保留的 Snapshot 分区内，防止可配置
    // SNAP_BASE/SNAP_SIZE 把拷贝写到 Ring 或其他 DDR 使用者区域。
    wire snap_addr_err = (snap_base < `DDR_SNAP_BASE) ||
                          (snap_base >= (`DDR_SNAP_BASE + `DDR_SNAP_SIZE)) ||
                          (freeze_len > (`DDR_SNAP_BASE + `DDR_SNAP_SIZE - snap_base));
    wire snap_base_align24;
    wire snap_size_align24;
    wire freeze_base_align24;
    wire freeze_len_align24;
    pd_align24_check u_snap_base_align24
        (.value(snap_base),   .aligned(snap_base_align24));
    pd_align24_check u_snap_size_align24
        (.value(snap_size),   .aligned(snap_size_align24));
    pd_align24_check u_freeze_base_align24
        (.value(freeze_base), .aligned(freeze_base_align24));
    pd_align24_check u_freeze_len_align24
        (.value(freeze_len),  .aligned(freeze_len_align24));
    wire snap_align_err = !snap_base_align24 || !snap_size_align24 ||
                          !freeze_base_align24 || !freeze_len_align24;
    wire legacy_snapshot_cfg_err = snap_len_err || snap_addr_err || snap_align_err;
    // Legacy automatic capture is retained when slot automatic mode is disabled.
    // A manual o_snap_start remains available in either mode and gets priority.
    assign legacy_auto_copy_launch = !auto_snap_en && legacy_auto_copy_pending &&
                                     ring_write_idle && !copy_busy &&
                                     !legacy_snapshot_cfg_err;
    assign manual_copy_launch = snap_start && freeze_done && ring_write_idle &&
                                !copy_busy && !legacy_snapshot_cfg_err;
    wire snapshot_cfg_err = auto_snap_en ? slot_cfg_err : legacy_snapshot_cfg_err;
    wire copy_start = legacy_auto_copy_launch | manual_copy_launch | slot_copy_start;
    wire [AXI_ADDR_W-1:0] copy_src_addr = slot_copy_start ? slot_copy_src_addr : freeze_base;
    wire [AXI_ADDR_W-1:0] copy_dst_addr = slot_copy_start ? slot_copy_dst_addr : snap_base;
    wire [31:0] copy_len = slot_copy_start ? slot_copy_len : freeze_len;

    pd_ddr_slot_mgr #(
        .AXI_ADDR_W (AXI_ADDR_W)
    ) u_slot_mgr (
        .clk                 (clk),
        .rst_n               (ddr_rst_n),
        .i_freeze_req        (slot_freeze_req),
        .i_cancel_req        (freeze_resume),
        .i_freeze_base       (freeze_base),
        .i_freeze_len        (freeze_len),
        .i_auto_snap_en      (auto_snap_en),
        .i_ring_write_idle   (ring_write_idle),
        .i_copy_busy         (copy_busy),
        .i_copy_done         (copy_done),
        .i_copy_err          (copy_err),
        .i_manual_copy_launch(manual_copy_launch),
        .i_slot_lock         (slot_lock_cmd),
        .i_slot_release      (slot_release_cmd),
        .i_req_overflow      (slot_req_overflow_pulse),
        .i_status_clear      (slot_status_clear),
        .o_req_ack           (slot_req_ack),
        .o_req_drop          (slot_req_drop),
        .o_copy_start        (slot_copy_start),
        .o_copy_src_addr     (slot_copy_src_addr),
        .o_copy_dst_addr     (slot_copy_dst_addr),
        .o_copy_len          (slot_copy_len),
        .o_slot_valid        (slot_valid),
        .o_slot_busy         (slot_busy),
        .o_slot_locked       (slot_locked),
        .o_slot_full         (slot_full),
        .o_cfg_err           (slot_cfg_err),
        .o_cmd_err           (slot_cmd_err),
        .o_req_overflow      (slot_req_overflow),
        .o_req_pending       (slot_req_pending),
        .o_snapshot_ready    (slot_snapshot_ready),
        .o_last_slot         (slot_last),
        .o_snapshot_seq      (slot_snapshot_seq),
        .o_drop_count        (slot_drop_count),
        .o_slot0_len         (slot0_len), .o_slot1_len(slot1_len),
        .o_slot2_len         (slot2_len), .o_slot3_len(slot3_len),
        .o_slot0_seq         (slot0_seq), .o_slot1_seq(slot1_seq),
        .o_slot2_seq         (slot2_seq), .o_slot3_seq(slot3_seq)
    );

    pd_ddr_snap_copy #(
        .AXI_ADDR_W  (AXI_ADDR_W),
        .CHUNK_BLOCKS(256)
    ) u_snap (
        .clk             (clk),
        .rst_n           (ddr_rst_n),
        .i_clear         (freeze_resume),
        .i_start         (copy_start),
        .i_src_addr      (copy_src_addr),
        .i_dst_addr      (copy_dst_addr),
        .i_len_bytes     (copy_len),
        .i_ring_base     (`DDR_RING_BASE),
        .i_ring_size     (`DDR_RING_SIZE),
        .o_busy          (copy_busy),
        .o_done          (copy_done),
        .o_err           (copy_err),
        .o_chunk_cnt     (copy_chunk_cnt),
        .o_bytes_done    (copy_bytes_done),
        .m_rd_cmd_tdata  (cp_rd_cmd_tdata),
        .m_rd_cmd_tvalid (cp_rd_cmd_tvalid),
        .m_rd_cmd_tready (cp_rd_cmd_tready),
        .s_rd_sts_tdata  (cp_rd_sts_tdata),
        .s_rd_sts_tvalid (cp_rd_sts_tvalid),
        .m_rd_sts_tready (cp_rd_sts_tready),
        .i_mm2s_err      (dm_cp_mm2s_err),
        .m_wr_cmd_tdata  (cp_wr_cmd_tdata),
        .m_wr_cmd_tvalid (cp_wr_cmd_tvalid),
        .m_wr_cmd_tready (cp_wr_cmd_tready),
        .s_wr_sts_tdata  (cp_wr_sts_tdata),
        .s_wr_sts_tvalid (cp_wr_sts_tvalid),
        .m_wr_sts_tready (cp_wr_sts_tready),
        .i_s2mm_err      (dm_cp_s2mm_err)
    );

    // =========================================================================
    // 10) AXI4-Lite 寄存器接口
    // =========================================================================
    pd_ddr_axil #(
        .AW (16),
        .DW (32)
    ) u_axil (
        .s_axi_aclk     (clk),
        .s_axi_aresetn  (rst_n),
        .s_axi_awaddr   (s_axi_awaddr),
        .s_axi_awvalid  (s_axi_awvalid),
        .s_axi_awready  (s_axi_awready),
        .s_axi_wdata    (s_axi_wdata),
        .s_axi_wstrb    (s_axi_wstrb),
        .s_axi_wvalid   (s_axi_wvalid),
        .s_axi_wready   (s_axi_wready),
        .s_axi_bresp    (s_axi_bresp),
        .s_axi_bvalid   (s_axi_bvalid),
        .s_axi_bready   (s_axi_bready),
        .s_axi_araddr   (s_axi_araddr),
        .s_axi_arvalid  (s_axi_arvalid),
        .s_axi_arready  (s_axi_arready),
        .s_axi_rdata    (s_axi_rdata),
        .s_axi_rresp    (s_axi_rresp),
        .s_axi_rvalid   (s_axi_rvalid),
        .s_axi_rready   (s_axi_rready),
        .o_acq_en       (acq_en),
        .o_sw_rst       (sw_rst),
        .o_snap_base    (snap_base),
        .o_snap_size    (snap_size),
        .o_freeze_trig  (freeze_trig),
        .o_freeze_resume(freeze_resume),
        .o_snap_start   (snap_start),
        .o_auto_snap_en (auto_snap_en),
        .o_slot_lock    (slot_lock_cmd),
        .o_slot_release (slot_release_cmd),
        .o_slot_status_clear(slot_status_clear),
        .i_copy_busy    (copy_busy),
        .i_copy_done    (copy_done),
        .i_copy_err     (copy_err),
        .i_copy_pending (auto_snap_en ? slot_req_pending : legacy_auto_copy_pending),
        .i_snapshot_cfg_err(snapshot_cfg_err),
        .i_hiwm         (hiwm),
        .i_freeze_done  (freeze_done),
        .i_snap_overrun (snap_overrun),
        .i_err          (ring_err | ring_ovf | copy_err | snapshot_cfg_err),
        .i_wr_off       (wr_off),
        .i_wrap_cnt     (wrap_cnt),
        .i_wr_bytes     (wr_bytes),
        .i_cmd_cnt      (cmd_cnt),
        .i_avail_bytes  (avail_bytes),
        .i_freeze_base  (freeze_base),
        .i_freeze_len   (freeze_len),
        .i_copy_chunk_cnt(copy_chunk_cnt),
        .i_copy_bytes_done(copy_bytes_done),
        .i_slot_valid   (slot_valid),
        .i_slot_busy    (slot_busy),
        .i_slot_locked  (slot_locked),
        .i_slot_full    (slot_full),
        .i_slot_cfg_err (slot_cfg_err),
        .i_slot_cmd_err (slot_cmd_err),
        .i_slot_req_overflow(slot_req_overflow),
        .i_slot_req_pending(slot_req_pending),
        .i_slot_snapshot_ready(slot_snapshot_ready),
        .i_slot_last    (slot_last),
        .i_slot_snapshot_seq(slot_snapshot_seq),
        .i_slot_drop_count(slot_drop_count),
        .i_slot0_len    (slot0_len), .i_slot1_len(slot1_len),
        .i_slot2_len    (slot2_len), .i_slot3_len(slot3_len),
        .i_slot0_seq    (slot0_seq), .i_slot1_seq(slot1_seq),
        .i_slot2_seq    (slot2_seq), .i_slot3_seq(slot3_seq)
    );

    // =========================================================================
    // 11) 中断与调试观测
    // =========================================================================
    // slot_snapshot_ready is level-held until SLOT_CTRL.status_clear.  This
    // converts an otherwise one-cycle automatic-copy completion into a PS-
    // observable interrupt without changing the external irq port contract.
    assign irq = slot_snapshot_ready | copy_done | snap_overrun | ring_err |
                 ring_ovf | copy_err | snapshot_cfg_err;
    assign dbg_ddr = {
        ring_state,          // [15:14]
        copy_busy,           // [13]
        freeze_done,         // [12]
        snap_overrun,        // [11]
        hiwm,                // [10]
        ring_err,            // [9]
        copy_err,            // [8]
        skew_err,            // [7]
        wr_off[6:0]          // [6:0] 写指针低 7 位（回绕观测）
    };

endmodule
