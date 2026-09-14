// =============================================================================
// pd_feature_core.v  --  单通道局部放电特征提取核心
// -----------------------------------------------------------------------------
// 功能: 移植现有 STM32/EP2C5 的 "分段取峰值 -> 1024 相位窗 -> n/I/P/Q 统计"
//       算法到 Zynq PL, 补充 AXI 寄存器配置与 8 字节事件打包。
//
// 关键设计说明:
//  1. 相位分频采用 Bresenham 增量法: 相位索引 = (样本序号 * N) / M
//     (M = 实测同步周期内的 ADC 样本数, N = 每周期相位窗数)
//     -> 无需除法器, 自动适配 50~400Hz 任意同步频率, 窗口边界误差 < 1 点。
//       50Hz @20MSPS: M = 400000, N = 1024 -> 窗口 390/391 点, 与原算法 391 一致。
//  2. 试验电压瞬时值 u 由 32bit 相位累加 + 1024 点正弦 ROM 查表得到,
//     相位增量 = 2^32/N 由 PS 计算后写入寄存器, PL 侧零除法开销。
//  3. 累加器输出 Σ|q| 与 Σq·u; I = Σ|q|/T、P = Σq·u/T 的除法由 PS 完成
//     (PL 不做除法, 省资源, 精度由 PS 侧浮点保证)。
//  4. 【路线 B 的固有局限】死区(脉冲分辨时间)只能在"窗口级"实现, 因为本
//     路线的数据已经是每窗口一个峰值。样本级死区需在全速域方案中实现。
//  5. 时钟域: 本模块为单时钟设计(clk, 典型 100MHz), adc_dv 为 ADC 样本有效
//     脉冲。真实板级接入 20MHz ADC 时钟时, 需在前端插入异步 FIFO 做 CDC,
//     见 pd_feature_top.v 头部说明。
// =============================================================================
`timescale 1ns / 1ps
`include "pd_defines.vh"

