`timescale 1ns / 1ps
// Five-clock, second-order IIR section for the 130 MHz / 26 MHz DDS prototype.
// PH is deliberately asserted as 5: the arithmetic schedule has three
// multiply/accumulate phases plus one output phase.
module pd_iir_biquad #(
    parameter integer DW = 16,
    parameter integer CW = 18,
    parameter integer FW = 16,
    parameter integer PH = 5
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
    reg [2:0] phase;
    reg signed [DW-1:0] x1, x2, y1, y2;
    reg signed [PW-1:0] prod_a, prod_b;
    reg signed [AW-1:0] acc;
    reg signed [DW-1:0] mul_a, mul_b;
    reg signed [CW-1:0] mul_ca, mul_cb;

    initial begin
        if (PH != 5) $error("pd_iir_biquad requires PH=5");
    end

    always @* begin
        mul_a = {DW{1'b0}}; mul_b = {DW{1'b0}};
        mul_ca = {CW{1'b0}}; mul_cb = {CW{1'b0}};
        case (phase)
            3'd0: begin mul_a=din; mul_ca=c_b0; mul_b=y1; mul_cb=c_a1; end
            3'd1: begin mul_a=x1;  mul_ca=c_b1; mul_b=y2; mul_cb=c_a2; end
            3'd2: begin mul_a=x2;  mul_ca=c_b2; end
            default: begin end
        endcase
    end
    wire signed [PW-1:0] mul_p_a = mul_a * mul_ca;
    wire signed [PW-1:0] mul_p_b = mul_b * mul_cb;
    wire signed [AW-1:0] pa_ext = {{(AW-PW){prod_a[PW-1]}},prod_a};
    wire signed [AW-1:0] pb_ext = {{(AW-PW){prod_b[PW-1]}},prod_b};
    wire signed [AW-1:0] sum1 = pa_ext - pb_ext;
    wire signed [AW-1:0] sum2 = acc + pa_ext - pb_ext;
    wire signed [AW-1:0] sum3 = acc + pa_ext;
    wire signed [AW-1:0] rounded = acc + (1 <<< (FW-1));
    wire signed [AW-1:0] shifted = rounded >>> FW;
    wire signed [AW-1:0] max_ext = {{(AW-DW){MAXV[DW-1]}},MAXV};
    wire signed [AW-1:0] min_ext = {{(AW-DW){MINV[DW-1]}},MINV};
    wire signed [DW-1:0] y_next = (shifted > max_ext) ? MAXV :
                                   (shifted < min_ext) ? MINV : shifted[DW-1:0];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n || clr) phase <= 3'd0;
        else if (dv) phase <= 3'd1;
        else if (phase == 3'd4) phase <= 3'd0;
        else if (phase != 3'd0) phase <= phase + 3'd1;
    end
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n || clr) begin
            prod_a<={PW{1'b0}}; prod_b<={PW{1'b0}}; acc<={AW{1'b0}};
            x1<={DW{1'b0}}; x2<={DW{1'b0}}; y1<={DW{1'b0}}; y2<={DW{1'b0}};
            dout<={DW{1'b0}}; dout_dv<=1'b0;
        end else begin
            prod_a <= mul_p_a; prod_b <= mul_p_b;
            if (phase == 3'd1) acc <= sum1;
            else if (phase == 3'd2) acc <= sum2;
            else if (phase == 3'd3) acc <= sum3;
            if (phase == 3'd4) begin
                x2 <= x1; x1 <= din; y2 <= y1; y1 <= y_next; dout <= y_next;
            end
            dout_dv <= (phase == 3'd4);
        end
    end
endmodule
