`timescale 1ns / 1ps
// =============================================================================
// pd_sync_pulse.v -- 外部工频同步输入的 CDC 与周期起始脉冲
// =============================================================================
module pd_sync_pulse (
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME CLK, FREQ_HZ 130000000, ASSOCIATED_RESET rst_n" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
    input  wire clk,
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME RST, POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst_n RST" *)
    input  wire rst_n,
    input  wire sync_in,
    output wire sync_level,
    output wire cycle_start
);

    (* ASYNC_REG = "TRUE" *) reg [1:0] sync_ff;
    reg sync_d;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sync_ff <= 2'b00;
            sync_d  <= 1'b0;
        end else begin
            sync_ff <= {sync_ff[0], sync_in};
            sync_d  <= sync_ff[1];
        end
    end

    assign sync_level  = sync_ff[1];
    assign cycle_start = sync_ff[1] & ~sync_d;

endmodule
