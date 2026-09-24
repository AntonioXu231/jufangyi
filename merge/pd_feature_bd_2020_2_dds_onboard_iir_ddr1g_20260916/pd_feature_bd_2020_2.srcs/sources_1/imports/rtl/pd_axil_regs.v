// =============================================================================
// pd_axil_regs.v  --  AXI4-Lite Slave 寄存器文件
// -----------------------------------------------------------------------------
// 地址空间 (byte address, 32bit 对齐)
//   !! 区域码取 addr[15:12], 因此各区必须按 4KB 边界对齐 !!
//   0x0000 ~ 0x07FF : 通道寄存器区, 通道 i 基址 = 0x0000 + i*0x80 (32 个字)
//   0x1000 ~ 0x1FFF : 全局寄存器区
//   0x4000 ~ 0x7FFF : PRPD 图谱区, 通道 i 基址 = 0x4000 + i*0x1000 (1024 字)
//
//   [设计教训] 早期版本把全局区放在 0x0800, 但 0x0800 的 addr[15:12] = 0x0,
//   与通道区的区域码相同, 导致全局访问被误译为通道寄存器访问(读 VERSION 实际
//   读到了 r_scale)。故全局区必须落在独立的 4KB 页。
//
// 通道寄存器 (偏移):
//   0x00 CTRL       RW  [0]enable [1]sw_rst(W1P) [2]auto_clear [3]ev_all
//                       [4]prpd_max [5]prpd_clear(W1P)
//   0x04 CFG0       RW  [1:0]mode  [31:16]phase_win(N, <=2048)
//   0x08 PHASE_INC  RW  2^32/N (u 相位累加步长)
//   0x0C THRESH     RW  [11:0] 门限 (ADC 码)
//   0x10 SCALE      RW  Q8.8  视在电荷量标定系数 (码 -> pC)
//   0x14 UPEAK      RW  Q8.8  试验电压峰值 (用于 P)
//   0x18 DEADTIME   RW  [15:0] 死区 (ADC 样本数, 0=禁用)
//   0x1C MEAS_CYC   RW  [15:0] 测量窗口长度 (同步周期数, 0=不自动快照)
//   0x20 STATUS     RO  [0]sync_locked [1]meas_done_sticky [2]overflow
//   0x24 SYNC_PER   RO  实测同步周期 (ADC 样本数 M); f_sync = 20e6 / M
//   0x28 CNT_N      RO  放电脉冲计数 n
//   0x2C Q_MAX      RO  最大视在电荷量 Q (pC)
//   0x30 SUM_ABS_LO RO  Σ|q| [31:0]   -> I = Σ|q| / T
//   0x34 SUM_ABS_HI RO  Σ|q| [63:32]
//   0x38 SUM_QU_LO  RO  Σq·u [31:0]   -> P = Σq·u / T
//   0x3C SUM_QU_HI  RO  Σq·u [63:32]
//   0x40 LIVE_CYC   RO  自上次清零以来累积的周期数
//   0x44 IRQ_STAT   W1C [0]meas_done [1]overflow
//   0x48 IRQ_EN     RW  [0]meas_done_en [1]overflow_en
//   ---- v2: PRPD 框选剔除区域 (REG0 / REG1, 各含相位与电荷量矩形) ----
//   0x4C REG0_CFG   RW  [0]en0 [1]sem0(0=不统计不显示 1=仅剔除PRPD显示, 预留)
//   0x50 REG0_PHASE RW  [23:12]phi1 [11:0]phi2 —— 相位窗号, phi1<=phi2
//   0x54 REG0_Q     RW  [31:16]q1 [15:0]q2 (Q8.8 pC), q1<=q2
//   0x58 REG1_PHASE RW  同上
//   0x5C REG1_Q     RW  同上
//   0x60 REG1_CFG   RW  [0]en1 [1]sem1  (更新清单补充: 原清单遗漏 REG1 使能寄存器)
//
// 全局寄存器 (偏移, 基址 0x1000):
//   0x1000 GCTRL     RW  [0]全局使能 [1]全局软复位
//   0x1004 GSTATUS   RO  [NUM_CH-1:0] 各通道 sync_locked
//   0x1008 GIRQ_STAT W1C 各通道中断聚合
//   0x100C GIRQ_EN   RW
//   0x1010 VERSION   RO  0x0003_0000 (v3: 新增 6 对门槛 + 12 计数器 + 帧快照)
//   0x1014 NUM_CH    RO
//   0x1018 FIFO_CNT  RO  输出 FIFO 占用深度
//   0x101C EV_CNT_LO RO  已输出事件总数 [31:0]
//   0x1020 EV_CNT_HI RO  已输出事件总数 [63:32]
//   ---- v3: 6 正 6 负门槛 (四通道共用) + APPLY ----
//   0x1024 POS_THR[0] RW   0x1028 POS_THR[1]  ... 0x1038 POS_THR[5]
//   0x1040 NEG_THR[0] RW   0x1044 NEG_THR[1]  ... 0x1054 NEG_THR[5]
//   0x1060 APPLY      W1P [0]写 1 提交 shadow 配置(周期边界原子生效)
//
// 计数器区 (0x2000 ~ 0x21FF, 每通道 0x80 字节 = 128B, 只读):
//   通道 i 基址 = 0x2000 + i*0x80
//   0x00~0x14 POS_CNT[0..5]  6 个 32bit 正档计数
//   0x18~0x2C NEG_CNT[0..5]  6 个 32bit 负档计数
//   0x30 FRAME_N_POS  0x34 FRAME_N_NEG  (本帧正/负合计)
//   0x38 FRAME_AD_MAX 0x3C FRAME_AD_MIN (本帧放电 AD 码极值)
//   0x40 FRAME_ID     0x44 FRAME_STAT   ([0]frame_valid 读清 [1]overflow)
// =============================================================================
`timescale 1ns / 1ps
`include "pd_defines.vh"

