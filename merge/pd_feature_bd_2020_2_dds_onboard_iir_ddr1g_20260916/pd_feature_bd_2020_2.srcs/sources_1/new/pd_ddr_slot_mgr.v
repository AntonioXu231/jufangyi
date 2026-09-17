`timescale 1ns / 1ps
// Four-slot automatic DDR snapshot manager.  It supplies descriptors to the
// existing single pd_ddr_snap_copy / dm_cp instance; legacy manual copy stays
// outside this module.
`include "pd_ddr_defines.vh"

module pd_ddr_slot_mgr #(
    parameter integer AXI_ADDR_W = 32
)(
    input  wire clk, input wire rst_n,
    input  wire i_freeze_req, input wire i_cancel_req,
    input  wire [AXI_ADDR_W-1:0] i_freeze_base,
    input  wire [31:0] i_freeze_len, input wire i_auto_snap_en,
    input  wire i_ring_write_idle, input wire i_copy_busy,
    input  wire i_copy_done, input wire i_copy_err,
    input  wire i_manual_copy_launch,
    input  wire [`DDR_SLOT_NUM-1:0] i_slot_lock,
    input  wire [`DDR_SLOT_NUM-1:0] i_slot_release,
    input  wire i_req_overflow, input wire i_status_clear,
    output reg o_req_ack, output reg o_req_drop,
    output reg o_copy_start,
    output reg [AXI_ADDR_W-1:0] o_copy_src_addr,
    output reg [AXI_ADDR_W-1:0] o_copy_dst_addr,
    output reg [31:0] o_copy_len,
    output reg [`DDR_SLOT_NUM-1:0] o_slot_valid,
    output reg [`DDR_SLOT_NUM-1:0] o_slot_busy,
    output reg [`DDR_SLOT_NUM-1:0] o_slot_locked,
    output reg o_slot_full, output reg o_cfg_err, output reg o_cmd_err,
    output reg o_req_overflow, output wire o_req_pending,
    // Sticky completion notification.  It is intentionally cleared only by
    // the software W1P status-clear command, so a PS level interrupt cannot
    // miss a one-cycle copy_done pulse.
    output reg o_snapshot_ready,
    output reg [`DDR_SLOT_IDX_W-1:0] o_last_slot,
    output reg [31:0] o_snapshot_seq, output reg [31:0] o_drop_count,
    output wire [31:0] o_slot0_len, output wire [31:0] o_slot1_len,
    output wire [31:0] o_slot2_len, output wire [31:0] o_slot3_len,
    output wire [31:0] o_slot0_seq, output wire [31:0] o_slot1_seq,
    output wire [31:0] o_slot2_seq, output wire [31:0] o_slot3_seq
);
    localparam [2:0] SLOT_FREE=3'd0, SLOT_RESERVED=3'd1,
                     SLOT_COPYING=3'd2, SLOT_VALID=3'd3, SLOT_LOCKED=3'd4;

    reg [2:0] slot_state [0:`DDR_SLOT_NUM-1];
    reg [31:0] slot_len [0:`DDR_SLOT_NUM-1];
    reg [31:0] slot_seq [0:`DDR_SLOT_NUM-1];
    reg req_reserved, active_valid;
    reg [`DDR_SLOT_IDX_W-1:0] req_slot, active_slot;
    reg [31:0] active_len;
    integer k;
    reg free_found;
    reg [`DDR_SLOT_IDX_W-1:0] free_slot;
    reg cfg_invalid_now;
    wire freeze_base_align24;
    wire freeze_len_align24;

    // Keep the same 24-byte safety rule as pd_ddr_snap_copy, but use the
    // dedicated balanced checker.  Writing "% 24" here makes Vivado infer a
    // deep 32-bit remainder network directly on the freeze descriptor path.
    pd_align24_check u_freeze_base_align24 (
        .value(i_freeze_base), .aligned(freeze_base_align24)
    );
    pd_align24_check u_freeze_len_align24 (
        .value(i_freeze_len), .aligned(freeze_len_align24)
    );

    function [AXI_ADDR_W-1:0] slot_base;
        input [`DDR_SLOT_IDX_W-1:0] index;
        begin
            case (index)
                2'd0: slot_base = `DDR_SLOT0_BASE;
                2'd1: slot_base = `DDR_SLOT1_BASE;
                2'd2: slot_base = `DDR_SLOT2_BASE;
                default: slot_base = `DDR_SLOT3_BASE;
            endcase
        end
    endfunction

    always @(*) begin
        free_found = 1'b0;
        free_slot = {`DDR_SLOT_IDX_W{1'b0}};
        for (k=0; k<`DDR_SLOT_NUM; k=k+1) begin
            if (!free_found && slot_state[k] == SLOT_FREE) begin
                free_found = 1'b1;
                free_slot = k[`DDR_SLOT_IDX_W-1:0];
            end
        end
        cfg_invalid_now = (i_freeze_len == 32'd0) ||
                          (i_freeze_len > `DDR_SLOT_SIZE) ||
                          !freeze_base_align24 || !freeze_len_align24;
        o_slot_valid = {`DDR_SLOT_NUM{1'b0}};
        o_slot_busy = {`DDR_SLOT_NUM{1'b0}};
        o_slot_locked = {`DDR_SLOT_NUM{1'b0}};
        for (k=0; k<`DDR_SLOT_NUM; k=k+1) begin
            o_slot_valid[k] = (slot_state[k] == SLOT_VALID) ||
                              (slot_state[k] == SLOT_LOCKED);
            o_slot_busy[k] = (slot_state[k] == SLOT_RESERVED) ||
                             (slot_state[k] == SLOT_COPYING);
            o_slot_locked[k] = (slot_state[k] == SLOT_LOCKED);
        end
    end

    assign o_req_pending = req_reserved;
    assign o_slot0_len = slot_len[0]; assign o_slot1_len = slot_len[1];
    assign o_slot2_len = slot_len[2]; assign o_slot3_len = slot_len[3];
    assign o_slot0_seq = slot_seq[0]; assign o_slot1_seq = slot_seq[1];
    assign o_slot2_seq = slot_seq[2]; assign o_slot3_seq = slot_seq[3];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            req_reserved <= 1'b0; req_slot <= 0;
            active_valid <= 1'b0; active_slot <= 0; active_len <= 0;
            o_req_ack <= 1'b0; o_req_drop <= 1'b0; o_copy_start <= 1'b0;
            o_copy_src_addr <= 0; o_copy_dst_addr <= 0; o_copy_len <= 0;
            o_slot_full <= 1'b0; o_cfg_err <= 1'b0; o_cmd_err <= 1'b0;
            o_req_overflow <= 1'b0; o_last_slot <= 0;
            o_snapshot_ready <= 1'b0;
            o_snapshot_seq <= 0; o_drop_count <= 0;
            for (k=0; k<`DDR_SLOT_NUM; k=k+1) begin
                slot_state[k] <= SLOT_FREE; slot_len[k] <= 0; slot_seq[k] <= 0;
            end
        end else begin
            o_req_ack <= 1'b0; o_req_drop <= 1'b0; o_copy_start <= 1'b0;
            if (i_status_clear) begin
                o_slot_full <= 1'b0; o_cfg_err <= 1'b0;
                o_cmd_err <= 1'b0; o_req_overflow <= 1'b0;
                o_snapshot_ready <= 1'b0;
            end
            if (i_req_overflow) begin
                o_req_overflow <= 1'b1;
                o_drop_count <= o_drop_count + 1'b1;
            end

            // lock -> read -> release: only VALID may lock and only LOCKED may release.
            for (k=0; k<`DDR_SLOT_NUM; k=k+1) begin
                if (i_slot_lock[k]) begin
                    if (slot_state[k] == SLOT_VALID) slot_state[k] <= SLOT_LOCKED;
                    else o_cmd_err <= 1'b1;
                end
                if (i_slot_release[k]) begin
                    if (slot_state[k] == SLOT_LOCKED) slot_state[k] <= SLOT_FREE;
                    else o_cmd_err <= 1'b1;
                end
            end

            if (i_cancel_req || !i_auto_snap_en) begin
                if (req_reserved) slot_state[req_slot] <= SLOT_FREE;
                req_reserved <= 1'b0;
            end

            if (active_valid && i_copy_done) begin
                slot_state[active_slot] <= SLOT_VALID;
                slot_len[active_slot] <= active_len;
                slot_seq[active_slot] <= o_snapshot_seq + 1'b1;
                o_snapshot_seq <= o_snapshot_seq + 1'b1;
                o_last_slot <= active_slot;
                o_snapshot_ready <= 1'b1;
                active_valid <= 1'b0;
            end else if (active_valid && i_copy_err) begin
                slot_state[active_slot] <= SLOT_FREE;
                active_valid <= 1'b0;
            end

            // First bind the frozen data to a free slot.  Reservation survives
            // any current DataMover activity until the launch preconditions hold.
            if (i_auto_snap_en && i_freeze_req && !req_reserved && !active_valid) begin
                if (cfg_invalid_now) begin
                    o_cfg_err <= 1'b1; o_req_ack <= 1'b1; o_req_drop <= 1'b1;
                    o_drop_count <= o_drop_count + 1'b1;
                end else if (!free_found) begin
                    o_slot_full <= 1'b1; o_req_ack <= 1'b1; o_req_drop <= 1'b1;
                    o_drop_count <= o_drop_count + 1'b1;
                end else begin
                    slot_state[free_slot] <= SLOT_RESERVED;
                    req_slot <= free_slot;
                    req_reserved <= 1'b1;
                end
            end

            // Manual launch has priority; automatic launch waits one more cycle.
            if (i_auto_snap_en && i_freeze_req && req_reserved &&
                i_ring_write_idle && !i_copy_busy && !i_copy_err &&
                !i_manual_copy_launch && !active_valid) begin
                slot_state[req_slot] <= SLOT_COPYING;
                active_slot <= req_slot; active_len <= i_freeze_len;
                active_valid <= 1'b1;
                o_copy_src_addr <= i_freeze_base;
                o_copy_dst_addr <= slot_base(req_slot);
                o_copy_len <= i_freeze_len;
                o_copy_start <= 1'b1; o_req_ack <= 1'b1;
                req_reserved <= 1'b0;
            end
        end
    end
endmodule
