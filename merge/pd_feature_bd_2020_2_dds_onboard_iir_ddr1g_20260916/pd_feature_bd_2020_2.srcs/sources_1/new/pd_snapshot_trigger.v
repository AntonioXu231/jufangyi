`timescale 1ns / 1ps
// =============================================================================
// pd_snapshot_trigger.v -- feature-event driven automatic DDR snapshot trigger
// -----------------------------------------------------------------------------
// The event pulse comes from pd_feature_top when a feature event is accepted by
// its per-channel FIFO.  It deliberately does NOT depend on AXI DMA TREADY.
// Exactly one trigger is emitted per freeze/resume transaction; software's
// FREEZE_CTRL trigger remains higher priority and is never replaced.
// =============================================================================
module pd_snapshot_trigger #(
    parameter integer CH_NUM = 4
)(
    input  wire              clk,
    input  wire              rst_n,
    input  wire [CH_NUM-1:0] i_event_accept,
    input  wire              i_enable,
    input  wire [CH_NUM-1:0] i_channel_mask,
    input  wire              i_sw_freeze_trig,
    input  wire              i_freeze_resume,
    input  wire              i_slot_full,
    input  wire              i_status_clear,

    output reg               o_auto_freeze_trig,
    output wire              o_armed,
    output reg               o_slot_full_drop,
    output reg  [31:0]       o_slot_full_drop_count
);

    wire event_match = |(i_event_accept & i_channel_mask);
    reg  waiting_resume;

    // A trigger is legal only while the ring is not already being held for a
    // previous snapshot and at least one destination slot remains available.
    assign o_armed = i_enable && !i_slot_full && !waiting_resume;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            waiting_resume          <= 1'b0;
            o_auto_freeze_trig      <= 1'b0;
            o_slot_full_drop        <= 1'b0;
            o_slot_full_drop_count  <= 32'd0;
        end else begin
            o_auto_freeze_trig <= 1'b0;
            o_slot_full_drop   <= 1'b0;

            if (i_status_clear)
                o_slot_full_drop_count <= 32'd0;

            // Resume is the ownership handoff back from PS; only then may the
            // next feature event create another automatic freeze request.
            if (i_freeze_resume)
                waiting_resume <= 1'b0;
            else if (i_sw_freeze_trig)
                waiting_resume <= 1'b1;
            else if (i_enable && event_match && !waiting_resume) begin
                if (i_slot_full) begin
                    o_slot_full_drop <= 1'b1;
                    if (!i_status_clear)
                        o_slot_full_drop_count <= o_slot_full_drop_count + 32'd1;
                end else begin
                    o_auto_freeze_trig <= 1'b1;
                    waiting_resume     <= 1'b1;
                end
            end
        end
    end
endmodule