module pd_feature_core #(
    parameter integer CH_ID      = 0,
    parameter integer ADC_W      = `PD_ADC_W,
    parameter integer PH_W       = `PD_PH_W,
    parameter integer TS_W       = `PD_TS_W,
    parameter integer EV_W       = `PD_EV_W
)(
    input  wire                 clk,
    input  wire                 rst_n,

    // ---------------- ADC 接口 (offset binary, 中值 0x800) ----------------
    input  wire [ADC_W-1:0]     adc_data,
    input  wire                 adc_dv,

    // ---------------- 同步输入 (板级整形后的 3.3V 方波) ----------------
    input  wire                 sync_in,

    // ---------------- 配置 (来自 AXI-Lite 寄存器) ----------------
    input  wire                 cfg_enable,      // 通道使能
    input  wire [1:0]           cfg_mode,        // 峰值提取模式
    input  wire [PH_W-1:0]      cfg_phase_win,   // 每周期相位窗数 N (<=2048)
    input  wire [31:0]          cfg_phase_inc,   // 2^32/N, u 相位累加步长
    input  wire [15:0]          cfg_thresh,      // 峰值门限 (ADC 码)
    input  wire [6*16-1:0]      cfg_pos_thr,
    input  wire [6*16-1:0]      cfg_neg_thr,
    input  wire                  cfg_apply,
    // 帧快照显式确认（契约 v3.0 §5.4：FRAME_STAT 只读，改用 FRAME_ACK 显式确认）
    input  wire                  cfg_frame_ack,      // bit0 确认已读 -> 清 frame_valid
    input  wire                  cfg_frame_ovf_ack,  // bit1 记录丢帧 -> 清 overflow
    input  wire [15:0]          cfg_scale,       // q 标定系数 Q8.8 (码->pC)
    input  wire [15:0]          cfg_upeak,       // 试验电压峰值 Q8.8
    input  wire [15:0]          cfg_deadtime,    // 死区 (ADC 样本数, 0=禁用)
    input  wire [15:0]          cfg_meas_cycles, // 测量窗口长度(周期数, 0=不自动快照)
    input  wire                 cfg_auto_clear,  // 快照后自动清零累加器
    input  wire                 cfg_ev_all,      // 1=所有窗口输出事件; 0=仅超阈值
    input  wire                 cfg_prpd_max,    // 1=PRPD 取历史最大; 0=覆盖写

    // ---------------- v2: PRPD 框选剔除区域 (REG0/REG1) ----------------
    input  wire [3:0]           cfg_reg0_cfg,    // [0]en0 [1]sem0(0=不统计不显示,1=仅剔除PRPD显示)
    input  wire [23:0]          cfg_reg0_phase,  // [23:12]phi1 [11:0]phi2 (相位窗号)
    input  wire [31:0]          cfg_reg0_q,      // [31:16]q1 [15:0]q2 (Q8.8 pC)
    input  wire [3:0]           cfg_reg1_cfg,    // [0]en1 [1]sem1
    input  wire [23:0]          cfg_reg1_phase,  // [23:12]phi1 [11:0]phi2
    input  wire [31:0]          cfg_reg1_q,      // [31:16]q1 [15:0]q2

    // ---------------- 事件输出 (AXI-Stream, 8 字节/beat) ----------------
    output reg  [EV_W-1:0]      ev_tdata,
    output reg                  ev_tvalid,
    output reg                  ev_tlast,
    input  wire                 ev_tready,

    // ---------------- PRPD 图谱 RAM 接口 (同步读, 1 拍延迟) ----------------
    output reg  [PH_W-1:0]      prpd_addr,
    output reg  [15:0]          prpd_wdata,
    output reg                  prpd_we,
    input  wire [15:0]          prpd_rdata,

    // ---------------- 状态 / 结果 ----------------
    output reg  [31:0]          st_sync_period,  // 实测同步周期(ADC 样本数) M
    output reg                  st_sync_locked,  // 已完成至少一次有效同步
    output reg  [31:0]          st_sn_n,         // 快照: 放电脉冲计数 n
    output reg  [31:0]          st_sn_qmax,      // 快照: 最大视在电荷量 Q (pC)
    output reg  [63:0]          st_sn_sum_abs_q, // 快照: Σ|q|  -> I = Σ|q| / T
    output reg  [63:0]          st_sn_sum_qu,    // 快照: Σq·u  -> P = Σq·u / T
    output reg  [31:0]          st_live_cycles,  // 自上次清零以来累积的周期数
    output reg                  st_meas_done,    // 快照完成脉冲 (1 clk)
    output reg                  st_overflow,      // 窗口丢失标志 (软复位清除)
    output reg [6*32-1:0]        st_pos_cnt, st_neg_cnt,
    output reg [31:0]            st_frame_n_pos, st_frame_n_neg,
    output reg [15:0]            st_frame_ad_max, st_frame_ad_min,
    output reg [31:0]            st_frame_id,
    output reg [1:0]             st_frame_stat,
    output reg                   st_apply_done,
    // 契约 v3.0 §5.3 APPLY_STATUS：本通道是否仍有未应用的 shadow 配置
    output wire                  st_apply_pending
);

    // =========================================================================
    // 常数与工具函数
    // =========================================================================
    localparam signed [ADC_W-1:0] SMAX = {1'b0, {(ADC_W-1){1'b1}}};   //  2047
    localparam signed [ADC_W-1:0] SMIN = {1'b1, {(ADC_W-1){1'b0}}};   // -2048

    // 13bit 符号扩展绝对值（|−2048| = 2048 需 13bit 才不溢出）
    // 2026-09-10 提频用：原实现把 absp_max/absp_min 放在 S_PEAK 拍组合计算，
    // 与 q0_code/q0_sgn 的两级 case mux 串成一条 12 级、含 4×CARRY4 的长链，
    // 130MHz 下成为 pd_feature_core 的头号失败路径。现改为在 win_close 拍
    // 与 h_max/h_min **同拍同源**预寄存（源同为 wc_max/wc_min），既缩短链路
    // 又保证与 h_max/h_min 严格同拍、不存在时序错位。
    function [ADC_W:0] abs13;
        input signed [ADC_W-1:0] v;
        begin
            abs13 = v[ADC_W-1] ? -$signed({v[ADC_W-1], v}) : $signed({1'b0, v});
        end
    endfunction

    reg signed [ADC_W:0] absp_max_r, absp_min_r;  // 预寄存 |h_max| / |h_min|
    reg                  pol_sel_r;               // 预寄存 |h_max|>=|h_min|
    reg  [ADC_W-1:0]     q_pp_r;                  // 预寄存 峰峰值 h_max-h_min
    localparam [ADC_W-1:0]        ZERO_CODE = {1'b1, {(ADC_W-1){1'b0}}}; // 0x800

    // 事件/统计状态机状态编码
    // 注意: S_PEAK 之后插入 S_Q1/S_Q2/S_ACC 三级流水, 把
    //   h_max -> 模式选择 -> q0_sgn -> DSP 乘 -> sat16 -> 累加
    // 这条 34 级组合路径拆成每拍 <=10 级, 使 100MHz 时序可收敛。
    // S_ACC 承接原 S_PEAK 的"发事件 + 累加"动作, 语义不变。
    localparam S_IDLE = 4'd0, S_PEAK = 4'd1, S_Q1 = 4'd2, S_Q2 = 4'd3,
               S_ACC = 4'd4, S_PEAKN = 4'd5, S_PRRD = 4'd6, S_PRWR = 4'd7,
               S_CYC  = 4'd8, S_STAT = 4'd9;

    function signed [15:0] sat16;
        input signed [31:0] v;
        begin
            if      (v >  32'sd32767) sat16 =  16'sd32767;
            else if (v < -32'sd32768) sat16 = -16'sd32768;
            else                      sat16 = v[15:0];
        end
    endfunction

    // =========================================================================
    // 正弦 ROM: 1024 点 Q15 有符号, 用于生成试验电压瞬时值 u = Upk*sin(phi)
    // =========================================================================
    (* rom_style = "block" *) reg [15:0] sin_lut [0:1023];
    integer k;
    real    ang;
    initial begin : SIN_LUT_GEN
        for (k = 0; k < 1024; k = k + 1) begin
            ang = 6.28318530717958647692 * k / 1024.0;
            sin_lut[k] = $rtoi(32767.0 * $sin(ang));
        end
    end

    // =========================================================================
    // 1. 同步边沿检测
    // =========================================================================
    reg [2:0] sync_sr;
    always @(posedge clk or negedge rst_n)
        if (!rst_n) sync_sr <= 3'b000;
        else        sync_sr <= {sync_sr[1:0], sync_in};

    wire sync_rise = (sync_sr[2:1] == 2'b01);      // 上升沿 = 工频相位 0

    // =========================================================================
    // 2. 窗口采集状态
    // =========================================================================
    reg         [31:0] m_cnt;         // 本同步周期内已采样本数
    reg         [31:0] m_period;      // 上一周期实测样本数 M
    reg         [31:0] ph_acc;        // Bresenham 相位累加器
    reg [PH_W-1:0]     ph_idx;        // 当前相位窗索引 0..N-1
    reg         [31:0] u_phase;       // u 相位累加器
    reg         [31:0] g_sample_cnt;  // 全局样本计数(时间戳源)
    reg         [31:0] w_cnt;         // 当前窗口内样本数
    reg                w_valid;       // 窗口内已有样本
    reg signed [ADC_W-1:0] w_max, w_min;

    wire [ADC_W-1:0] sdata = adc_data - ZERO_CODE;   // 去直流中值, 转补码

    wire [31:0] ph_acc_nx = ph_acc + {{(32-PH_W){1'b0}}, cfg_phase_win};
    wire        ph_adv    = (ph_acc_nx >= m_period);  // 本样本触发窗口切换

    // 窗口收尾时锁存的"旧窗口"结果(必须在同拍保存, 否则会被新窗口覆盖)
    reg                    win_close, cyc_end;
    reg [PH_W-1:0]         wc_ph;
    reg signed [ADC_W-1:0] wc_max, wc_min;
    reg         [31:0]     wc_u;
    reg         [TS_W-1:0] wc_ts;
    reg                    win_lost;

    // 帧内 AD 波形包络极值(有符号 ADC 码)。每个窗口的 max/min 样本都参与比较
    // (覆盖全帧, 而非仅放电窗) —— 对应企业需求"放电 AD 最大最小值, 便于上位机
    // 做直流放电时域曲线绘图": 每帧取 [AD_MIN, AD_MAX] 竖线段, 逐帧连成包络。
    // (声明位置需早于下方窗口收尾块, 否则综合报"used before declaration")
    reg signed [ADC_W-1:0] frame_ad_max_acc, frame_ad_min_acc;

    reg                    win_pending, cyc_pending;
    reg                    armed;            // 已收到第一个有效同步沿
    reg                    arm_clr;          // 上膛脉冲: 清零预热期累积的 cyc_n/cyc_qmax
    reg signed [ADC_W-1:0] h_max, h_min;
    reg [PH_W-1:0]         h_ph;
    reg         [TS_W-1:0] h_ts;
    reg         [31:0]     h_u_phase;

    reg  [3:0] fsm;   // 事件/统计状态机(前置声明, 供握手信号使用)

    // FSM 握手信号(组合): 在 S_IDLE 取走一个待处理项
    wire fsm_take_win = (fsm == S_IDLE) &&  win_pending;
    wire fsm_take_cyc = (fsm == S_IDLE) && !win_pending && cyc_pending;

    task close_window;
        begin
            if (w_valid) begin
                // win_lost 置位条件: 收尾时上一窗口仍 pending(尚未被 FSM 取走)
                // = 丢窗。此处置为 win_pending 的电平, 由下方 sync 块在非
                // close 拍清零, FSM 块只读不清零(避免 multi-driven)。
                win_lost   <= win_pending;
                win_close <= 1'b1;
                wc_ph     <= ph_idx;
                wc_max    <= w_max;
                wc_min    <= w_min;
                wc_u      <= u_phase;                // 旧窗口的 u 相位
                wc_ts     <= g_sample_cnt[TS_W-1:0];
            end
        end
    endtask

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            m_cnt        <= 32'd0;
            m_period     <= `PD_M_DEFAULT;
            ph_acc       <= 32'd0;
            ph_idx       <= {PH_W{1'b0}};
            u_phase      <= 32'd0;
            g_sample_cnt <= 32'd0;
            w_cnt        <= 32'd0;
            w_valid      <= 1'b0;
            w_max        <= SMIN;   w_min   <= SMAX;
            win_close    <= 1'b0;   cyc_end <= 1'b0;
            wc_ph        <= {PH_W{1'b0}};
            wc_max       <= SMIN;   wc_min  <= SMAX;
            wc_u         <= 32'd0;  wc_ts   <= {TS_W{1'b0}};
            win_lost     <= 1'b0;
            armed        <= 1'b0;
            arm_clr      <= 1'b0;
            st_sync_locked <= 1'b0;
            st_sync_period <= `PD_M_DEFAULT;
        end else begin
            win_close <= 1'b0;
            cyc_end   <= 1'b0;
            arm_clr   <= 1'b0;
            win_lost  <= 1'b0;   // 默认清零, 仅在 close_window 拍被 win_pending 覆盖

            // 同步周期测量独立于通道使能, 保证 f_sync 始终可读
            if (adc_dv) m_cnt <= m_cnt + 32'd1;

            if (sync_rise) begin
                // ---- 同步上升沿 ----
                // m_cnt 过短说明这不是一个完整的工频周期(例如复位释放瞬间
                // sync 已为高而误检出的上升沿), 此时只复位相位、丢弃数据,
                // 不产生周期统计包, 避免污染 n / I / P。
                if (m_cnt > 32'd1024) begin
                    m_period       <= m_cnt;
                    st_sync_period <= m_cnt;
                    st_sync_locked <= 1'b1;

                    if (armed) begin
                        close_window;               // 收尾上一周期
                        cyc_end <= 1'b1;            // 输出周期统计包
                    end else begin
                        armed   <= 1'b1;            // 第一个有效沿仅"上膛"
                        arm_clr <= 1'b1;            // 清零预热期累积, 避免污染首周期 n/I/P
                    end
                end

                ph_acc  <= {{(32-PH_W){1'b0}}, cfg_phase_win};
                ph_idx  <= {PH_W{1'b0}};
                u_phase <= cfg_phase_inc;
                m_cnt   <= adc_dv ? 32'd1 : 32'd0;

                if (adc_dv && cfg_enable) begin
                    g_sample_cnt <= g_sample_cnt + 32'd1;
                    w_max <= sdata; w_min <= sdata; w_cnt <= 32'd1; w_valid <= 1'b1;
                end else begin
                    w_max <= SMIN;  w_min <= SMAX;  w_cnt <= 32'd0; w_valid <= 1'b0;
                end

            end else if (adc_dv && cfg_enable) begin
                g_sample_cnt <= g_sample_cnt + 32'd1;

                if (ph_adv) begin
                    close_window;
                    // 新窗口以本样本为首样本
                    w_max   <= sdata; w_min <= sdata; w_cnt <= 32'd1; w_valid <= 1'b1;
                    ph_acc  <= ph_acc_nx - m_period;
                    ph_idx  <= (ph_idx == (cfg_phase_win - 1'b1)) ?
                               {PH_W{1'b0}} : (ph_idx + 1'b1);
                    u_phase <= u_phase + cfg_phase_inc;
                end else begin
                    ph_acc <= ph_acc_nx;
                    w_cnt  <= w_cnt + 32'd1;
                    w_valid <= 1'b1;
                    if ($signed(sdata) > w_max) w_max <= sdata;
                    if ($signed(sdata) < w_min) w_min <= sdata;
                end
            end
        end
    end

    // ---------------- 窗口/周期待处理项锁存 ----------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            win_pending <= 1'b0;  cyc_pending <= 1'b0;
            h_max <= SMIN;  h_min <= SMAX;
            absp_max_r <= {ADC_W+1{1'b0}}; absp_min_r <= {ADC_W+1{1'b0}};
            pol_sel_r  <= 1'b1;            q_pp_r     <= {ADC_W{1'b0}};
            h_ph  <= {PH_W{1'b0}};
            h_ts  <= {TS_W{1'b0}};
            h_u_phase <= 32'd0;
        end else begin
            if (win_close) begin
                win_pending <= 1'b1;
                h_max <= wc_max; h_min <= wc_min;
                // 与 h_max/h_min 同拍同源预寄存后续所需的派生量（提频用，见 abs13 处注释）
                absp_max_r <= abs13(wc_max);
                absp_min_r <= abs13(wc_min);
                pol_sel_r  <= (abs13(wc_max) >= abs13(wc_min));
                q_pp_r     <= wc_max - wc_min;
                h_ph  <= wc_ph;  h_ts  <= wc_ts;
                h_u_phase <= wc_u;
                // 帧内 AD 波形包络极值: 全窗口参与(含无放电的噪声窗), 有符号比较。
                // 与放电事件解耦 —— 即使某帧一次放电都没检出, 也能给出该帧的
                // AD 上下边界, 供上位机画直流放电时域曲线。
                // ⚠ 2026-09-10 修正: 累积逻辑已移入下方帧块(block2)末尾。
                //   原因: 该 reg 若在本块与帧块同时驱动, 综合报
                //   [Synth 8-6859] multi-driven net (2nd driver = VCC), AD max/min 失效。
            end else if (fsm_take_win) begin
                win_pending <= 1'b0;
            end

            if (cyc_end) begin
                cyc_pending <= 1'b1;
            end else if (fsm_take_cyc) begin
                cyc_pending <= 1'b0;
            end
        end
    end

    // =========================================================================
    // 3. 幅度 / 极性 / 量化 / 门限 (组合逻辑)
    // =========================================================================
    // 幅值/极性/峰峰值：改用 win_close 拍预寄存版本（提频，见 abs13 处注释）。
    // 原实现在这里组合计算 absp_max/absp_min（2 个条件取反），
    // 再串 q0_code/q0_sgn 两级 case mux，构成 12 级长链。
    wire [ADC_W-1:0] q_abs   = (absp_max_r >= absp_min_r) ? absp_max_r[ADC_W-1:0]
                                                          : absp_min_r[ADC_W-1:0];
    wire             pol_sel = pol_sel_r;
    wire [ADC_W-1:0] q_pp    = q_pp_r;             // 峰峰值, 无符号, 0..4095

    // 第一拍(正峰)的 ADC 码与极性
    reg  [ADC_W-1:0] q0_code;
    reg              q0_pol;
    always @* begin
        case (cfg_mode)
            `PD_MODE_PP    : begin q0_code = q_pp_r;  q0_pol = `PD_POL_POS; end
            `PD_MODE_POSNEG: begin q0_code = h_max; q0_pol = `PD_POL_POS; end
            default        : begin q0_code = q_abs; q0_pol = pol_sel_r;     end
        endcase
    end
    wire [ADC_W-1:0] q1_code = h_min;              // 第二拍(负峰), 仅 POSNEG
    wire             q1_pol  = `PD_POL_NEG;

    // 量化 q(pC) = (q_signed * scale) >>> 8,  scale 为 Q8.8
    reg signed [ADC_W+1:0] q0_sgn, q1_sgn;
    always @* begin
        case (cfg_mode)
            `PD_MODE_ABSMAX:
                q0_sgn = q0_pol ? -$signed({{2{1'b0}}, q0_code})
                                :  $signed({{2{1'b0}}, q0_code});
            `PD_MODE_PP:
                q0_sgn = $signed({{2{1'b0}}, q0_code});     // 峰峰值恒为正
            default:  // POSNEG
                q0_sgn = $signed({{2{q0_code[ADC_W-1]}}, q0_code});
        endcase
        q1_sgn = $signed({{2{q1_code[ADC_W-1]}}, q1_code});
    end

    wire signed [31:0] q0_mul = q0_sgn * $signed({1'b0, cfg_scale});
    wire signed [31:0] q1_mul = q1_sgn * $signed({1'b0, cfg_scale});
    wire signed [15:0] q0_pc  = sat16(q0_mul >>> 8);
    wire signed [15:0] q1_pc  = sat16(q1_mul >>> 8);

    // -------------------------------------------------------------------------
    // 量化链流水线寄存器 (修复 3: 拆 34 级组合路径)
    //   S_PEAK 拍: 采样 模式/门限/死区/相位/时间戳 与 u_val
    //   S_Q1  拍: q0_sgn x cfg_scale (DSP48E1 内部流水)
    //   S_Q2  拍: sat16 量化 + 取绝对值
    //   S_ACC 拍: 发事件 + 累加 (原 S_PEAK 的动作)
    // 注意: 这些寄存器只在本通道 FSM 推进时更新, 对应值全部源于
    //       S_PEAK 时刻已稳定的 h_max/h_min/h_ph/h_ts/sin_q, 语义与
    //       原单拍组合完全一致, 仅延迟 2 拍。
    // -------------------------------------------------------------------------
    reg signed [ADC_W+1:0] q0_sgn_r, q1_sgn_r;
    // u_val_r: 试验电压瞬时值采样寄存器。值域论证(2026-09-08):
    //   u_val = (cfg_upeak[15:0]无符号 × sin_q[15:0]signed) >>> 15
    //   upeak 最大 65535(Q8.8=255.996) -> |u_val| ≤ 65535*32767>>15 ≈ 65534 < 2^17
    //   故 signed[17:0] 足够(原 33bit 过宽, 使 q×u 成 16×33 乘法被拆 2 级
    //   DSP48 级联, 130M 下 7.53ns 关键路径)。收窄后 18×16 单 DSP48E1。
    reg signed [17:0]      u_val_r;
    reg                    gate0_r, gate1_r, dead_ok_r;
    reg [PH_W-1:0]         h_ph_r;
    reg [TS_W-1:0]         h_ts_r;
    reg                    q0_pol_r, q1_pol_r;
    reg [31:0]             frame_npos_acc, frame_nneg_acc;
    // 12 档计数: 帧内实时累加器(内部) + 帧末快照输出 st_pos_cnt/st_neg_cnt。
    // 这样 12 档计数与 FRAME_N_POS/NEG/AD_MAX/MIN 同属一帧快照, PS 读到的数据
    // 一定自洽(旧版直接读实时累加器, 帧边界刚过读到几乎全 0, 与帧合计矛盾)。
    reg [6*32-1:0]         pos_cnt_live, neg_cnt_live;
    // 档位门槛的"周期边界生效"副本: PS 写 POS/NEG_THR 进 axil 影子寄存器,
    // 发 APPLY 后, 本模块在下一次周期边界(S_CYC)才一次性换档, 保证一帧之内
    // 门槛恒定(否则帧内换档会让同一帧的前后脉冲按不同门槛计数, 统计不自洽)。
    reg [6*16-1:0]         pos_thr_r, neg_thr_r;
    reg                    apply_pending;
    // 契约 v3.0 §5.3：把 pending 状态引到端口，供 top 汇总成 APPLY_STATUS
    assign st_apply_pending = apply_pending;
    reg [ADC_W-1:0]        amp0_r, amp1_r;                      // ADC 码幅度锁存 (S_PEAK 拍)
    integer                 ti;
    reg signed [31:0]      q0_mul_r, q1_mul_r;
    reg signed [15:0]      q0_pc_r, q1_pc_r;
    reg [15:0]             abs_q0_r, abs_q1_r;   // 见下方绝对值预寄存说明
    // S_Q2 拍量化的组合结果：q0_pc_r / abs_q0_r 必须由**同一组合值**同步载入，
    // 否则非阻塞赋值会让两者错开一拍（原注释的顾虑正是此点）。
    wire signed [15:0]     q0_pc_n = sat16(q0_mul_r >>> 8);
    wire signed [15:0]     q1_pc_n = sat16(q1_mul_r >>> 8);

    // ---- 12 档计数专用统计拍 (S_STAT) 暂存 ----
    // 事件拍(S_ACC/S_PEAKN)只发事件+累加 n/I/P, 把本窗口的 ADC 码幅度与极性暂存;
    // S_STAT 拍独立做 12 组门槛比较+回差+分档计数, 避免在 130M 关键路径上
    // 叠加 12 组比较逻辑(实测 130M WNS 仅 ~0.275ns)。
    // 单位约定: 门槛 cfg_pos_thr/neg_thr 与 AD 极值均为 ADC 码(与 THRESH 同域),
    //           非标定后的 pC —— 企业需求"放电 AD 最大最小值"即 AD 码。
    reg [ADC_W-1:0] stat_q0_c, stat_q1_c;   // 本窗口正/负峰 ADC 码幅度
    reg            stat_q0_pol, stat_q1_pol; // 本窗口极性 (0=正 1=负)
    reg            stat_v0, stat_v1;        // 本窗口 q0/q1 是否有效统计(超最低门槛且未剔除)

    // 绝对值在 S_ACC 拍由 q0_pc_r 组合得到(不能用寄存器: 非阻塞赋值会读到
    // 上一窗口的值)。q0_pc_r -> abs 只有 1~2 级 LUT, 不构成关键路径。
    // 绝对值改为**寄存器**，在 S_Q2 拍与 q0_pc_r/q1_pc_r 用同一个组合值同步载入
    // （2026-09-10 提频）。原实现放在 S_ACC/S_PEAK 拍组合计算，使
    //   q1_pc_r -> abs(16bit 条件取反, 4×CARRY4) -> 剔除区 2×16bit 比较
    //           -> keep_stat1 -> qu_pipe_r/CE
    // 成为 BD 级实现中 pd_feature_core 的最差路径（−0.707ns，布线占 70%）。
    // 注意: 必须与 q0_pc_r 用**同一组合表达式**载入，才能严格同拍、无错位。
    // （寄存器声明见上方 q0_pc_r 处）

    // =========================================================================
    // v2: PRPD 框选剔除区域判定 (组合, 基于 S_ACC/S_PEAKN 拍的流水寄存器)
    // 语义: sem=0 (默认) 不统计不显示; sem=1 保留统计、仅剔除 PRPD 显示 (预留)
    //   REG 相位域 = 相位窗号 [phi1, phi2];  电荷量域 = Q8.8 pC [q1, q2]
    //   h_ph_r  = 本窗口相位窗号 (12bit)   ✓
    //   abs_q0_r/abs_q1_r = Q8.8 幅值 (pC) ✓ 与 REG q 域同精度
    // 注意: 判定是组合逻辑且只依赖 S_ACC/S_PEAKN 拍已稳定的寄存器,
    //       未进入 64bit 累加器关键路径 (q0_pc_r -> 比较器 仅 1~2 级 LUT)。
    // =========================================================================
    // ---- q0 拍 (S_ACC, 正峰/单峰) ----
    wire in_r0_q0 = cfg_reg0_cfg[0] && (h_ph_r >= cfg_reg0_phase[23:12]) &&
                    (h_ph_r <= cfg_reg0_phase[11:0]) &&
                    (abs_q0_r >= cfg_reg0_q[31:16]) && (abs_q0_r <= cfg_reg0_q[15:0]);
    wire in_r1_q0 = cfg_reg1_cfg[0] && (h_ph_r >= cfg_reg1_phase[23:12]) &&
                    (h_ph_r <= cfg_reg1_phase[11:0]) &&
                    (abs_q0_r >= cfg_reg1_q[31:16]) && (abs_q0_r <= cfg_reg1_q[15:0]);
    wire in_rg_q0  = in_r0_q0 | in_r1_q0;
    wire drop_q0   = (in_r0_q0 & ~cfg_reg0_cfg[1]) | (in_r1_q0 & ~cfg_reg1_cfg[1]);
    wire keep_ev0   = ~drop_q0;    // 事件流使能
    wire keep_stat0 = ~drop_q0;    // n/I/P 累加使能 (含 last_trig 更新)
    wire keep_prpd0 = ~in_rg_q0;   // PRPD RAM 写使能 (两种语义都剔除)
    // ---- q1 拍 (S_PEAKN, 仅 POSNEG 模式) ----
    wire in_r0_q1 = cfg_reg0_cfg[0] && (h_ph_r >= cfg_reg0_phase[23:12]) &&
                    (h_ph_r <= cfg_reg0_phase[11:0]) &&
                    (abs_q1_r >= cfg_reg0_q[31:16]) && (abs_q1_r <= cfg_reg0_q[15:0]);
    wire in_r1_q1 = cfg_reg1_cfg[0] && (h_ph_r >= cfg_reg1_phase[23:12]) &&
                    (h_ph_r <= cfg_reg1_phase[11:0]) &&
                    (abs_q1_r >= cfg_reg1_q[31:16]) && (abs_q1_r <= cfg_reg1_q[15:0]);
    wire in_rg_q1  = in_r0_q1 | in_r1_q1;
    wire drop_q1   = (in_r0_q1 & ~cfg_reg0_cfg[1]) | (in_r1_q1 & ~cfg_reg1_cfg[1]);
    wire keep_ev1   = ~drop_q1;
    wire keep_stat1 = ~drop_q1;
    wire keep_prpd1 = ~in_rg_q1;

    // 试验电压瞬时值 u = Upk * sin(phi)
    wire [9:0]  u_addr = h_u_phase[31:22];
    reg  [15:0] sin_q;
    always @(posedge clk) sin_q <= sin_lut[u_addr];
    wire signed [32:0] u_val = ($signed({1'b0, cfg_upeak}) * $signed(sin_q)) >>> 15;

    // 门限: 按幅度比较(POSNEG 模式需取绝对值, 否则负峰会因补码被误判)
    // 同样改用 win_close 拍预寄存的绝对值（提频）
    wire [ADC_W-1:0] amp0 = (cfg_mode == `PD_MODE_POSNEG) ? absp_max_r[ADC_W-1:0] : q0_code;
    wire [ADC_W-1:0] amp1 = absp_min_r[ADC_W-1:0];
    wire gate0 = (amp0 >= cfg_thresh[ADC_W-1:0]);
    wire gate1 = (amp1 >= cfg_thresh[ADC_W-1:0]);

    // 死区(脉冲分辨时间): 与上次有效触发的样本间隔不足则抑制计数
    // 注: 路线 B 只能做到窗口级死区, 这是数据已被压缩的固有代价
    // 注意: 不要写 cfg_deadtime[TS_W-1:0] —— cfg_deadtime 只有 16bit,
    //       越界位选择会引入 x 使整个比较变成 x。这里让 Verilog 自动零扩展。
    reg [TS_W-1:0] last_trig;
    wire [TS_W-1:0] dt_gap = h_ts - last_trig;     // 模 2^24 回绕安全
    wire dead_ok = (cfg_deadtime == 16'd0) || (dt_gap >= cfg_deadtime);

    // =========================================================================
    // 4. 累加器与事件打包
    // =========================================================================
    reg         [31:0] acc_n, acc_qmax, acc_cycles;

    // 测量窗命中判据（2026-09-10 提频）：原式 `(acc_cycles + 32'd1) >= cfg_meas_cycles`
    // 把「32bit 加法 + 32bit 比较」串成 17 级 CARRY4，成为综合后 pd_feature_core
    // 的头号路径（+0.111ns，次紧路径尚有 +0.705ns，是明显离群项）。
    // 等价改写（K = cfg_meas_cycles）：
    //   acc+1 >= K  ⟺  acc >= K-1                （K >= 1）
    //   acc[31:16] != 0  ⇒  acc >= 65536 > K-1   （K <= 65535）→ 直接命中
    // 路径退化为「16bit 比较 + 16 输入或」，CARRY4 由 17 降到约 4。
    wire [15:0] meas_k_m1    = cfg_meas_cycles - 16'd1;
    wire        meas_win_hit = (cfg_meas_cycles != 16'd0) &&
                               ((|acc_cycles[31:16]) || (acc_cycles[15:0] >= meas_k_m1));
    reg         [63:0] acc_sum_abs_q;
    reg signed  [63:0] acc_sum_qu;
    reg         [31:0] cyc_n, cyc_qmax, cycle_idx;
    reg         [15:0] prpd_q_h;

    // ---- Σq·u 累加流水 (2026-09-08, 130M 关键路径拆分) ----
    // 原实现单拍串行 [q×u 乘法 + 64bit 累加] (130M 预检 WNS -1.056ns, 关键
    // 路径全落在 acc_sum_qu 链)。拆两拍:
    //   事件拍(S_ACC/S_PEAKN): 只把乘积锁存进 qu_pipe_r, 置 qu_acc_v=1;
    //   下一拍(S_PEAKN/S_PRRD):  把 qu_pipe_r 累加进 acc_sum_qu。
    // 事件链 S_ACC->(S_PEAKN)->S_PRRD->S_PRWR 原子执行, S_CYC 仅在 S_IDLE
    // 被取走 -> 进 S_CYC 前 pending 必清, 周期快照/清零无需顺延, 语义不变。
    reg signed  [63:0] qu_pipe_r;    // q×u 乘积暂存 (64bit, 与 acc_sum_qu 同宽)
    reg               qu_acc_v;      // 累加 pending

    // ---- 峰值事件包字段位宽 (对齐 A 接口文档 §6.2, 总宽恒 64bit) ----
    //   phase 字段宽度可切 10/12 (见 pd_defines.vh 的 PD_PH_FIELD_W),
    //   evt_seq 宽度随之联动, 保证拼接总宽不变。
    localparam PH_FIELD_W = `PD_PH_FIELD_W;      // 10 或 12
    localparam EVT_SEQ_W  = 37 - PH_FIELD_W;     // 27 或 25

    // 每通道事件序号: 每成功发出一个峰值事件 +1, 自然回绕(不饱和), 供 PS 侧丢帧检测
    reg [EVT_SEQ_W-1:0] evt_seq;

    // 事件包布局 (8 字节, 对齐 A §6.2):
    //   [63:56]  type      = 0x00 峰值事件
    //   [55:40]  q         16bit Q8.8 视在电荷量 pC, 有符号补码
    //   [39:30]  phase     PH_FIELD_W bit 相位窗号 (PH_FIELD_W=10 时 0~1023)
    //   [29]     polarity  0=负 1=正  (注意: 内部 q0_pol_r 为 0=正 1=负, 故取反)
    //   [28:27]  ch_id     通道号 0~3
    //   [26:0]   evt_seq   每通道自增事件序号
    // 注意: 用流水线寄存器版本, 仅在 S_ACC / S_PEAKN 拍采样, 值才是本窗口的。
    // 注意: timestamp(h_ts_r) 不再进事件包(phase+cycle_idx 已可还原绝对时间),
    //       但 h_ts_r 与死区逻辑必须保留 —— 死区判断仍依赖时间戳。
    wire [EV_W-1:0] ev0 = {`PD_EV_PEAK,               // [63:56]
                           q0_pc_r,                   // [55:40]
                           h_ph_r[PH_FIELD_W-1:0],    // phase
                           ~q0_pol_r,                 // polarity 0=负 1=正
                           CH_ID[1:0],                // ch_id
                           evt_seq};                  // evt_seq
    wire [EV_W-1:0] ev1 = {`PD_EV_PEAK,               // [63:56]
                           q1_pc_r,                   // [55:40]
                           h_ph_r[PH_FIELD_W-1:0],    // phase
                           ~q1_pol_r,                 // polarity 0=负 1=正
                           CH_ID[1:0],                // ch_id
                           evt_seq};                  // evt_seq
    // 周期统计包: ch_id 在 [31:30]（本包布局: type/cycle_idx/ch/cyc_n/qmax）。
    //   注意: 峰值包(ev0/ev1)的 ch_id 在 [26:25] —— 两种包型位宽不同, ch 不可能
    //   同位; 下游解析必须按 type 区分(ch 位置: 峰值 [26:25] / 周期 [31:30])。
    wire [EV_W-1:0] evc = {`PD_EV_CYCLE, cycle_idx[TS_W-1:0],
                           CH_ID[1:0], cyc_n[13:0], cyc_qmax[15:0]};

    // =========================================================================
    // 5. 事件/统计/PRPD 状态机
    // =========================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            fsm <= S_IDLE;
            ev_tvalid <= 1'b0; ev_tlast <= 1'b0; ev_tdata <= {EV_W{1'b0}};
            prpd_we   <= 1'b0; prpd_wdata <= 16'd0; prpd_addr <= {PH_W{1'b0}};
            acc_n <= 32'd0; acc_qmax <= 32'd0; acc_cycles <= 32'd0;
            acc_sum_abs_q <= 64'd0; acc_sum_qu <= 64'd0;
            cyc_n <= 32'd0; cyc_qmax <= 32'd0; cycle_idx <= 32'd0;
            evt_seq <= {EVT_SEQ_W{1'b0}};      // 事件序号清零
            st_sn_n <= 32'd0; st_sn_qmax <= 32'd0;
            st_sn_sum_abs_q <= 64'd0; st_sn_sum_qu <= 64'd0;
            st_live_cycles <= 32'd0;
            st_meas_done <= 1'b0; st_overflow <= 1'b0;
            prpd_q_h <= 16'd0;
            last_trig <= {TS_W{1'b1}};
            qu_pipe_r <= 64'sd0; qu_acc_v <= 1'b0;

            // 量化链流水线寄存器复位
            q0_sgn_r <= {(ADC_W+2){1'b0}}; q1_sgn_r <= {(ADC_W+2){1'b0}};
            u_val_r   <= 18'sd0;
            gate0_r   <= 1'b0; gate1_r <= 1'b0; dead_ok_r <= 1'b0;
            h_ph_r    <= {PH_W{1'b0}};
            h_ts_r    <= {TS_W{1'b0}};
            q0_pol_r  <= 1'b0; q1_pol_r <= 1'b0;
            amp0_r    <= {ADC_W{1'b0}}; amp1_r <= {ADC_W{1'b0}};
            q0_mul_r  <= 32'sd0; q1_mul_r <= 32'sd0;
            q0_pc_r   <= 16'sd0; q1_pc_r  <= 16'sd0;
            abs_q0_r  <= 16'd0;  abs_q1_r <= 16'd0;
            pos_cnt_live <= 0; neg_cnt_live <= 0;
            pos_thr_r <= {6{16'd40}}; neg_thr_r <= {6{16'd40}};   // 与 axil 复位默认值一致
            apply_pending <= 1'b0;
            frame_npos_acc <= 0; frame_nneg_acc <= 0; frame_ad_max_acc <= SMIN; frame_ad_min_acc <= SMAX;
            st_frame_n_pos <= 0; st_frame_n_neg <= 0; st_frame_ad_max <= 0; st_frame_ad_min <= 0; st_frame_id <= 0; st_frame_stat <= 0;
            st_pos_cnt <= 0; st_neg_cnt <= 0;
            stat_q0_c <= {ADC_W{1'b0}}; stat_q1_c <= {ADC_W{1'b0}};
            stat_q0_pol <= 1'b0; stat_q1_pol <= 1'b0;
            stat_v0 <= 1'b0; stat_v1 <= 1'b0;
        end else begin
            ev_tvalid    <= 1'b0;
            ev_tlast     <= 1'b0;
            prpd_we      <= 1'b0;
            st_meas_done <= 1'b0;
            st_apply_done <= 1'b0;
            // frame_valid/overflow 显式确认（契约 v3.0 §5.4）：
            //   FRAME_STAT(0x44) 只读，不再读清；由 FRAME_ACK(0x48) 的
            //   bit0 / bit1 分别清 frame_valid / overflow。
            if (cfg_frame_ack)     st_frame_stat[0] <= 1'b0;
            if (cfg_frame_ovf_ack) st_frame_stat[1] <= 1'b0;
            // APPLY: 只登记请求, 真正的换档在周期边界 S_CYC 完成
            if (cfg_apply) apply_pending <= 1'b1;

            if (win_lost) begin
                st_overflow <= 1'b1;
                // 注意: win_lost 是单拍脉冲, 由 sync 块在非 close 拍自动清零,
                //       此处不再清零, 避免与 sync 块 multi-driven。
            end

            // 上膛脉冲: 丢弃预热期(尚未完成首个同步)累积的周期内计数,
            // 否则首个周期统计包会把预热期的脉冲一并计入 n/I/P。
            if (arm_clr) begin
                cyc_n    <= 32'd0;
                cyc_qmax <= 32'd0;
            end

            case (fsm)
                // -------------------------------------------------------------
                S_IDLE: begin
                    if (fsm_take_win)      fsm <= S_PEAK;
                    else if (fsm_take_cyc) fsm <= S_CYC;
                end

                // -------------------------------------------------------------
                S_PEAK: begin
                    // 第 0 级流水: 采样由 h_max/h_min/sin_q 组合派生的中间量
                    q0_sgn_r  <= q0_sgn;
                    q1_sgn_r  <= q1_sgn;
                    u_val_r   <= u_val[17:0];        // 截断高位: 值域内高位恒为符号扩展, 无损
                    gate0_r   <= gate0;
                    gate1_r   <= gate1;
                    dead_ok_r <= dead_ok;
                    h_ph_r    <= h_ph;
                    h_ts_r    <= h_ts;
                    q0_pol_r  <= q0_pol;
                    q1_pol_r  <= q1_pol;
                    amp0_r    <= amp0;
                    amp1_r    <= amp1;
                    fsm       <= S_Q1;
                end

                // -------------------------------------------------------------
                S_Q1: begin
                    // 第 1 级流水: 幅度 x 标定系数 (DSP48E1, 内部流水寄存器)
                    q0_mul_r <= q0_sgn_r * $signed({1'b0, cfg_scale});
                    q1_mul_r <= q1_sgn_r * $signed({1'b0, cfg_scale});
                    fsm      <= S_Q2;
                end

                // -------------------------------------------------------------
                S_Q2: begin
                    // 第 2 级流水: 量化 (sat16) + 绝对值同步预寄存。
                    // q0_pc_r 与 abs_q0_r 由**同一组合值** q0_pc_n 载入 → 严格同拍，
                    // 不存在"非阻塞赋值读到旧一拍"的错位（原顾虑已由同源载入规避）。
                    q0_pc_r  <= q0_pc_n;
                    q1_pc_r  <= q1_pc_n;
                    abs_q0_r <= q0_pc_n[15] ? (-q0_pc_n) : q0_pc_n;
                    abs_q1_r <= q1_pc_n[15] ? (-q1_pc_n) : q1_pc_n;
                    fsm      <= S_ACC;
                end

                // -------------------------------------------------------------
                // S_ACC: 原 S_PEAK 的动作 (发正峰事件 + 累加 q0), 现在用流水结果
                S_ACC: begin
                    if (ev_tready) begin
                        if ((gate0_r || cfg_ev_all) && keep_ev0) begin   // ① 事件: 剔除区不输出
                            ev_tdata  <= ev0;
                            ev_tvalid <= 1'b1;
                            evt_seq   <= evt_seq + 1'b1;   // 事件序号(与累加器并行, 不入关键路径)
                        end
                        if (gate0_r && dead_ok_r && keep_stat0) begin    // ② 统计: 剔除区不累加
                            last_trig     <= h_ts_r;
                            acc_n         <= acc_n + 32'd1;
                            acc_sum_abs_q <= acc_sum_abs_q + {48'd0, abs_q0_r};
                            qu_pipe_r     <= q0_pc_r * u_val_r;  // 只锁存乘积, 下一拍累加
                            qu_acc_v      <= 1'b1;
                            if ({16'd0, abs_q0_r} > acc_qmax) acc_qmax <= {16'd0, abs_q0_r};
                            cyc_n         <= cyc_n + 32'd1;
                            if ({16'd0, abs_q0_r} > cyc_qmax) cyc_qmax <= {16'd0, abs_q0_r};
                            // 12 档计数/回差/frame 极值 移到 S_STAT 拍, 此处只暂存 ADC 码幅度
                            stat_q0_c   <= amp0_r;
                            stat_q0_pol <= q0_pol_r;
                            stat_v0     <= 1'b1;
                        end else begin
                            stat_v0 <= 1'b0;
                        end
                        // ③ PRPD: 剔除区写 0 (覆盖写模式即清除; max 模式保持旧值)
                        prpd_q_h <= keep_prpd0 ? abs_q0_r : 16'd0;
                        fsm <= (cfg_mode == `PD_MODE_POSNEG) ? S_PEAKN : S_STAT;
                    end
                end

                // -------------------------------------------------------------
                S_PEAKN: begin
                    if (ev_tready) begin
                        // 先处理 S_ACC 拍锁存的 Σq·u 乘积 (独立一拍累加)
                        if (qu_acc_v) begin
                            acc_sum_qu <= acc_sum_qu + qu_pipe_r;
                            qu_acc_v   <= 1'b0;
                        end
                        if ((gate1_r || cfg_ev_all) && keep_ev1) begin   // ① 事件
                            ev_tdata  <= ev1;
                            ev_tvalid <= 1'b1;
                            evt_seq   <= evt_seq + 1'b1;   // 事件序号
                        end
                        if (gate1_r && dead_ok_r && keep_stat1) begin    // ② 统计
                            last_trig     <= h_ts_r;
                            acc_n         <= acc_n + 32'd1;
                            acc_sum_abs_q <= acc_sum_abs_q + {48'd0, abs_q1_r};
                            qu_pipe_r     <= q1_pc_r * u_val_r;  // 只锁存乘积, 下一拍累加
                            qu_acc_v      <= 1'b1;
                            if ({16'd0, abs_q1_r} > acc_qmax) acc_qmax <= {16'd0, abs_q1_r};
                            cyc_n         <= cyc_n + 32'd1;
                            if ({16'd0, abs_q1_r} > cyc_qmax) cyc_qmax <= {16'd0, abs_q1_r};
                            // 12 档计数移到 S_STAT, 此处暂存负峰 ADC 码幅度
                            stat_q1_c   <= amp1_r;
                            stat_q1_pol <= q1_pol_r;
                            stat_v1     <= 1'b1;
                        end else begin
                            stat_v1 <= 1'b0;
                        end
                        // ③ PRPD: 剔除区不更新
                        if (keep_prpd1 && (abs_q1_r > prpd_q_h)) prpd_q_h <= abs_q1_r;
                        fsm <= S_STAT;
                    end
                end

                // -------------------------------------------------------------
                // S_STAT: 12 档门槛计数专用拍 (独立于事件/累加拍, 见声明注释)
                //   处理本窗口已暂存的 q0(及 POSNEG 的 q1): 6 正 6 负门槛比较 +
                //   分档计数 + frame 合计。
                //   计数规则(2026-09-09 定案):
                //     每个"合格放电事件"(已通过基本门槛 cfg_thresh + 死区去重
                //     + 剔除区域判定)在其幅度 ≥ 档位门槛时, 该档 +1; 6 档相互
                //     独立, 一次事件可同时命中多档。
                //     档位门槛 = 0 表示该档停用(不统计)。
                //     帧合计 FRAME_N_POS/NEG 与档位门槛无关, 只数合格事件数。
                //   不设回差: 死区(cfg_deadtime)已完成"同一脉冲跨窗"的去重;
                //     在"窗口峰值域"再叠加回差会造成永久漏计(峰值流不是连续
                //     信号, 触发后没有低于释放阈的采样点去重新上膛)。
                // -------------------------------------------------------------
                S_STAT: begin
                    // 正峰 q0 (所有模式都有 q0 统计)
                    if (stat_v0) begin
                        if (!stat_q0_pol) begin
                            // 正脉冲: 合计每脉冲 +1 一次; 各档分别 +1
                            frame_npos_acc <= frame_npos_acc + 1'b1;
                            for (ti = 0; ti < 6; ti = ti + 1) begin
                                if (pos_thr_r[16*ti +: 16] != 16'd0 &&
                                    stat_q0_c >= pos_thr_r[16*ti +: 16]) begin
                                    pos_cnt_live[32*ti +: 32] <= pos_cnt_live[32*ti +: 32] + 1'b1;
                                end
                            end
                        end else begin
                            // 负脉冲: 合计每脉冲 +1 一次; 各档分别 +1
                            frame_nneg_acc <= frame_nneg_acc + 1'b1;
                            for (ti = 0; ti < 6; ti = ti + 1) begin
                                if (neg_thr_r[16*ti +: 16] != 16'd0 &&
                                    stat_q0_c >= neg_thr_r[16*ti +: 16]) begin
                                    neg_cnt_live[32*ti +: 32] <= neg_cnt_live[32*ti +: 32] + 1'b1;
                                end
                            end
                        end
                    end
                    // POSNEG 模式的负峰 q1 (stat_q1 仅在 S_PEAKN 暂存)
                    if (cfg_mode == `PD_MODE_POSNEG && stat_v1) begin
                        if (!stat_q1_pol) begin
                            frame_npos_acc <= frame_npos_acc + 1'b1;
                            for (ti = 0; ti < 6; ti = ti + 1) begin
                                if (pos_thr_r[16*ti +: 16] != 16'd0 &&
                                    stat_q1_c >= pos_thr_r[16*ti +: 16]) begin
                                    pos_cnt_live[32*ti +: 32] <= pos_cnt_live[32*ti +: 32] + 1'b1;
                                end
                            end
                        end else begin
                            frame_nneg_acc <= frame_nneg_acc + 1'b1;
                            for (ti = 0; ti < 6; ti = ti + 1) begin
                                if (neg_thr_r[16*ti +: 16] != 16'd0 &&
                                    stat_q1_c >= neg_thr_r[16*ti +: 16]) begin
                                    neg_cnt_live[32*ti +: 32] <= neg_cnt_live[32*ti +: 32] + 1'b1;
                                end
                            end
                        end
                    end
                    fsm <= S_PRRD;
                end

                // -------------------------------------------------------------
                S_PRRD: begin
                    // 处理上一拍锁存的 Σq·u 乘积 (ABSMAX/PP: S_ACC 直接落账;
                    // POSNEG: S_PEAKN 若未触发统计, 此拍兜底落账 S_ACC 的乘积)
                    if (qu_acc_v) begin
                        acc_sum_qu <= acc_sum_qu + qu_pipe_r;
                        qu_acc_v   <= 1'b0;
                    end
                    prpd_addr <= h_ph_r;
                    fsm       <= S_PRWR;
                end

                // -------------------------------------------------------------
                S_PRWR: begin
                    prpd_wdata <= cfg_prpd_max ? ((prpd_q_h > prpd_rdata) ? prpd_q_h
                                                                          : prpd_rdata)
                                               : prpd_q_h;
                    prpd_we <= 1'b1;
                    fsm     <= S_IDLE;
                end

                // -------------------------------------------------------------
                S_CYC: begin
                    if (ev_tready) begin
                        ev_tdata  <= evc;
                        ev_tvalid <= 1'b1;
                        ev_tlast  <= 1'b1;                 // 周期统计包 = 帧尾

                        cycle_idx      <= cycle_idx + 32'd1;
                        acc_cycles     <= acc_cycles + 32'd1;
                        st_live_cycles <= acc_cycles + 32'd1;
                        cyc_n          <= 32'd0;
                        cyc_qmax       <= 32'd0;

                        // 快照持续更新, PS 可随时读取
                        st_sn_n         <= acc_n;
                        st_sn_qmax      <= acc_qmax;
                        st_sn_sum_abs_q <= acc_sum_abs_q;
                        st_sn_sum_qu    <= acc_sum_qu;
                        st_frame_n_pos <= frame_npos_acc; st_frame_n_neg <= frame_nneg_acc;
                        // AD 包络极值: 12bit 有符号 ADC 码 -> 符号扩展 16bit
                        //   (全窗口无有效样本时输出 0, 避免把 -2048/+2047 当真实极值)
                        st_frame_ad_max <= (frame_ad_max_acc == SMIN) ? 16'd0
                                         : {{16-ADC_W{frame_ad_max_acc[ADC_W-1]}}, frame_ad_max_acc};
                        st_frame_ad_min <= (frame_ad_min_acc == SMAX) ? 16'd0
                                         : {{16-ADC_W{frame_ad_min_acc[ADC_W-1]}}, frame_ad_min_acc};
                        st_frame_id <= st_frame_id + 1'b1; st_frame_stat[0] <= 1'b1;
                        // 溢出: 上一帧 frame_valid 未读清即被本帧覆盖
                        if (st_frame_stat[0]) st_frame_stat[1] <= 1'b1;
                        // 12 档计数: 帧末先快照再清零实时累加器 -> 与帧合计同源同帧
                        st_pos_cnt <= pos_cnt_live;
                        st_neg_cnt <= neg_cnt_live;
                        pos_cnt_live <= {6*32{1'b0}};
                        neg_cnt_live <= {6*32{1'b0}};
                        frame_npos_acc <= 0; frame_nneg_acc <= 0;
                        frame_ad_max_acc <= SMIN; frame_ad_min_acc <= SMAX;
                        // 周期边界换档(与帧计数清零同一拍, 保证帧内门槛恒定)
                        if (apply_pending) begin
                            pos_thr_r     <= cfg_pos_thr;
                            neg_thr_r     <= cfg_neg_thr;
                            apply_pending <= 1'b0;
                            st_apply_done <= 1'b1;
                        end

                        if (meas_win_hit) begin
                            st_meas_done <= 1'b1;
                            if (cfg_auto_clear) begin
                                acc_n          <= 32'd0;
                                acc_qmax       <= 32'd0;
                                acc_sum_abs_q  <= 64'd0;
                                acc_sum_qu     <= 64'd0;
                                acc_cycles     <= 32'd0;
                                st_live_cycles <= 32'd0;
                            end
                        end
                        fsm <= S_IDLE;
                    end
                end

                default: fsm <= S_IDLE;
            endcase

            // -----------------------------------------------------------------
            // 帧内 AD 包络极值累积（2026-09-10 从窗口块 block1 移入本块 block2）
            //   移入原因: frame_ad_max_acc / frame_ad_min_acc 原先同时在 block1
            //             与本块被赋值 → 综合报 [Synth 8-6859] multi-driven net
            //             (2nd driver = VCC)，AD max/min 功能失效。
            //   位置: 本块内、case 之后 → 本块成为这两个 reg 的唯一驱动源。
            //   优先级: 与帧末清零(S_CYC 分支)同拍时，本处赋值在后 → 窗口更新优先，
            //           末窗数据不丢失（顺延计入下一帧快照）。
            //   语义保持: 全窗口参与（含无放电噪声窗），与放电事件解耦。
            // -----------------------------------------------------------------
            if (win_close) begin
                if ($signed(wc_max) > frame_ad_max_acc) frame_ad_max_acc <= wc_max;
                if ($signed(wc_min) < frame_ad_min_acc) frame_ad_min_acc <= wc_min;
            end
        end
    end

endmodule
