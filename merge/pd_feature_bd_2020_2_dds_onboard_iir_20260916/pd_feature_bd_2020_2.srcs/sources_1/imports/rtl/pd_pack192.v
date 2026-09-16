`timescale 1ns / 1ps
// =============================================================================
// pd_pack192.v  --  48bit 字 -> 192bit 块 -> 3x64bit AXI beat（契约 §3.2 / §3.3）
// -----------------------------------------------------------------------------
// 契约 §3.2：连续 4 个采样时刻的 48bit 字（W0~W3）拼成 192bit 整数 V
//     V = W0 + W1<<48 + W2<<96 + W3<<144        （小端）
//     beat0 = V[63:0]   beat1 = V[127:64]   beat2 = V[191:128]
//
// 实现要点（为什么不用移位寄存器做齿轮箱）：
//   把 blk 固定布局为 {W3, W2, W1, W0}，则该 192 位数在数值上恰好等于 V，
//   于是 beat_i 就是 blk 的固定切片 blk[64i +: 64]，只需一个 3:1 的 64bit mux，
//   完全不需要桶形移位器（省约 300 LUT）。写入侧同理：第 j 个字写 blk[48j +:48]。
//
// 时序（tready 常高、数据常有时）：每 5 个时钟完成 1 个块（4 字入 / 3 beat 出）
//     c5:in W0 | c6:in W1 | c7:out b0 + in W2 | c8:out b1 + in W3 | c9:out b2
//   65MSPS 时 130MHz 域需 2 clk/样本 = 8 clk/块，5 < 8，余量充足；
//   26MSPS(测试) 时 20 clk/块，更宽松。
//
// 背压：m_axis_tready 拉低时停止发 beat，wcnt 饱和在 4 并使 s48_ready=0，
//       反压逐级传到上游异步 FIFO，绝不丢样本（契约"原始无损"要求）。
// =============================================================================
module pd_pack192 #(
    parameter integer SAMPLE_W     = 48,    // 输入字宽
    parameter integer BLK_W        = 192,   // 块宽
    parameter integer AXI_DW       = 64     // 输出 AXI-Stream 位宽
)(
    input  wire                  clk,
    input  wire                  rst_n,

    // ---- 上游：48bit 样本字（来自异步 FIFO 读侧，clk 域）----
    input  wire [SAMPLE_W-1:0]   i_s48_data,
    input  wire                  i_s48_valid,
    output wire                  o_s48_ready,

    // ---- 下游：64bit AXI-Stream（送 AXI-Stream Data FIFO -> DataMover）----
    output reg  [AXI_DW-1:0]     m_axis_tdata,
    output wire                  m_axis_tvalid,
    input  wire                  m_axis_tready,
    output wire [AXI_DW/8-1:0]   m_axis_tkeep,
    output wire                  m_axis_tlast,

    // ---- 辅助 ----
    output reg                   o_beat_en,   // 1 个 64bit beat 被下游接收（供字节信用计数）
    output reg                   o_blk_done,  // 1 个完整 192bit 块发出
    output reg  [31:0]           o_blk_cnt    // 已发出块数（块地址源）
);

    localparam integer WORD_PER_BLK = BLK_W / SAMPLE_W;   // 4
    localparam integer BEAT_PER_BLK = BLK_W / AXI_DW;     // 3

    // 注意：wcnt 需要表示 0..4，必须用 3bit；写成 WORD_PER_BLK[1:0] 会把 4 截断成 0
    reg [BLK_W-1:0] blk;                   // {W3, W2, W1, W0}
    reg [2:0]       wcnt;                  // 已接收字数 0..4
    reg [2:0]       bcnt;                  // 已发出 beat 数 0..2

    wire in_acc  = i_s48_valid & o_s48_ready;
    wire out_acc = m_axis_tvalid & m_axis_tready;

    // ---- 握手信号 ----
    // 收满 4 个字后暂停接收，直到 3 个 beat 全部发出
    assign o_s48_ready  = (wcnt != WORD_PER_BLK[2:0]);
    // 有效 bit 数 = wcnt*48，需覆盖第 (bcnt+1) 个 beat 的 64bit
    //   bcnt=0 需 wcnt>=2 ; bcnt=1 需 wcnt>=3 ; bcnt=2 需 wcnt>=4
    assign m_axis_tvalid = (bcnt == 3'd0) ? (wcnt >= 3'd2)
                         : (bcnt == 3'd1) ? (wcnt >= 3'd3)
                         : (bcnt == 3'd2) ? (wcnt >= WORD_PER_BLK[2:0])
                         : 1'b0;
    assign m_axis_tkeep  = {AXI_DW/8{1'b1}};   // 恒全有效（192bit 100% 利用）
    assign m_axis_tlast  = 1'b0;               // 由 DataMover 的 BTT 决定长度，不依赖 tlast

    // ---- 输出数据：固定切片选择，无桶形移位 ----
    always @(*) begin
        case (bcnt)
            3'd0: m_axis_tdata = blk[ 63:  0];
            3'd1: m_axis_tdata = blk[127: 64];
            3'd2: m_axis_tdata = blk[191:128];
            default: m_axis_tdata = blk[63:0];
        endcase
    end

    // ---- 主时序 ----
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            blk        <= {BLK_W{1'b0}};
            wcnt       <= 3'd0;
            bcnt       <= 3'd0;
            o_beat_en  <= 1'b0;
            o_blk_done <= 1'b0;
            o_blk_cnt  <= 32'd0;
        end else begin
            o_beat_en  <= out_acc;
            o_blk_done <= 1'b0;

            // 写入：第 wcnt 个字放在 blk[48*wcnt +: 48]
            if (in_acc) begin
                blk[wcnt*SAMPLE_W +: SAMPLE_W] <= i_s48_data;
                wcnt <= wcnt + 3'd1;
            end

            // 读出：3 个 beat 发完即整块复位，形成 5 clk/块 的稳定节拍
            if (out_acc) begin
                if (bcnt == BEAT_PER_BLK[2:0] - 3'd1) begin
                    bcnt       <= 3'd0;
                    wcnt       <= 3'd0;
                    o_blk_done <= 1'b1;
                    o_blk_cnt  <= o_blk_cnt + 32'd1;
                end else begin
                    bcnt <= bcnt + 3'd1;
                end
            end
        end
    end

endmodule
