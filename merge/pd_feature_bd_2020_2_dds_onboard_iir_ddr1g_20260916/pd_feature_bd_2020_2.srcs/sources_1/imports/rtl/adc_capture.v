`timescale 1ns / 1ps
// =========================================================================
// adc_capture.v  --  ADC 并行接口（12bit，偏移二进制）
// 输入 adc_data_in 在 adc_clk 上升沿被采样；仅当 adc_dv_in 有效时才认为
// 本拍数据有效（adc_dv_in 与 adc_data_in 同一时钟域，跟随 ADC 位时钟）。
//
// 四通道化说明：本模块保持单通道，由 pd_acq_top 用 generate 例化 4 份。
// =========================================================================
module adc_capture #(
    parameter WIDTH = 12
)(
    input  wire              adc_clk,
    input  wire              adc_rst_n,
    input  wire [WIDTH-1:0]  adc_data_in,
    input  wire              adc_dv_in,     // 本通道样本有效（adc_clk 域）
    output reg  [WIDTH-1:0]  adc_data_out,
    output reg               adc_data_vld
);

    // 同步复位（驱动 FIFO 写使能等 RAMB 控制引脚，避免 REQP-1840 异步复位
    // 释放瞬间毛刺损坏 BRAM 内容）。复位期间行为不变：下个时钟沿清零。
    always @(posedge adc_clk) begin
        if (!adc_rst_n) begin
            adc_data_out <= 0;
            adc_data_vld <= 1'b0;
        end else begin
            adc_data_out <= adc_data_in;
            adc_data_vld <= adc_dv_in;
        end
    end

endmodule
