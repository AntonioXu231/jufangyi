`timescale 1ns / 1ps
// =============================================================================
// pd_pack48.v  --  4 通道样本字打包（IF-1 -> IF-3 §3.1）
// -----------------------------------------------------------------------------
// 契约 §3.1：一个采样时刻的 4 通道拼成一个 48bit 字
//     bit [47:36] ch3   [35:24] ch2   [23:12] ch1   [11:0] ch0
//   offset binary（0x800 = 0V），本模块只做拼接，不做码制转换（无损）。
//
// 本模块同时完成 契约 §2.2 的 "双路扇出"：
//     Path A -> DDR 环形缓存（48bit 样本字流）
//     Path B -> 实时算法链（带通/选频/FFT/门槛统计），按通道拆开输出
//
// 通道同步检查（开发目标 风险 2）：4 路 AD9226 共用同一采样时钟，正常情况下
//   4 个 adc_dv 必然同拍。若出现错位（PCB 等长不良 / 采样时钟偏移），
//   本模块拉高 o_ch_skew_err 一拍，供 QUALITY 寄存器上报，不阻断数据。
//
// 数据通路是本模块的"直通"部分：adc_data 本身已是 {ch3,ch2,ch1,ch0} 顺序，
//   因此 48bit 字在数值上等于输入总线；这里显式写出是为了把"字段顺序"这一
//   契约条款固定下来，避免上游改序后静默出错。
// =============================================================================
module pd_pack48 #(
    parameter integer CH_NUM = 4,
    parameter integer ADC_W  = 12
)(
    input  wire                      clk,
    input  wire                      rst_n,

    // ---- IF-1：ADC 采样域（adc_clk）输入 ----
    input  wire [CH_NUM*ADC_W-1:0]   i_adc_data,   // {ch3,ch2,ch1,ch0}
    input  wire [CH_NUM-1:0]         i_adc_dv,     // 每通道样本有效（正常应全部同拍）

    // ---- Path A：48bit 样本字（送异步 FIFO -> DDR 环形缓存）----
    output reg  [CH_NUM*ADC_W-1:0]   o_s48_data,
    output reg                       o_s48_valid,

    // ---- Path B：按通道拆开（送实时算法链）----
    output reg  [CH_NUM*ADC_W-1:0]   o_ch_data,    // 与 Path A 同序，供算法链按切片取用
    output reg                       o_ch_valid,

    // ---- 同步/异常 ----
    output reg                       o_ch_skew_err // 4 通道 dv 不同拍
);

    localparam SAMPLE_W = CH_NUM * ADC_W;          // 48

    // ---- 1) 通道有效判据：要求 4 路 dv 全部有效（与）----
    wire       dv_all   = (&i_adc_dv);             // 全 1 -> 该拍为有效采样时刻
    wire       dv_any   = (|i_adc_dv);             // 至少 1 路有效
    wire       skew_err = dv_any & ~dv_all;        // 部分有效 = 通道错位

    // ---- 2) 48bit 样本字（严格按 §3.1 字段顺序，逐通道显式拼接）----
    //     [47:36]=ch3  [35:24]=ch2  [23:12]=ch1  [11:0]=ch0
    wire [SAMPLE_W-1:0] s48_word = {
        i_adc_data[ (3*ADC_W) +: ADC_W ],          // ch3 -> [47:36]
        i_adc_data[ (2*ADC_W) +: ADC_W ],          // ch2 -> [35:24]
        i_adc_data[ (1*ADC_W) +: ADC_W ],          // ch1 -> [23:12]
        i_adc_data[ (0*ADC_W) +: ADC_W ]           // ch0 -> [11:0]
    };

    // ---- 3) 输出寄存一拍，改善时序（130MHz 下 48bit 直连也满足，但寄存更稳）----
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            o_s48_data    <= {SAMPLE_W{1'b0}};
            o_s48_valid   <= 1'b0;
            o_ch_data     <= {SAMPLE_W{1'b0}};
            o_ch_valid    <= 1'b0;
            o_ch_skew_err <= 1'b0;
        end else begin
            o_s48_valid   <= dv_all;
            o_ch_valid    <= dv_all;
            o_ch_skew_err <= skew_err;
            if (dv_all) begin
                o_s48_data <= s48_word;            // Path A
                o_ch_data  <= s48_word;            // Path B（同一份数据，无损复制）
            end
        end
    end

endmodule
