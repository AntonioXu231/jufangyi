`timescale 1ns/1ps
// Source-only regression for the COE-backed 65 MSPS streaming test source.
// Run in Vivado after sourcing scripts/13_configure_matlab_coe_roms.tcl.
module tb_pd_dds_matlab_template;
    localparam [20:0] LAST_SAMPLE = 21'd1299999;

    reg clk = 1'b0;
    reg rst_n = 1'b0;
    wire [47:0] adc_data;
    wire [3:0] adc_dv;
    wire sync_in;
    reg [20:0] sample_index = 21'd0;
    reg seen_first_sync = 1'b0;
    integer event_count0 = 0;
    integer event_count1 = 0;
    integer event_count2 = 0;
    integer event_count3 = 0;
    integer delta;

    always #7.692308 clk = ~clk; // 65 MHz

    pd_dds_adc_source dut (
        .clk(clk), .rst_n(rst_n), .adc_data(adc_data),
        .adc_dv(adc_dv), .sync_in(sync_in)
    );

    initial begin
        repeat (4) @(negedge clk);
        #1 rst_n = 1'b1;
    end

    always @(negedge clk) begin
        if (rst_n) begin
            if (!seen_first_sync) begin
                if (adc_dv === 4'hf) begin
                    if (sync_in !== 1'b1)
                        $fatal(1, "first valid output sample must carry 50 Hz sync");
                    seen_first_sync = 1'b1;
                    sample_index = 21'd1;
                end else if (sync_in !== 1'b0) begin
                    $fatal(1, "sync asserted before ROM pipeline became valid");
                end
            end else begin
                if (adc_dv !== 4'hf)
                    $fatal(1, "sample valid dropped at sample %0d", sample_index);
                if (sync_in !== (sample_index == 0))
                    $fatal(1, "sync/sample alignment mismatch at sample %0d", sample_index);

                if ((event_count0 < dut.schedule_ch0.event_count(0)) &&
                    (sample_index == dut.schedule_ch0.event_sample(0, event_count0) + 4)) begin
                    delta = {1'b0, adc_data[11:0]};
                    delta = delta - 2048;
                    if ((delta < 100) && (delta > -100))
                        $fatal(1, "CH0 COE pulse not visible at sample %0d", sample_index);
                    event_count0 = event_count0 + 1;
                end
                if ((event_count1 < dut.schedule_ch1.event_count(1)) &&
                    (sample_index == dut.schedule_ch1.event_sample(1, event_count1) + 4)) begin
                    delta = {1'b0, adc_data[23:12]};
                    delta = delta - 2056;
                    if ((delta < 100) && (delta > -100))
                        $fatal(1, "CH1 COE pulse not visible at sample %0d", sample_index);
                    event_count1 = event_count1 + 1;
                end
                if ((event_count2 < dut.schedule_ch2.event_count(2)) &&
                    (sample_index == dut.schedule_ch2.event_sample(2, event_count2) + 4)) begin
                    delta = {1'b0, adc_data[35:24]};
                    delta = delta - 2038;
                    if ((delta < 100) && (delta > -100))
                        $fatal(1, "CH2 COE pulse not visible at sample %0d", sample_index);
                    event_count2 = event_count2 + 1;
                end
                if ((event_count3 < dut.schedule_ch3.event_count(3)) &&
                    (sample_index == dut.schedule_ch3.event_sample(3, event_count3) + 4)) begin
                    delta = {1'b0, adc_data[47:36]};
                    delta = delta - 2062;
                    if ((delta < 100) && (delta > -100))
                        $fatal(1, "CH3 COE pulse not visible at sample %0d", sample_index);
                    event_count3 = event_count3 + 1;
                end

                if (sample_index == LAST_SAMPLE) begin
                    if (event_count0 != 8 || event_count1 != 9 ||
                        event_count2 != 7 || event_count3 != 10)
                        $fatal(1, "event count mismatch: %0d/%0d/%0d/%0d",
                            event_count0, event_count1, event_count2, event_count3);
                    $display("DDS_COE_STREAM_PASS channels=4 bits_per_channel=12 samples_per_cycle=1300000 events=8/9/7/10");
                    $finish;
                end
                sample_index = sample_index + 21'd1;
            end
        end
    end

    initial begin
        #21000000;
        $fatal(1, "COE streaming regression timed out");
    end
endmodule
