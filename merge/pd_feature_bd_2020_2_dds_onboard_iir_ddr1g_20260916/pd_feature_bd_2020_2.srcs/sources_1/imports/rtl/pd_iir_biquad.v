`timescale 1ns / 1ps
// Second-order IIR section with a two-system-clock initiation interval.
// At the final configuration (130 MHz system / 65 MSPS ADC), each sample
// arrives every two system clocks. Five parallel products are captured on the
// sample strobe and accumulated/committed on the following clock. This uses
// five DSP multipliers per section, but sustains the required 65 MSPS rate.
module pd_iir_biquad #(
    parameter integer DW = 16,
    parameter integer CW = 18,
    parameter integer FW = 16,
    parameter integer PH = 2
)(
    input wire clk, input wire rst_n, input wire clr, input wire dv,
    input wire signed [DW-1:0] din,
    input wire signed [CW-1:0] c_b0, c_b1, c_b2, c_a1, c_a2,
    output reg signed [DW-1:0] dout, output reg dout_dv
);
    localparam integer PW = DW + CW;
    localparam integer AW = PW + 3;
    localparam signed [DW-1:0] MAXV = {1'b0,{(DW-1){1'b1}}};
    localparam signed [DW-1:0] MINV = {1'b1,{(DW-1){1'b0}}};

    reg pending;
    reg signed [DW-1:0] x_sample;
    reg signed [DW-1:0] x1, x2, y1, y2;
    reg signed [PW-1:0] p_b0, p_b1, p_b2, p_a1, p_a2;

    wire signed [PW-1:0] p_b0_next = din * c_b0;
    wire signed [PW-1:0] p_b1_next = x1 * c_b1;
    wire signed [PW-1:0] p_b2_next = x2 * c_b2;
    wire signed [PW-1:0] p_a1_next = y1 * c_a1;
    wire signed [PW-1:0] p_a2_next = y2 * c_a2;

    wire signed [AW-1:0] b0_ext = {{(AW-PW){p_b0[PW-1]}}, p_b0};
    wire signed [AW-1:0] b1_ext = {{(AW-PW){p_b1[PW-1]}}, p_b1};
    wire signed [AW-1:0] b2_ext = {{(AW-PW){p_b2[PW-1]}}, p_b2};
    wire signed [AW-1:0] a1_ext = {{(AW-PW){p_a1[PW-1]}}, p_a1};
    wire signed [AW-1:0] a2_ext = {{(AW-PW){p_a2[PW-1]}}, p_a2};
    wire signed [AW-1:0] acc_next = b0_ext + b1_ext + b2_ext - a1_ext - a2_ext;
    wire signed [AW-1:0] rounded = acc_next + (1 <<< (FW-1));
    wire signed [AW-1:0] shifted = rounded >>> FW;
    wire signed [AW-1:0] max_ext = {{(AW-DW){MAXV[DW-1]}}, MAXV};
    wire signed [AW-1:0] min_ext = {{(AW-DW){MINV[DW-1]}}, MINV};
    wire signed [DW-1:0] y_next = (shifted > max_ext) ? MAXV :
                                   (shifted < min_ext) ? MINV : shifted[DW-1:0];

    initial begin
        if (PH != 2) $error("pd_iir_biquad requires a two-clock initiation interval");
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n || clr) begin
            pending <= 1'b0;
            x_sample <= {DW{1'b0}};
            x1 <= {DW{1'b0}}; x2 <= {DW{1'b0}};
            y1 <= {DW{1'b0}}; y2 <= {DW{1'b0}};
            p_b0 <= {PW{1'b0}}; p_b1 <= {PW{1'b0}}; p_b2 <= {PW{1'b0}};
            p_a1 <= {PW{1'b0}}; p_a2 <= {PW{1'b0}};
            dout <= {DW{1'b0}};
            dout_dv <= 1'b0;
        end else begin
            dout_dv <= 1'b0;

`ifndef SYNTHESIS
            if (dv && pending)
                $error("pd_iir_biquad input dv spacing is shorter than two system clocks");
`endif

            if (pending) begin
                x2 <= x1;
                x1 <= x_sample;
                y2 <= y1;
                y1 <= y_next;
                dout <= y_next;
                dout_dv <= 1'b1;
                pending <= 1'b0;
            end

            if (dv) begin
                p_b0 <= p_b0_next;
                p_b1 <= p_b1_next;
                p_b2 <= p_b2_next;
                p_a1 <= p_a1_next;
                p_a2 <= p_a2_next;
                x_sample <= din;
                pending <= 1'b1;
            end
        end
    end
endmodule
