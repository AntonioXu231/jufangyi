`timescale 1ns/1ps
// Small source-only regression. No block design or full DDR simulation needed.
module tb_pd_dds_matlab_template;
    reg clk = 1'b0;
    reg rst_n = 1'b0;
    wire [47:0] adc_data;
    wire [3:0] adc_dv;
    wire sync_in;
    integer sample_index = 0;

    always #19.230769 clk = ~clk; // 26 MHz

    pd_dds_adc_source dut (
        .clk(clk), .rst_n(rst_n), .adc_data(adc_data),
        .adc_dv(adc_dv), .sync_in(sync_in)
    );

    initial begin
        repeat (4) @(negedge clk);
        // Do not release reset in the same active region as the checker.
        // Otherwise the first sample can be checked before the DUT has
        // emitted its sync pulse.
        #1 rst_n = 1'b1;
    end

    always @(negedge clk) if (rst_n) begin
        if (adc_dv !== 4'hf)
            $fatal(1, "ADC data-valid missing at sample %0d", sample_index);
        if (sync_in !== (((sample_index == 0) || (sample_index == 520000)) ?
                         1'b1 : 1'b0))
            $fatal(1, "50 Hz sync mismatch at sample %0d", sample_index);
        case (sample_index)
            64998:  if (adc_data[11:0]   !== 12'd2101) $fatal(1, "CH0 template start");
            65000:  if (adc_data[11:0]   !== 12'd3178) $fatal(1, "CH0 main pulse");
            194998: if (adc_data[23:12]  !== 12'd2149) $fatal(1, "CH1 template start");
            195000: if (adc_data[23:12]  !== 12'd1169) $fatal(1, "CH1 main pulse");
            324998: if (adc_data[35:24]  !== 12'd2301) $fatal(1, "CH2 template start");
            325000: if (adc_data[35:24]  !== 12'd1711) $fatal(1, "CH2 main pulse");
            454998: if (adc_data[47:36]  !== 12'd1892) $fatal(1, "CH3 template start");
            455000: if (adc_data[47:36]  !== 12'd2755) $fatal(1, "CH3 main pulse");
        endcase
        if (sample_index == 520000) begin
            $display("DDS_MATLAB_TEMPLATE_PASS channels=4 period_samples=520000");
            $finish;
        end
        sample_index = sample_index + 1;
    end

    initial begin
        #21000000;
        $fatal(1, "DDS template regression timed out");
    end
endmodule