module pd_axil_regs #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 16,
    parameter integer NUM_CH             = 4,
    parameter integer PH_W               = `PD_PH_W,
    parameter integer VERSION            = 32'h0003_0000
)(
    // ================= AXI4-Lite Slave =================
    input  wire                          S_AXI_ACLK,
    input  wire                          S_AXI_ARESETN,
    input  wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_AWADDR,
    input  wire                          S_AXI_AWVALID,
    output wire                          S_AXI_AWREADY,
    input  wire [C_S_AXI_DATA_WIDTH-1:0] S_AXI_WDATA,
    input  wire [3:0]                    S_AXI_WSTRB,
    input  wire                          S_AXI_WVALID,
    output wire                          S_AXI_WREADY,
    output wire [1:0]                    S_AXI_BRESP,
    output wire                          S_AXI_BVALID,
    input  wire                          S_AXI_BREADY,
    input  wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_ARADDR,
    input  wire                          S_AXI_ARVALID,
    output wire                          S_AXI_ARREADY,
    output wire [C_S_AXI_DATA_WIDTH-1:0] S_AXI_RDATA,
    output wire [1:0]                    S_AXI_RRESP,
    output wire                          S_AXI_RVALID,
    input  wire                          S_AXI_RREADY,

    // ================= 配置输出 (每通道) =================
    output wire [NUM_CH-1:0]             o_enable,
    output wire [NUM_CH-1:0]             o_sw_rst,          // 单拍脉冲
    output wire [NUM_CH-1:0]             o_prpd_clear,      // 单拍脉冲
    output wire [2*NUM_CH-1:0]           o_mode,
    output wire [32*NUM_CH-1:0]          o_phase_win,
    output wire [32*NUM_CH-1:0]          o_phase_inc,
    output wire [32*NUM_CH-1:0]          o_thresh,
    output wire [32*NUM_CH-1:0]          o_scale,
    output wire [32*NUM_CH-1:0]          o_upeak,
    output wire [32*NUM_CH-1:0]          o_deadtime,
    output wire [32*NUM_CH-1:0]          o_meas_cycles,
    output wire [NUM_CH-1:0]             o_auto_clear,
    output wire [NUM_CH-1:0]             o_ev_all,
    output wire [NUM_CH-1:0]             o_prpd_max,

    // ================= v2: 剔除区域配置输出 =================
    output wire [4*NUM_CH-1:0]           o_reg0_cfg,
    output wire [4*NUM_CH-1:0]           o_reg1_cfg,
    output wire [24*NUM_CH-1:0]          o_reg0_phase, o_reg1_phase,
    output wire [32*NUM_CH-1:0]          o_reg0_q,     o_reg1_q,
    output wire [96-1:0]                 o_pos_thr, o_neg_thr,
    output wire                          o_apply,

    // ================= 状态输入 (每通道) =================
    input  wire [NUM_CH-1:0]             i_sync_locked,
    input  wire [NUM_CH-1:0]             i_meas_done,
    input  wire [NUM_CH-1:0]             i_overflow,
    input  wire [32*NUM_CH-1:0]          i_sync_period,
    input  wire [32*NUM_CH-1:0]          i_n,
    input  wire [32*NUM_CH-1:0]          i_qmax,
    input  wire [64*NUM_CH-1:0]          i_sum_abs_q,
    input  wire [64*NUM_CH-1:0]          i_sum_qu,
    input  wire [32*NUM_CH-1:0]          i_live_cycles,
    input  wire [192*NUM_CH-1:0]           i_pos_cnt,
    input  wire [192*NUM_CH-1:0]           i_neg_cnt,
    input  wire [32*NUM_CH-1:0]          i_frame_n_pos, i_frame_n_neg,
    input  wire [16*NUM_CH-1:0]          i_frame_ad_max, i_frame_ad_min,
    input  wire [32*NUM_CH-1:0]          i_frame_id,
    input  wire [2*NUM_CH-1:0]           i_frame_stat,
    // 契约 v3.0 §5.3 APPLY_STATUS 数据源（来自各通道 core）
    input  wire [NUM_CH-1:0]             i_apply_done,     // 各通道 apply 完成脉冲
    input  wire [NUM_CH-1:0]             i_apply_pending,  // 各通道仍有未应用的 shadow 配置

    // ================= PRPD 读端口 =================
    output wire [PH_W-1:0]               prpd_ps_addr,
    input  wire [16*NUM_CH-1:0]          prpd_ps_rdata,

    // ================= 全局 =================
    output wire [31:0]                   o_gctrl,
    output wire [31:0]                   o_girq_en,
    output wire [NUM_CH-1:0]             o_frame_ack,   // v3.0: FRAME_ACK[0] 脉冲(清 frame_valid)
    output wire [NUM_CH-1:0]             o_frame_ovf_ack, // v3.0: FRAME_ACK[1] 脉冲(清 overflow)
    input  wire [31:0]                   i_fifo_count,
    input  wire [63:0]                   i_event_count,

    output wire                          irq
);

    localparam CH_SEL_W = (NUM_CH <= 2) ? 1 : (NUM_CH <= 4) ? 2 : 3;

    // =========================================================================
    // 寄存器存储
    // =========================================================================
    reg [31:0] r_ctrl       [0:NUM_CH-1];
    reg [31:0] r_cfg0       [0:NUM_CH-1];
    reg [31:0] r_phase_inc  [0:NUM_CH-1];
    reg [31:0] r_thresh     [0:NUM_CH-1];
    reg [31:0] r_scale      [0:NUM_CH-1];
    reg [31:0] r_upeak      [0:NUM_CH-1];
    reg [31:0] r_deadtime   [0:NUM_CH-1];
    reg [31:0] r_meas_cyc   [0:NUM_CH-1];
    reg [31:0] r_irq_en     [0:NUM_CH-1];
    // ---- v2: PRPD 框选剔除区域 (REG0 / REG1) ----
    reg [31:0] r_reg0_cfg   [0:NUM_CH-1];   // 0x4C
    reg [31:0] r_reg0_phase [0:NUM_CH-1];   // 0x50
    reg [31:0] r_reg0_q     [0:NUM_CH-1];   // 0x54
    reg [31:0] r_reg1_cfg   [0:NUM_CH-1];   // 0x60 (清单补充)
    reg [31:0] r_reg1_phase [0:NUM_CH-1];   // 0x58
    reg [31:0] r_reg1_q     [0:NUM_CH-1];   // 0x5C
    reg [15:0] r_pos_thr [0:5], r_neg_thr [0:5];
    reg        apply_p;
    // ---- 契约 v3.0 §5.3 APPLY_STATUS 状态 ----
    //   applied_ch[i]: 通道 i 已应用本次 shadow 配置（收到 i_apply_done[i] 后置位，
    //                  新的 apply 请求到来时整体清零）
    //   apply_seq    : 四通道全部应用成功后自增（16 bit 自然回绕）
    reg [NUM_CH-1:0] applied_ch;
    reg [15:0]       apply_seq;
    // ---- 契约 v3.0 §5.4 FRAME_ACK（计数页 0x48）写脉冲 ----
    (* max_fanout = 64 *) reg [NUM_CH-1:0] frame_ack_p, frame_ovf_ack_p;

    // 双路扇出: max_fanout 触发综合器复制, 把 890 路驱动分摊到 ~14 个副本,
    // 消除 LUT1 驱动的可控硅式(net fanout)延迟, 同时打散 control set 拥塞。
    (* max_fanout = 64 *) reg [31:0] r_gctrl, r_girq_en;

    reg [NUM_CH-1:0] irq_done, irq_ovf;
    reg [NUM_CH-1:0] meas_done_d;

    integer c;

    // 说明: irq_done / irq_ovf / meas_done_d 的中断置位逻辑已合并到下方
    //       "写寄存器" always 块中, 避免同一 reg 被多个 always 块驱动
    //       (multi-driven net 综合错误)。所有寄存器的复位也统一在该块内。

    // ---- 配置输出打包 ----
    genvar gi;
    generate
        for (gi = 0; gi < NUM_CH; gi = gi + 1) begin : GEN_CFG_OUT
            assign o_enable[gi]              = r_ctrl[gi][0] & r_gctrl[0];
            assign o_mode[2*gi +: 2]         = r_cfg0[gi][1:0];
            assign o_phase_win[32*gi +: 32]  = {16'd0, r_cfg0[gi][31:16]};
            assign o_phase_inc[32*gi +: 32]  = r_phase_inc[gi];
            assign o_thresh[32*gi +: 32]     = r_thresh[gi];
            assign o_scale[32*gi +: 32]      = r_scale[gi];
            assign o_upeak[32*gi +: 32]      = r_upeak[gi];
            assign o_deadtime[32*gi +: 32]   = r_deadtime[gi];
            assign o_meas_cycles[32*gi +: 32]= r_meas_cyc[gi];
            assign o_auto_clear[gi]          = r_ctrl[gi][2];
            assign o_ev_all[gi]              = r_ctrl[gi][3];
            assign o_prpd_max[gi]            = r_ctrl[gi][4];

            // v2: 剔除区域 (全 32bit 输出, core 侧取所需位段)
            assign o_reg0_cfg[4*gi +: 4]     = r_reg0_cfg[gi][3:0];
            assign o_reg1_cfg[4*gi +: 4]     = r_reg1_cfg[gi][3:0];
            assign o_reg0_phase[24*gi +: 24] = r_reg0_phase[gi][23:0];
            assign o_reg1_phase[24*gi +: 24] = r_reg1_phase[gi][23:0];
            assign o_reg0_q[32*gi +: 32]     = r_reg0_q[gi];
            assign o_reg1_q[32*gi +: 32]     = r_reg1_q[gi];
        end
    endgenerate

    assign o_gctrl   = r_gctrl;
    assign o_girq_en = r_girq_en;
    assign o_apply   = apply_p;
    generate
        for (gi = 0; gi < 6; gi = gi + 1) begin : GEN_THR_OUT
            assign o_pos_thr[16*gi +: 16] = r_pos_thr[gi];
            assign o_neg_thr[16*gi +: 16] = r_neg_thr[gi];
        end
    endgenerate

    // =========================================================================
    // AXI 握手
    // =========================================================================
    reg                          axi_awready, axi_wready, axi_bvalid;
    reg                          axi_arready, axi_rvalid;
    reg [C_S_AXI_ADDR_WIDTH-1:0] axi_awaddr, axi_araddr;
    reg [1:0]                    axi_bresp, axi_rresp;
    reg                          aw_en;

    assign S_AXI_AWREADY = axi_awready;
    assign S_AXI_WREADY  = axi_wready;
    assign S_AXI_BRESP   = axi_bresp;
    assign S_AXI_BVALID  = axi_bvalid;
    assign S_AXI_ARREADY = axi_arready;
    assign S_AXI_RRESP   = axi_rresp;
    assign S_AXI_RVALID  = axi_rvalid;

    // ---- 写地址通道 ----
    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            axi_awready <= 1'b0; aw_en <= 1'b1;
        end else begin
            if (~axi_awready && S_AXI_AWVALID && S_AXI_WVALID && aw_en) begin
                axi_awready <= 1'b1; aw_en <= 1'b0;
            end else if (S_AXI_BREADY && axi_bvalid) begin
                axi_awready <= 1'b0; aw_en <= 1'b1;
            end else begin
                axi_awready <= 1'b0;
            end
        end
    end

    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN)
            axi_awaddr <= {C_S_AXI_ADDR_WIDTH{1'b0}};
        else if (~axi_awready && S_AXI_AWVALID && S_AXI_WVALID && aw_en)
            axi_awaddr <= S_AXI_AWADDR;
    end

    // ---- 写数据通道 ----
    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN)
            axi_wready <= 1'b0;
        else if (~axi_wready && S_AXI_WVALID && S_AXI_AWVALID && aw_en)
            axi_wready <= 1'b1;
        else
            axi_wready <= 1'b0;
    end

    wire slv_wren = axi_wready && S_AXI_WVALID && axi_awready && S_AXI_AWVALID;

    // ---- 写响应通道 ----
    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            axi_bvalid <= 1'b0; axi_bresp <= 2'b00;
        end else begin
            if (axi_awready && S_AXI_AWVALID && ~axi_bvalid && axi_wready && S_AXI_WVALID) begin
                axi_bvalid <= 1'b1; axi_bresp <= 2'b00;      // OKAY
            end else if (S_AXI_BREADY && axi_bvalid) begin
                axi_bvalid <= 1'b0;
            end
        end
    end

    // ---- 读地址通道 ----
    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            axi_arready <= 1'b0;
            axi_araddr  <= {C_S_AXI_ADDR_WIDTH{1'b0}};
        end else begin
            if (~axi_arready && S_AXI_ARVALID) begin
                axi_arready <= 1'b1;
                axi_araddr  <= S_AXI_ARADDR;
            end else begin
                axi_arready <= 1'b0;
            end
        end
    end

    always @(posedge S_AXI_ACLK) begin
        if (!S_AXI_ARESETN) begin
            axi_rvalid <= 1'b0; axi_rresp <= 2'b00;
        end else begin
            if (axi_arready && S_AXI_ARVALID && ~axi_rvalid) begin
                axi_rvalid <= 1'b1; axi_rresp <= 2'b00;
            end else if (axi_rvalid && S_AXI_RREADY) begin
                axi_rvalid <= 1'b0;
            end
        end
    end

    // =========================================================================
    // 写寄存器
    // =========================================================================
    // sw_rst_p 旧版扇出 926(经 u_regs 衍生后, 全局复位网 756), 用 max_fanout
    // 强制复制驱动, 避免 reset distribution 单一节点拥塞。
    (* max_fanout = 64 *) reg [NUM_CH-1:0] sw_rst_p, prpd_clr_p;
    reg [31:0]       wdata;
    reg [CH_SEL_W-1:0] wch;
    reg [4:0]          widx;
    reg [1:0]          cnt_wch;
    reg [3:0]          cnt_widx;

    always @(posedge S_AXI_ACLK or negedge S_AXI_ARESETN) begin
        if (!S_AXI_ARESETN) begin
            sw_rst_p   <= {NUM_CH{1'b0}};
            prpd_clr_p <= {NUM_CH{1'b0}};
            r_gctrl    <= 32'h0000_0001;
            r_girq_en  <= 32'd0;
            irq_done    <= {NUM_CH{1'b0}};
            irq_ovf     <= {NUM_CH{1'b0}};
            meas_done_d <= {NUM_CH{1'b0}};
            apply_p <= 1'b0;
            applied_ch      <= {NUM_CH{1'b0}};
            apply_seq       <= 16'd0;
            frame_ack_p     <= {NUM_CH{1'b0}};
            frame_ovf_ack_p <= {NUM_CH{1'b0}};
            for (c = 0; c < NUM_CH; c = c + 1) begin
                r_ctrl[c]      <= 32'h0000_0001;   // 默认使能
                r_cfg0[c]      <= 32'h0400_0000;   // N = 1024, mode = ABSMAX
                r_phase_inc[c] <= `PD_PH_INC_1024; // 2^32 / 1024
                // The DDS background is +/-32 codes on CH2.  A 40-code
                // default makes nearly every phase window a false event and
                // paints a continuous ellipse arc.  The synthetic pulse
                // templates are hundreds of codes, so 80 rejects the
                // background while retaining the intended pulses.
                r_thresh[c]    <= 32'd80;          // 门限 80 LSB (DDS-safe)
                r_scale[c]     <= 32'd256;         // Q8.8 -> 1.0
                r_upeak[c]     <= 32'd2560;        // Q8.8 -> 10.0
                r_deadtime[c]  <= 32'd100;         // 5 us @20MSPS
                r_meas_cyc[c]  <= 32'd50;          // 50 周期 = 1 s @50Hz
                r_irq_en[c]    <= 32'd0;
                r_reg0_cfg[   c] <= 32'd0;         // 默认禁用剔除区域
                r_reg0_phase[c] <= 32'd0;
                r_reg0_q[     c] <= 32'd0;
                r_reg1_cfg[   c] <= 32'd0;
                r_reg1_phase[c] <= 32'd0;
                r_reg1_q[     c] <= 32'd0;
            end
            for (c = 0; c < 6; c = c + 1) begin
                r_pos_thr[c] <= 16'd40;
                r_neg_thr[c] <= 16'd40;
            end
        end else begin
            sw_rst_p   <= {NUM_CH{1'b0}};
            prpd_clr_p <= {NUM_CH{1'b0}};
            apply_p    <= 1'b0;
            frame_ack_p     <= {NUM_CH{1'b0}};
            frame_ovf_ack_p <= {NUM_CH{1'b0}};

            // ---- 契约 v3.0 §5.3 APPLY_STATUS ----
            //   收到新的 apply 请求 (apply_p, 由 SHADOW_APPLY 写产生) -> 清 applied_ch
            if (apply_p) applied_ch <= {NUM_CH{1'b0}};
            //   收到某通道 apply 完成 -> 置该通道 applied_ch 位
            for (c = 0; c < NUM_CH; c = c + 1) begin
                if (i_apply_done[c]) applied_ch[c] <= 1'b1;
            end
            //   四通道全部应用成功 -> apply_seq 自增 (16 bit 自然回绕)
            //   注意: 与 applied_ch 置位同拍比较时需并入本拍的 i_apply_done
            if ((applied_ch | i_apply_done) == {NUM_CH{1'b1}}) begin
                apply_seq <= apply_seq + 16'd1;
            end

            // ---- 中断置位逻辑（每个时钟沿检查, 不依赖 AXI 写） ----
            meas_done_d <= i_meas_done;
            for (c = 0; c < NUM_CH; c = c + 1) begin
                if (i_meas_done[c] && !meas_done_d[c]) irq_done[c] <= 1'b1;
                if (i_overflow[c])                     irq_ovf[c]  <= 1'b1;
            end

            if (slv_wren) begin
                wdata = S_AXI_WDATA;
                wch   = axi_awaddr[11:7];
                widx  = axi_awaddr[6:2];
                cnt_wch = axi_awaddr[7:6];
                cnt_widx = axi_awaddr[5:2];

                case (axi_awaddr[15:12])
                    // ---------------- 通道区 ----------------
                    4'h0: begin
                        if (wch < NUM_CH) begin
                            case (widx)
                                5'd0: begin
                                    r_ctrl[wch] <= wdata;
                                    if (wdata[1]) sw_rst_p[wch]   <= 1'b1;
                                    if (wdata[5]) prpd_clr_p[wch] <= 1'b1;
                                end
                                5'd1:  r_cfg0[wch]      <= wdata;
                                5'd2:  r_phase_inc[wch] <= wdata;
                                5'd3:  r_thresh[wch]    <= wdata;
                                5'd4:  r_scale[wch]     <= wdata;
                                5'd5:  r_upeak[wch]     <= wdata;
                                5'd6:  r_deadtime[wch]  <= wdata;
                                5'd7:  r_meas_cyc[wch]  <= wdata;
                                5'd17: begin                       // 0x44 IRQ_STAT W1C
                                    if (wdata[0]) irq_done[wch] <= 1'b0;
                                    if (wdata[1]) irq_ovf[wch]  <= 1'b0;
                                end
                                5'd18: r_irq_en[wch] <= wdata;     // 0x48 IRQ_EN
                                5'd19: r_reg0_cfg[   wch] <= wdata; // 0x4C
                                5'd20: r_reg0_phase[wch] <= wdata; // 0x50
                                5'd21: r_reg0_q[     wch] <= wdata; // 0x54
                                5'd22: r_reg1_phase[wch] <= wdata; // 0x58
                                5'd23: r_reg1_q[     wch] <= wdata; // 0x5C
                                5'd24: r_reg1_cfg[   wch] <= wdata; // 0x60 (清单补充)
                                default: ;
                            endcase
                        end
                    end
                    // ---------------- 全局区 ----------------
                    4'h1: begin
                        case (axi_awaddr[9:2])
                            8'd0: r_gctrl <= wdata;
                            8'd2: begin
                                      if (wdata[0]) irq_done <= {NUM_CH{1'b0}};
                                      if (wdata[1]) irq_ovf  <= {NUM_CH{1'b0}};
                                  end
                            8'd3: r_girq_en <= wdata;
                            8'd9:  r_pos_thr[0] <= wdata[15:0];
                            8'd10: r_pos_thr[1] <= wdata[15:0];
                            8'd11: r_pos_thr[2] <= wdata[15:0];
                            8'd12: r_pos_thr[3] <= wdata[15:0];
                            8'd13: r_pos_thr[4] <= wdata[15:0];
                            8'd14: r_pos_thr[5] <= wdata[15:0];
                            8'd16: r_neg_thr[0] <= wdata[15:0];
                            8'd17: r_neg_thr[1] <= wdata[15:0];
                            8'd18: r_neg_thr[2] <= wdata[15:0];
                            8'd19: r_neg_thr[3] <= wdata[15:0];
                            8'd20: r_neg_thr[4] <= wdata[15:0];
                            8'd21: r_neg_thr[5] <= wdata[15:0];
                            8'd24: if (wdata[0]) apply_p <= 1'b1;
                            default: ;
                        endcase
                    end
                    4'h2: begin
                        // 计数 / 帧快照区只读；
                        // 唯一例外：0x48 FRAME_ACK 为 W1P（契约 v3.0 §5.4）
                        //   bit0 = 1 -> 确认已读，清 frame_valid
                        //   bit1 = 1 -> 记录丢帧，清 overflow
                        if (wch < NUM_CH && widx == 5'd18) begin
                            if (wdata[0]) frame_ack_p[wch]     <= 1'b1;
                            if (wdata[1]) frame_ovf_ack_p[wch] <= 1'b1;
                        end
                    end
                    default: ;              // PRPD 区只读
                endcase
            end
        end
    end

    assign o_sw_rst     = sw_rst_p;
    assign o_prpd_clear = prpd_clr_p;

    // =========================================================================
    // 读寄存器
    // =========================================================================
    // 地址提前一拍送出, 以匹配 PRPD RAM 的 1 拍同步读延迟
    wire [C_S_AXI_ADDR_WIDTH-1:0] ar_now   = S_AXI_ARVALID ? S_AXI_ARADDR : axi_araddr;
    wire [CH_SEL_W-1:0]           rch      = ar_now[11:7];
    wire [4:0]                    ridx     = ar_now[6:2];
    // PRPD 区: 0x4000 + ch*0x1000  ->  addr[15:12] = 4 .. 4+NUM_CH-1
    wire [CH_SEL_W-1:0]           prpd_ch  = ar_now[15:12] - 4'd4;

    assign prpd_ps_addr = ar_now[11:2];

    reg [31:0] rdata_r;
    reg        rdata_is_prpd;

    always @* begin
        rdata_r       = 32'd0;
        rdata_is_prpd = 1'b0;

        if (ar_now[15:12] == 4'h0) begin
            // ---------------- 通道区 ----------------
            begin
                if (rch < NUM_CH) begin
                    case (ridx)
                        5'd0:  rdata_r = r_ctrl[rch];
                        5'd1:  rdata_r = r_cfg0[rch];
                        5'd2:  rdata_r = r_phase_inc[rch];
                        5'd3:  rdata_r = r_thresh[rch];
                        5'd4:  rdata_r = r_scale[rch];
                        5'd5:  rdata_r = r_upeak[rch];
                        5'd6:  rdata_r = r_deadtime[rch];
                        5'd7:  rdata_r = r_meas_cyc[rch];
                        5'd8:  rdata_r = {29'd0, i_overflow[rch], irq_done[rch],
                                          i_sync_locked[rch]};
                        5'd9:  rdata_r = i_sync_period[32*rch +: 32];
                        5'd10: rdata_r = i_n[32*rch +: 32];
                        5'd11: rdata_r = i_qmax[32*rch +: 32];
                        5'd12: rdata_r = i_sum_abs_q[64*rch +: 32];
                        5'd13: rdata_r = i_sum_abs_q[64*rch+32 +: 32];
                        5'd14: rdata_r = i_sum_qu[64*rch +: 32];
                        5'd15: rdata_r = i_sum_qu[64*rch+32 +: 32];
                        5'd16: rdata_r = i_live_cycles[32*rch +: 32];
                        5'd17: rdata_r = {30'd0, irq_ovf[rch], irq_done[rch]};
                        5'd18: rdata_r = r_irq_en[rch];
                        5'd19: rdata_r = r_reg0_cfg[   rch];
                        5'd20: rdata_r = r_reg0_phase[rch];
                        5'd21: rdata_r = r_reg0_q[     rch];
                        5'd22: rdata_r = r_reg1_phase[rch];
                        5'd23: rdata_r = r_reg1_q[     rch];
                        5'd24: rdata_r = r_reg1_cfg[   rch];
                        default: rdata_r = 32'd0;
                    endcase
                end
            end
        end else if (ar_now[15:12] == 4'h1) begin
            // ---------------- 全局区 (0x1000 ~ 0x1FFF) ----------------
            begin
                case (ar_now[9:2])
                    8'd0: rdata_r = r_gctrl;
                    8'd1: rdata_r = {{(32-NUM_CH){1'b0}}, i_sync_locked};
                    8'd2: rdata_r = {{(32-NUM_CH){1'b0}}, irq_done | irq_ovf};
                    8'd3: rdata_r = r_girq_en;
                    8'd4: rdata_r = VERSION;
                    8'd5: rdata_r = NUM_CH[31:0];
                    8'd6: rdata_r = i_fifo_count;
                    8'd7: rdata_r = i_event_count[31:0];
                    8'd8: rdata_r = i_event_count[63:32];
                    8'd9,8'd10,8'd11,8'd12,8'd13,8'd14:
                        rdata_r = {16'd0, r_pos_thr[ar_now[9:2] - 8'd9]};
                    8'd16,8'd17,8'd18,8'd19,8'd20,8'd21:
                        rdata_r = {16'd0, r_neg_thr[ar_now[9:2] - 8'd16]};
                    // ---- 契约 v3.0 §5.3 新增两个只读寄存器 ----
                    8'd22: rdata_r = 32'd26000000;      // 0x1058 SAMPLE_RATE_HZ
                    // 0x105C APPLY_STATUS (契约 v3.0 §5.3 / §11.5, 字段已冻结):
                    //   bits3:0   pending_ch  每通道待应用标志
                    //   bits7:4   applied_ch  每通道已应用标志
                    //   bits15:8  reserved    读零
                    //   bits31:16 apply_seq   四通道全部应用后自增
                    8'd23: rdata_r = {apply_seq, 8'd0, applied_ch, i_apply_pending};
                    default: rdata_r = 32'd0;
                endcase
            end
        end else if (ar_now[15:12] == 4'h2) begin
            // 0x2000 + ch*0x80: 计数器 + 帧快照 (每通道 128 字节)
            //   页内: [11:7] 选通道(0x80 步长), [6:2] 选页内寄存器
            if (ar_now[11:7] < NUM_CH) begin
                case (ar_now[6:2])
                    5'd0,5'd1,5'd2,5'd3,5'd4,5'd5: rdata_r = i_pos_cnt[192*(ar_now[11:7]) + 32*(ar_now[6:2]) +: 32];
                    5'd6,5'd7,5'd8,5'd9,5'd10,5'd11: rdata_r = i_neg_cnt[192*(ar_now[11:7]) + 32*(ar_now[6:2]-5'd6) +: 32];
                    5'd12: rdata_r = i_frame_n_pos[32*(ar_now[11:7]) +: 32];
                    5'd13: rdata_r = i_frame_n_neg[32*(ar_now[11:7]) +: 32];
                    5'd14: rdata_r = {16'd0, i_frame_ad_max[16*(ar_now[11:7]) +: 16]};
                    5'd15: rdata_r = {16'd0, i_frame_ad_min[16*(ar_now[11:7]) +: 16]};
                    5'd16: rdata_r = i_frame_id[32*(ar_now[11:7]) +: 32];      // 0x40
                    5'd17: rdata_r = {30'd0, i_frame_stat[2*(ar_now[11:7]) +: 2]}; // 0x44
                    default: rdata_r = 32'd0;
                endcase
            end
        end else if (ar_now[15:12] >= 4'd4 &&
                     ar_now[15:12] <  (4'd4 + NUM_CH[3:0])) begin
            // ---------------- PRPD 区 (只读, 同步 RAM 1 拍延迟) ----------------
            rdata_is_prpd = 1'b1;
        end
    end

    assign S_AXI_RDATA = rdata_is_prpd ?
                         {16'd0, prpd_ps_rdata[16*prpd_ch +: 16]} : rdata_r;

    // ---- 契约 v3.0 §5.4：FRAME_STAT(0x44) 改为**只读** ----
    //   旧语义"读 0x44 即清 frame_valid"作废：PS 在读 FRAME_ID 与读快照之间
    //   若被中断，frame_valid 已被清掉，"复读 ID 比对"的一致性读流程无法实现。
    //   现改为写 FRAME_ACK(0x48, W1P) 显式确认：
    //     bit0 -> o_frame_ack     (清 frame_valid)
    //     bit1 -> o_frame_ovf_ack (清 overflow)
    assign o_frame_ack     = frame_ack_p;
    assign o_frame_ovf_ack = frame_ovf_ack_p;

    // =========================================================================
    // 中断聚合
    // =========================================================================
    wire [NUM_CH-1:0] ch_irq;
    generate
        for (gi = 0; gi < NUM_CH; gi = gi + 1) begin : GEN_IRQ
            assign ch_irq[gi] = (irq_done[gi] & r_irq_en[gi][0]) |
                                (irq_ovf[gi]  & r_irq_en[gi][1]);
        end
    endgenerate
    assign irq = |ch_irq;

endmodule
