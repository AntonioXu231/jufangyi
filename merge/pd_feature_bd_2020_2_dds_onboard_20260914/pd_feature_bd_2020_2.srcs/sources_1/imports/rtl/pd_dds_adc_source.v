`timescale 1ns / 1ps
// =============================================================================
// pd_dds_adc_source.v -- synthesizable on-chip four-channel ADC test source
// -----------------------------------------------------------------------------
// Board-test mode only. The source runs from the internally generated 26 MHz
// ADC clock and emits one valid sample on every clock edge.
// It emits four low-amplitude background waveforms plus one different PD pulse
// per channel in every 20 ms (50 Hz) mains cycle.
//
// This verifies: DDS source -> 26 MHz to 130 MHz CDC -> pd_ddr/pd_feature
// -> AXIS. It deliberately does not verify physical ADC pins or I/O timing.
// =============================================================================
module pd_dds_adc_source #(
    parameter integer CLK_HZ          = 26000000,
    parameter integer SAMPLE_HZ       = 26000000,
    parameter integer SYNC_HZ         = 50,
    parameter integer ADC_W           = 12,
    parameter integer NUM_CH          = 4
)(
    input  wire                     clk,
    input  wire                     rst_n,
    output reg  [NUM_CH*ADC_W-1:0]  adc_data,
    output reg  [NUM_CH-1:0]        adc_dv,
    output reg                      sync_in
);

    localparam integer CLK_PER_SAMPLE = CLK_HZ / SAMPLE_HZ;
    localparam integer SYNC_SAMPLES   = SAMPLE_HZ / SYNC_HZ;

    // In board-test mode CLK_HZ and SAMPLE_HZ are both 26 MHz.
    reg [2:0]  tick_div;
    reg [18:0] cycle_sample;
    reg [7:0]  phase0, phase1, phase2, phase3;

    function integer tri_wave;
        input [7:0] phase;
        integer magnitude;
        begin
            magnitude = phase[7] ? (127 - phase[6:0]) : phase[6:0];
            tri_wave = magnitude - 64;
        end
    endfunction

    function integer pd_pulse;
        input [18:0] sample_pos;
        input [18:0] centre;
        begin
            if (sample_pos == centre)
                pd_pulse = 720;
            else if (sample_pos == centre + 19'd1)
                pd_pulse = 600;
            else if (sample_pos == centre + 19'd2)
                pd_pulse = 460;
            else if (sample_pos == centre + 19'd3)
                pd_pulse = 300;
            else
                pd_pulse = 0;
        end
    endfunction

    function [11:0] saturate_u12;
        input integer sample;
        begin
            if (sample < 0)
                saturate_u12 = 12'd0;
            else if (sample > 4095)
                saturate_u12 = 12'hFFF;
            else
                saturate_u12 = sample[11:0];
        end
    endfunction

    wire sample_ce = (tick_div == CLK_PER_SAMPLE - 1);
    wire [11:0] ch0_next = saturate_u12(2048 + (tri_wave(phase0) >>> 2) +
                                         pd_pulse(cycle_sample, 19'd65000));
    wire [11:0] ch1_next = saturate_u12(2056 + (tri_wave(phase1) >>> 2) +
                                         pd_pulse(cycle_sample, 19'd195000));
    wire [11:0] ch2_next = saturate_u12(2038 + (tri_wave(phase2) >>> 1) +
                                         pd_pulse(cycle_sample, 19'd325000));
    wire [11:0] ch3_next = saturate_u12(2062 + (tri_wave(phase3) >>> 2) +
                                         pd_pulse(cycle_sample, 19'd455000));

    initial begin
        if ((CLK_PER_SAMPLE != 1) || (CLK_PER_SAMPLE * SAMPLE_HZ != CLK_HZ))
            $error("pd_dds_adc_source requires a 26 MHz DDS clock and 26 MSPS sample rate");
        if ((SYNC_SAMPLES != 520000) || (SYNC_SAMPLES * SYNC_HZ != SAMPLE_HZ))
            $error("pd_dds_adc_source requires a 50 Hz / 26 MSPS integer ratio");
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tick_div    <= 3'd0;
            cycle_sample <= 19'd0;
            phase0      <= 8'd0;
            phase1      <= 8'd53;
            phase2      <= 8'd107;
            phase3      <= 8'd179;
            adc_data    <= {NUM_CH{12'd2048}};
            adc_dv      <= {NUM_CH{1'b0}};
            sync_in     <= 1'b0;
        end else begin
            adc_dv  <= {NUM_CH{1'b0}};
            sync_in <= 1'b0;
            if (sample_ce) begin
                tick_div <= 3'd0;
                adc_data <= {ch3_next, ch2_next, ch1_next, ch0_next};
                adc_dv   <= {NUM_CH{1'b1}};
                sync_in  <= (cycle_sample == 19'd0);

                phase0 <= phase0 + 8'd3;
                phase1 <= phase1 + 8'd5;
                phase2 <= phase2 + 8'd7;
                phase3 <= phase3 + 8'd11;
                if (cycle_sample == SYNC_SAMPLES - 1)
                    cycle_sample <= 19'd0;
                else
                    cycle_sample <= cycle_sample + 19'd1;
            end else begin
                tick_div <= tick_div + 3'd1;
            end
        end
    end
endmodule
