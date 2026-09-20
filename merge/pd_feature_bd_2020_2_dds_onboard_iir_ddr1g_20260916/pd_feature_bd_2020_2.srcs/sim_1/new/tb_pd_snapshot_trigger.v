`timescale 1ns / 1ps
module tb_pd_snapshot_trigger;
    reg clk = 1'b0;
    always #5 clk = ~clk;

    reg rst_n = 1'b0;
    reg [3:0] event_accept = 4'b0;
    reg enable = 1'b0;
    reg [3:0] channel_mask = 4'hF;
    reg sw_freeze_trig = 1'b0;
    reg freeze_resume = 1'b0;
    reg slot_full = 1'b0;
    reg status_clear = 1'b0;
    wire auto_freeze_trig, armed, slot_full_drop;
    wire [31:0] slot_full_drop_count;

    pd_snapshot_trigger #(.CH_NUM(4)) dut (
        .clk(clk), .rst_n(rst_n), .i_event_accept(event_accept),
        .i_enable(enable), .i_channel_mask(channel_mask),
        .i_sw_freeze_trig(sw_freeze_trig), .i_freeze_resume(freeze_resume),
        .i_slot_full(slot_full), .i_status_clear(status_clear),
        .o_auto_freeze_trig(auto_freeze_trig), .o_armed(armed),
        .o_slot_full_drop(slot_full_drop),
        .o_slot_full_drop_count(slot_full_drop_count)
    );

    task pulse_event;
        input [3:0] ch;
        begin
            event_accept = ch;
            @(posedge clk); #1;
            event_accept = 4'b0;
        end
    endtask

    initial begin
        repeat (3) @(posedge clk);
        rst_n = 1'b1;
        @(posedge clk); #1;
        enable = 1'b1;

        // Unselected channels must not cause a snapshot.
        channel_mask = 4'b0010;
        pulse_event(4'b0001);
        if (auto_freeze_trig || !armed) begin $display("FAIL: masked event"); $fatal; end

        // Selected FIFO-accepted event produces exactly one W1P trigger.
        pulse_event(4'b0010);
        if (!auto_freeze_trig || armed) begin $display("FAIL: first trigger"); $fatal; end
        @(posedge clk); #1;
        if (auto_freeze_trig) begin $display("FAIL: trigger width"); $fatal; end
        pulse_event(4'b0010);
        if (auto_freeze_trig) begin $display("FAIL: duplicate before resume"); $fatal; end

        freeze_resume = 1'b1;
        @(posedge clk); #1;
        freeze_resume = 1'b0;
        if (!armed) begin $display("FAIL: re-arm after resume"); $fatal; end

        slot_full = 1'b1;
        pulse_event(4'b0010);
        if (!slot_full_drop || auto_freeze_trig || slot_full_drop_count != 1) begin
            $display("FAIL: full-slot rejection"); $fatal;
        end
        slot_full = 1'b0;

        // Software trigger blocks automatic re-entry until the same resume.
        sw_freeze_trig = 1'b1;
        @(posedge clk); #1;
        sw_freeze_trig = 1'b0;
        pulse_event(4'b0010);
        if (auto_freeze_trig) begin $display("FAIL: software priority"); $fatal; end
        freeze_resume = 1'b1;
        @(posedge clk); #1;
        freeze_resume = 1'b0;

        status_clear = 1'b1;
        @(posedge clk); #1;
        status_clear = 1'b0;
        if (slot_full_drop_count != 0) begin $display("FAIL: counter clear"); $fatal; end
        $display("TB_PD_SNAPSHOT_TRIGGER_PASS");
        $finish;
    end
endmodule
