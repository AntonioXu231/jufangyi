// =============================================================================
// pd_axis_arb.v  --  多通道事件流轮询仲裁器 (beat 级轮转)
// -----------------------------------------------------------------------------
// 【为什么不做帧级锁定】
//   本设计的事件流是"稀疏"的: 一个工频周期内只有少数几个超阈值峰值事件,
//   相邻事件间隔可达数百微秒到数毫秒。若采用"锁定某通道直到 tlast"的帧级
//   仲裁, 仲裁器会在等待本帧后续事件期间阻塞其他通道, 导致其余通道 FIFO
//   堆积甚至溢出丢数。
//   因此改为 beat 级轮转: 每成功传输一个 beat 后轮转到下一通道。
//   事件包中已携带 ch_id[1:0] 字段, PS 侧按通道重组即可; tlast 仍用于标记
//   周期统计包(帧尾), 只是不同通道的帧会交织出现。
// =============================================================================
`timescale 1ns / 1ps

module pd_axis_arb #(
    parameter integer NUM_CH = 4,
    parameter integer DATA_W = 64,
    parameter integer CH_W   = 2
)(
    input  wire                      clk,
    input  wire                      rst_n,

    // 从端 (每通道一路)
    input  wire [NUM_CH*DATA_W-1:0]  s_tdata,
    input  wire [NUM_CH-1:0]         s_tvalid,
    input  wire [NUM_CH-1:0]         s_tlast,
    output wire [NUM_CH-1:0]         s_tready,

    // 主端 (合并后)
    output wire [DATA_W-1:0]         m_tdata,
    output wire                      m_tvalid,
    output wire                      m_tlast,
    input  wire                      m_tready
);

    wire [DATA_W-1:0] tdata_arr [0:NUM_CH-1];

    genvar g;
    generate
        for (g = 0; g < NUM_CH; g = g + 1) begin : GEN_TDATA
            assign tdata_arr[g] = s_tdata[g*DATA_W +: DATA_W];
        end
    endgenerate

    reg [CH_W-1:0] rr_ptr;

    // ---- 循环优先级选择器: 从 rr_ptr 起找第一个有数据的通道 ----
    reg [CH_W-1:0] sel;
    reg            sel_valid;
    integer i;
    reg [CH_W-1:0] cand;

    always @* begin
        sel       = rr_ptr;
        sel_valid = 1'b0;
        for (i = 0; i < NUM_CH; i = i + 1) begin
            cand = (rr_ptr + i) & (NUM_CH - 1);
            if (!sel_valid && s_tvalid[cand]) begin
                sel       = cand;
                sel_valid = 1'b1;
            end
        end
    end

    wire [CH_W-1:0] out_ch = sel;

    assign m_tdata  = tdata_arr[out_ch];
    assign m_tvalid = sel_valid;
    assign m_tlast  = s_tlast[out_ch];

    generate
        for (g = 0; g < NUM_CH; g = g + 1) begin : GEN_RDY
            assign s_tready[g] = (out_ch == g) && m_tready && m_tvalid;
        end
    endgenerate

    // 每成功传输一个 beat 即轮转一次, 保证公平且不阻塞任何通道
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            rr_ptr <= {CH_W{1'b0}};
        else if (m_tvalid && m_tready)
            rr_ptr <= out_ch + 1'b1;
    end

endmodule
