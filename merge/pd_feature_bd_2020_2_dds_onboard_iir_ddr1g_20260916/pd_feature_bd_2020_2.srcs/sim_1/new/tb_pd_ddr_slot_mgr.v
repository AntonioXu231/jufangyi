`timescale 1ns / 1ps
// Focused RTL test: copy engine signals are modeled; no AXI DataMover BFM needed.
module tb_pd_ddr_slot_mgr;
    reg clk=0; always #5 clk=~clk;
    reg rst_n=0, freeze_req=0, cancel_req=0, auto_en=1;
    reg [31:0] freeze_base=32'h1000_2000, freeze_len=32'h0000_6000;
    reg ring_idle=1, copy_busy=0, copy_done=0, copy_err=0, manual_launch=0;
    reg [3:0] slot_lock=0, slot_release=0;
    reg req_overflow=0, status_clear=0;
    wire req_ack, req_drop, copy_start;
    wire [31:0] copy_src, copy_dst, copy_len;
    wire [3:0] slot_valid, slot_busy, slot_locked;
    wire slot_full, cfg_err, cmd_err, req_ovf, req_pending;
    wire [1:0] last_slot; wire [31:0] snapshot_seq, drop_count;
    wire [31:0] slot0_len,slot1_len,slot2_len,slot3_len;
    wire [31:0] slot0_seq,slot1_seq,slot2_seq,slot3_seq;

    pd_ddr_slot_mgr dut (
        .clk(clk),.rst_n(rst_n),.i_freeze_req(freeze_req),.i_cancel_req(cancel_req),
        .i_freeze_base(freeze_base),.i_freeze_len(freeze_len),.i_auto_snap_en(auto_en),
        .i_ring_write_idle(ring_idle),.i_copy_busy(copy_busy),.i_copy_done(copy_done),.i_copy_err(copy_err),
        .i_manual_copy_launch(manual_launch),.i_slot_lock(slot_lock),.i_slot_release(slot_release),
        .i_req_overflow(req_overflow),.i_status_clear(status_clear),.o_req_ack(req_ack),.o_req_drop(req_drop),
        .o_copy_start(copy_start),.o_copy_src_addr(copy_src),.o_copy_dst_addr(copy_dst),.o_copy_len(copy_len),
        .o_slot_valid(slot_valid),.o_slot_busy(slot_busy),.o_slot_locked(slot_locked),.o_slot_full(slot_full),
        .o_cfg_err(cfg_err),.o_cmd_err(cmd_err),.o_req_overflow(req_ovf),.o_req_pending(req_pending),
        .o_last_slot(last_slot),.o_snapshot_seq(snapshot_seq),.o_drop_count(drop_count),
        .o_slot0_len(slot0_len),.o_slot1_len(slot1_len),.o_slot2_len(slot2_len),.o_slot3_len(slot3_len),
        .o_slot0_seq(slot0_seq),.o_slot1_seq(slot1_seq),.o_slot2_seq(slot2_seq),.o_slot3_seq(slot3_seq));

    task do_copy;
        input [31:0] expected_dst;
        begin
            freeze_len=32'h0000_6000;
            freeze_req=1;
            @(posedge clk); #1; // reserve a FREE slot
            @(posedge clk); #1; // launch into existing copy engine
            if(!copy_start || copy_dst!==expected_dst || copy_len!==32'h0000_6000) begin
                $display("FAIL: copy launch dst=%h expected=%h len=%h",copy_dst,expected_dst,copy_len);
                $fatal;
            end
            copy_busy=1;
            @(posedge clk); #1;
            freeze_req=0;
            copy_busy=0;
            copy_done=1;
            @(posedge clk); #1;
            copy_done=0;
            @(posedge clk); #1;
        end
    endtask

    initial begin
        repeat(3) @(posedge clk); rst_n=1; @(posedge clk); #1;
        do_copy(32'h2000_1000);
        if(!slot_valid[0] || snapshot_seq!=1 || slot0_len!=32'h0000_6000) begin
            $display("FAIL: slot0 completion"); $fatal;
        end
        slot_lock=4'b0001; @(posedge clk); #1; slot_lock=0; @(posedge clk); #1;
        if(!slot_locked[0]) begin $display("FAIL: lock"); $fatal; end
        slot_release=4'b0001; @(posedge clk); #1; slot_release=0; @(posedge clk); #1;
        if(slot_valid[0]) begin $display("FAIL: release"); $fatal; end

        do_copy(32'h2000_1000);
        do_copy(32'h20C0_1000);
        do_copy(32'h2180_1000);
        do_copy(32'h2240_1000);
        if(slot_valid!==4'b1111 || snapshot_seq!=5) begin
            $display("FAIL: four-slot fill valid=%b seq=%0d",slot_valid,snapshot_seq); $fatal;
        end

        freeze_req=1; @(posedge clk); #1;
        if(!req_drop || !slot_full || slot_valid!==4'b1111) begin
            $display("FAIL: full-slot rejection"); $fatal;
        end
        freeze_req=0; @(posedge clk); #1;
        freeze_len=32'h00C0_0018; freeze_req=1; @(posedge clk); #1;
        if(!req_drop || !cfg_err) begin $display("FAIL: oversized length"); $fatal; end
        freeze_req=0; @(posedge clk); #1;
        $display("TB_PD_DDR_SLOT_MGR_PASS seq=%0d drops=%0d",snapshot_seq,drop_count);
        $finish;
    end
endmodule
