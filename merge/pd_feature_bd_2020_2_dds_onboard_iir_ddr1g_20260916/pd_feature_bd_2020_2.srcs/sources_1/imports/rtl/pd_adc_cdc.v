// =============================================================================
// pd_adc_cdc.v  --  单通道 ADC 采集 + 跨时钟域 FIFO (SAMPLE_HZ ADC -> CLK_HZ 系统时钟)
// -----------------------------------------------------------------------------
// 定位 (对齐《pd_feature IP 接口契约 v2》§2 末段):
//   pd_feature_top 为单时钟域设计, 期望 adc_data(12bit offset binary) +
//   adc_dv(每 CDC_RATIO 拍一个样本) 已在系统时钟域内。真实 ADC 随采样时钟
//   变化, 与本模块异步, 需在其前端插一级异步 FIFO 做 CDC。
//
//   本模块即该 CDC 前端 (取 project_2 的 adc_capture + async_fifo 思路, 但
//   保持 12bit offset binary, 不做 Q1.23 转换 —— pd_feature_core 内部会自行
//   完成 offset-binary -> 补码 -> 电荷标定):
//     adc_data/adc_dv(adc_clk 域, SAMPLE_HZ)
//        -> adc_capture(打拍) -> async_fifo(adc_clk->clk)
//        -> CLK_HZ 域按 CDC_RATIO(=CLK_HZ/SAMPLE_HZ) 生成节拍 adc_dv_100
//        -> adc_data_100 / adc_dv_100 (契约视图: 每 CDC_RATIO 拍一个样本)
//
// 速率切换 (只改 CLK_HZ / SAMPLE_HZ 两个参数):
//     130M/65M -> 2:1   (生产目标)
//     130M/26M -> 5:1   (当前默认, 测试)
//     130M/13M -> 10:1  (测试)
//   注意: SAMPLE_HZ 必须整除 CLK_HZ, 否则节拍不齐 (仿真会用 $display 报错)
// =============================================================================
`timescale 1ns / 1ps

module pd_adc_cdc #(
    parameter integer ADC_W     = 12,
    parameter integer DEPTH     = 64,           // 必须是 2 的幂 (格雷码指针)
    parameter integer CLK_HZ    = 130000000,    // 系统时钟 (Hz), PL 主时钟
    parameter integer SAMPLE_HZ = 26000000      // ADC 采样率 (Hz), 必须整除 CLK_HZ
)(
    input  wire              adc_clk,     // ADC 位时钟 (20MHz)
    input  wire              adc_rst_n,   // 异步复位 (低有效), 两域共用
    input  wire              clk,         // 系统时钟 (100MHz)
    input  wire              rst_n,       // 系统域异步复位 (低有效)
    input  wire [ADC_W-1:0]  adc_data,    // offset binary, adc_clk 域
    input  wire              adc_dv,      // 样本有效, adc_clk 域
    output reg  [ADC_W-1:0]  adc_data_100,// 100MHz 域 12bit offset binary
    output reg               adc_dv_100   // 100MHz 域样本有效 (每 5 拍一个)
);

    // 1) ADC 接口采样 (adc_clk 域)
    wire [ADC_W-1:0] cap_out;
    wire             cap_vld;
    adc_capture #(.WIDTH(ADC_W)) u_cap (
        .adc_clk     (adc_clk),
        .adc_rst_n   (adc_rst_n),
        .adc_data_in (adc_data),
        .adc_dv_in   (adc_dv),
        .adc_data_out(cap_out),
        .adc_data_vld(cap_vld)
    );

    // 2) 跨时钟异步 FIFO (adc_clk -> clk), 12bit
    //    写使能在非空且非满时; 读侧由 100MHz 域节拍控制
    wire fifo_full, fifo_empty;
    wire [ADC_W-1:0] fifo_dout;
    reg  rd_en;
    async_fifo #(.WIDTH(ADC_W), .DEPTH(DEPTH)) u_fifo (
        .wr_clk   (adc_clk),
        .wr_rst_n (adc_rst_n),
        .wr_en    (cap_vld && !fifo_full),
        .wr_data  (cap_out),
        .full     (fifo_full),
        .rd_clk   (clk),
        .rd_rst_n (rst_n),
        .rd_en    (rd_en),
        .rd_data  (fifo_dout),
        .empty    (fifo_empty)
    );

    // 3) 系统时钟域的采样节拍: 每 CDC_RATIO 拍产生一个样本有效脉冲
    //    CDC_RATIO = CLK_HZ / SAMPLE_HZ, 必须整数
    //    130M 下: 65M->2:1, 26M->5:1(当前默认), 13M->10:1
    localparam integer CDC_RATIO = CLK_HZ / SAMPLE_HZ;
    localparam [7:0]   CDC_TERM  = CDC_RATIO - 8'd1;       // 计数终值 = RATIO-1 (RATIO<=256)

    // 参数自检 (仅仿真有效, 综合忽略 $display)
    initial begin
        if (CDC_RATIO < 1 || (CDC_RATIO * SAMPLE_HZ) != CLK_HZ)
            $display("[ERROR] pd_adc_cdc: CLK_HZ=%0d 与 SAMPLE_HZ=%0d 非整数比(=%0d), CDC 节拍将不齐! " +
                     "130M 下请选: 65M(2:1) / 26M(5:1) / 13M(10:1)",
                     CLK_HZ, SAMPLE_HZ, CDC_RATIO);
        else
            $display("[INFO ] pd_adc_cdc: CDC ratio = %0d:1 (CLK_HZ=%0d, SAMPLE_HZ=%0d)",
                     CDC_RATIO, CLK_HZ, SAMPLE_HZ);
    end

    reg [7:0] ce_div;
    wire      ce_sample = (ce_div == 8'd0);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) ce_div <= 8'd0;
        else        ce_div <= (ce_div == CDC_TERM) ? 8'd0 : ce_div + 8'd1;
    end

    // 4) 节拍且 FIFO 非空时读
    //    注: async_fifo v2 改为同步读(BRAM 输出寄存器), rd_en 拉高后第 2 拍
    //        数据才有效, 故下面用 rd_en_d 再打一拍对齐。
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) rd_en <= 1'b0;
        else        rd_en <= ce_sample && !fifo_empty;
    end

    // 5) 输出寄存 (adc_dv_100 与 adc_data_100 同一拍有效; 数据保持到下次读出)
    //    读节拍间隔 = CDC_RATIO (>=2 拍), 额外 1 拍流水延迟不影响吞吐率。
    reg rd_en_d;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) rd_en_d <= 1'b0;
        else        rd_en_d <= rd_en;
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            adc_data_100 <= {ADC_W{1'b0}};
            adc_dv_100   <= 1'b0;
        end else begin
            adc_dv_100 <= rd_en_d;
            if (rd_en_d) adc_data_100 <= fifo_dout;
        end
    end

endmodule
