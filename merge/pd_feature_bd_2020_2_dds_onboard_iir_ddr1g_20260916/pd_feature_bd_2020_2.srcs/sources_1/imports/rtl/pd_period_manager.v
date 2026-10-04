`timescale 1ns / 1ps
// Shared per-channel AC/DC phase-window period manager.
// Counts only valid ADC samples, so its period units match pd_feature_core.
module pd_period_manager #(
    parameter integer SAMPLE_HZ    = 26000000,
    parameter integer DC_WINDOW_US = 20000,
    parameter integer F_MIN_HZ     = 45,
    parameter integer F_MAX_HZ     = 65,
    parameter integer LOST_CYC     = 3
)(
    input  wire        clk,
    input  wire        rst_n,
    input  wire        ac_mode,
    input  wire        dv,
    input  wire        sync_edge,
    output reg         cycle_start,
    output reg         sync_lost,
    output reg  [31:0] period,
    output wire        freq_ok,
    output wire [31:0] meas_cnt
);

    localparam [31:0] P_NOM    = SAMPLE_HZ / 50;
    localparam [31:0] P_DC     = (SAMPLE_HZ / 1000000) * DC_WINDOW_US;
    localparam [31:0] P_MIN    = SAMPLE_HZ / F_MAX_HZ;
    localparam [31:0] P_MAX    = SAMPLE_HZ / F_MIN_HZ;
    localparam [31:0] LOST_LIM = P_NOM * LOST_CYC;

    reg [31:0] sample_count;
    reg [31:0] meas_count_r;
    reg        ac_mode_d;
    reg        have_valid_sync;

    assign meas_cnt = meas_count_r;
    assign freq_ok = ac_mode && have_valid_sync && !sync_lost &&
                     (period >= P_MIN) && (period <= P_MAX);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sample_count     <= 32'd0;
            meas_count_r     <= 32'd0;
            ac_mode_d        <= 1'b1;
            have_valid_sync  <= 1'b0;
            period           <= P_NOM;
            cycle_start      <= 1'b0;
            sync_lost        <= 1'b0;
        end else begin
            cycle_start <= 1'b0;
            ac_mode_d   <= ac_mode;

            // A mode change starts a fresh timing epoch. Do not generate a
            // synthetic boundary at the instant the software changes CTRL[6].
            if (ac_mode != ac_mode_d) begin
                sample_count    <= dv ? 32'd1 : 32'd0;
                period          <= ac_mode ? P_NOM : P_DC;
                sync_lost       <= 1'b0;
                have_valid_sync <= 1'b0;
            end else if (!ac_mode) begin
                // DC has no mains phase reference: generate an exact 20 ms
                // sample-count window and report sync_lost/freq_ok as false.
                sync_lost       <= 1'b0;
                have_valid_sync <= 1'b0;
                period          <= P_DC;
                if (dv) begin
                    if (sample_count >= P_DC - 1) begin
                        sample_count <= 32'd0;
                        cycle_start  <= 1'b1;
                    end else begin
                        sample_count <= sample_count + 32'd1;
                    end
                end
            end else if (sync_edge) begin
                // Include a coincident valid sample, but do not invent one if
                // the synchronized edge falls between ADC sample strobes.
                period       <= sample_count + (dv ? 32'd1 : 32'd0);
                sample_count <= 32'd0;
                cycle_start  <= 1'b1;
                sync_lost    <= 1'b0;
                if (((sample_count + (dv ? 32'd1 : 32'd0)) >= P_MIN) &&
                    ((sample_count + (dv ? 32'd1 : 32'd0)) <= P_MAX)) begin
                    have_valid_sync <= 1'b1;
                    meas_count_r    <= meas_count_r + 32'd1;
                end else begin
                    have_valid_sync <= 1'b0;
                end
            end else if (dv) begin
                // Count actual samples, not system clocks. After declaring
                // loss at 3 nominal cycles, free-run at one nominal cycle so
                // the processing engine keeps a stable 50 Hz phase cadence.
                if (sample_count >= ((sync_lost ? P_NOM : LOST_LIM) - 1)) begin
                    sample_count    <= 32'd0;
                    period          <= P_NOM;
                    cycle_start     <= 1'b1;
                    sync_lost       <= 1'b1;
                    have_valid_sync <= 1'b0;
                end else begin
                    sample_count <= sample_count + 32'd1;
                end
            end
        end
    end

endmodule
