`timescale 1ns / 1ps
// Four-channel atomic sample CDC. All channels cross in one 48-bit FIFO word,
// avoiding independent FIFO latency/skew between ADC channels.
module pd_adc_pack_cdc #(
    parameter integer CH_NUM     = 4,
    parameter integer ADC_W      = 12,
    parameter integer DEPTH      = 256,
    parameter integer CLK_HZ     = 130000000,
    parameter integer SAMPLE_HZ  = 26000000
)(
    input  wire                         adc_clk,
    input  wire                         clk,
    input  wire                         rst_n,
    input  wire [CH_NUM*ADC_W-1:0]      adc_data,
    input  wire [CH_NUM-1:0]            adc_dv,
    output reg  [CH_NUM*ADC_W-1:0]      adc_data_clk,
    output reg  [CH_NUM-1:0]            adc_dv_clk,
    output wire                         fifo_overflow,
    output wire                         channel_skew
);

    localparam integer CDC_RATIO = CLK_HZ / SAMPLE_HZ;
    (* ASYNC_REG = "TRUE" *) reg [1:0] adc_rst_sync;
    always @(posedge adc_clk or negedge rst_n) begin
        if (!rst_n)
            adc_rst_sync <= 2'b00;
        else
            adc_rst_sync <= {adc_rst_sync[0], 1'b1};
    end
    wire adc_rst_n = adc_rst_sync[1];

    wire [CH_NUM*ADC_W-1:0] packed_data;
    wire                    packed_valid;
    wire                    skew_pulse;
    pd_pack48 #(.CH_NUM(CH_NUM), .ADC_W(ADC_W)) u_pack (
        .clk          (adc_clk),
        .rst_n        (adc_rst_n),
        .i_adc_data   (adc_data),
        .i_adc_dv     (adc_dv),
        .o_s48_data   (packed_data),
        .o_s48_valid  (packed_valid),
        .o_ch_data    (),
        .o_ch_valid   (),
        .o_ch_skew_err(skew_pulse)
    );

    wire fifo_full, fifo_empty;
    wire [CH_NUM*ADC_W-1:0] fifo_data;
    wire fifo_wr_en = packed_valid && !fifo_full;
    reg fifo_rd_en, fifo_rd_en_d;
    reg fifo_overflow_adc, channel_skew_adc;

    always @(posedge adc_clk) begin
        if (!adc_rst_n) begin
            fifo_overflow_adc <= 1'b0;
            channel_skew_adc  <= 1'b0;
        end else begin
            if (packed_valid && fifo_full)
                fifo_overflow_adc <= 1'b1;
            if (skew_pulse)
                channel_skew_adc <= 1'b1;
        end
    end

    async_fifo #(
        .WIDTH     (CH_NUM*ADC_W),
        .DEPTH     (DEPTH),
        .RAM_STYLE ("block")
    ) u_fifo (
        .wr_clk   (adc_clk),
        .wr_rst_n (adc_rst_n),
        .wr_en    (fifo_wr_en),
        .wr_data  (packed_data),
        .full     (fifo_full),
        .rd_clk   (clk),
        .rd_rst_n (rst_n),
        .rd_en    (fifo_rd_en),
        .rd_data  (fifo_data),
        .empty    (fifo_empty)
    );

    reg [7:0] rate_count;
    wire      sample_tick = (rate_count == 0);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rate_count    <= 8'd0;
            fifo_rd_en    <= 1'b0;
            fifo_rd_en_d  <= 1'b0;
            adc_data_clk  <= {(CH_NUM*ADC_W){1'b0}};
            adc_dv_clk    <= {CH_NUM{1'b0}};
        end else begin
            if (CDC_RATIO < 1 || CDC_RATIO > 256)
                rate_count <= 8'd0;
            else if (rate_count == CDC_RATIO-1)
                rate_count <= 8'd0;
            else
                rate_count <= rate_count + 1'b1;

            fifo_rd_en   <= sample_tick && !fifo_empty;
            fifo_rd_en_d <= fifo_rd_en;
            adc_dv_clk   <= {CH_NUM{fifo_rd_en_d}};
            if (fifo_rd_en_d)
                adc_data_clk <= fifo_data;
        end
    end

    // Error flags are sticky in the ADC domain and cross as single-bit levels.
    (* ASYNC_REG = "TRUE" *) reg [1:0] overflow_sync, skew_sync;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            overflow_sync <= 2'b00;
            skew_sync     <= 2'b00;
        end else begin
            overflow_sync <= {overflow_sync[0], fifo_overflow_adc};
            skew_sync     <= {skew_sync[0], channel_skew_adc};
        end
    end
    assign fifo_overflow = overflow_sync[1];
    assign channel_skew  = skew_sync[1];

    initial begin
        if (CH_NUM != 4)
            $display("[ERROR] pd_adc_pack_cdc requires exactly four channels");
        if (CDC_RATIO < 1 || CDC_RATIO > 256 || (CDC_RATIO*SAMPLE_HZ) != CLK_HZ)
            $display("[ERROR] pd_adc_pack_cdc clock ratio must be an integer in 1..256");
        if (DEPTH < 4 || (DEPTH & (DEPTH-1)) != 0)
            $display("[ERROR] pd_adc_pack_cdc DEPTH must be a power of two >= 4");
    end

endmodule
