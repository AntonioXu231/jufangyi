`timescale 1ns/1ps
// Behavioral DDS regression for the 4-channel feature/event path.
module tb_pd_feature_dds;
  reg clk=0, adc_clk=0, rst_n=0, sync_in=0;
  always #3.846 clk=~clk;          // 130 MHz
  always #19.231 adc_clk=~adc_clk; // 26 MHz
  reg [47:0] adc_data=0; reg [3:0] adc_dv=0;
  reg [15:0] awaddr=0, araddr=0; reg [2:0] awprot=0, arprot=0;
  reg awvalid=0,wvalid=0,bready=1,arvalid=0,rready=1; reg [31:0] wdata=0; reg [3:0] wstrb=4'hf;
  wire awready,wready,bvalid,arready,rvalid,irq; wire [1:0] bresp,rresp; wire [31:0] rdata;
  wire [63:0] m_axis_tdata; wire m_axis_tvalid,m_axis_tlast; reg m_axis_tready=1;
  integer sample_n=0, event_n=0; reg [3:0] channel_seen=0; real theta; integer base;
  // 26 MSPS / 50 Hz = 520000 ADC samples per mains cycle.
  // Keep this tied to the MATLAB stimulus time base, not to phase-window count.
  localparam integer SYNC_PERIOD_SAMPLES = 520000;
  // One quiet cycle lets the DUT measure a 520000-sample period before the
  // first MATLAB event, avoiding its 20-MSPS power-up default (400000 samples).
  localparam integer WARMUP_SAMPLES = SYNC_PERIOD_SAMPLES;
  // Include channel DC offsets when rejecting background/weak pickup while
  // retaining the 430--700 LSB primary PD pulses.
  localparam [31:0] PD_THRESH_LSB = 32'd250;
  integer event_fd;
  localparam USE_MATLAB_SAMPLES = 1'b1;
  reg [47:0] matlab_samples [0:1039999];
  initial begin
    if (USE_MATLAB_SAMPLES) begin
      $readmemh("pd_adc_4ch_26m_40ms.mem", matlab_samples);
      $display("MATLAB_STIMULUS_LOADED: pd_adc_4ch_26m_40ms.mem (1040000 samples)");
    end else begin
      $display("INTERNAL_DDS_STIMULUS_ACTIVE");
    end
  end

  pd_feature_sys_top #(.INPUT_CDC(1),.CLK_HZ(130000000),.SAMPLE_HZ(26000000)) dut (
    .clk(clk),.rst_n(rst_n),.adc_clk(adc_clk),.adc_data(adc_data),.adc_dv(adc_dv),.sync_in(sync_in),
    .s_axi_awaddr(awaddr),.s_axi_awprot(awprot),.s_axi_awvalid(awvalid),.s_axi_awready(awready),
    .s_axi_wdata(wdata),.s_axi_wstrb(wstrb),.s_axi_wvalid(wvalid),.s_axi_wready(wready),.s_axi_bresp(bresp),.s_axi_bvalid(bvalid),.s_axi_bready(bready),
    .s_axi_araddr(araddr),.s_axi_arprot(arprot),.s_axi_arvalid(arvalid),.s_axi_arready(arready),.s_axi_rdata(rdata),.s_axi_rresp(rresp),.s_axi_rvalid(rvalid),.s_axi_rready(rready),
    .m_axis_tdata(m_axis_tdata),.m_axis_tvalid(m_axis_tvalid),.m_axis_tlast(m_axis_tlast),.m_axis_tready(m_axis_tready),.irq(irq));

  task axil_write(input [15:0] a,input [31:0] d); begin
    @(posedge clk); awaddr<=a; wdata<=d; awvalid<=1; wvalid<=1;
    while (!(awready && wready)) @(posedge clk);
    @(posedge clk); awvalid<=0; wvalid<=0;
    while (!bvalid) @(posedge clk);
  end endtask

  always @(posedge adc_clk) begin
    if (!rst_n) begin sample_n<=0; adc_data<=0; adc_dv<=0; sync_in<=0; end
    else begin
      adc_dv<=4'hf; sync_in <= ((sample_n % SYNC_PERIOD_SAMPLES)==0);
      if (USE_MATLAB_SAMPLES) begin
        if ((sample_n >= WARMUP_SAMPLES) && (sample_n < (WARMUP_SAMPLES + 1040000)))
          adc_data <= matlab_samples[sample_n - WARMUP_SAMPLES];
        else
          adc_data <= {4{12'd2048}}; // quiet preamble/postamble; no false event source
      end else begin
        theta = 6.28318530718 * sample_n / 1024.0;
        base = $rtoi(2048.0 + 600.0*$sin(theta));
        // ch0 occupies the least-significant 12 bits at the DUT interface.
        adc_data[11:0]  <= base;
        adc_data[23:12] <= base + 120;
        adc_data[35:24] <= base - 180;
        adc_data[47:36] <= base + 300;
      end
      sample_n<=sample_n+1;
    end
  end
  initial begin
    event_fd = $fopen("dds_axis_events.csv", "w");
    $fwrite(event_fd, "time_ns,type,ch_id,q,phase,polarity,tlast,raw_tdata\\n");
  end
  always @(posedge clk) if (rst_n && m_axis_tvalid && m_axis_tready) begin
    event_n<=event_n+1;
    if (m_axis_tdata[63:56] == 8'h00) begin
      channel_seen[m_axis_tdata[26:25]]<=1'b1;
      $fwrite(event_fd, "%0t,peak,%0d,%0d,%0d,%0d,%0d,%h\\n", $time,
              m_axis_tdata[26:25], m_axis_tdata[55:40], m_axis_tdata[39:28], m_axis_tdata[27], m_axis_tlast, m_axis_tdata);
    end else begin
      $fwrite(event_fd, "%0t,type_%h,-,-,-,-,%0d,%h\\n", $time,
              m_axis_tdata[63:56], m_axis_tlast, m_axis_tdata);
    end
  end
  initial begin
    repeat(12) @(posedge clk); rst_n=1;
    // Enable all channels, but emit AXI events only after the amplitude gate.
    // CTRL[3]=0 disables ev_all; THRESH is an ADC-code amplitude threshold.
    axil_write(16'h1000,32'h1);
    axil_write(16'h0000,32'h1); axil_write(16'h000C,PD_THRESH_LSB);
    axil_write(16'h0080,32'h1); axil_write(16'h008C,PD_THRESH_LSB);
    axil_write(16'h0100,32'h1); axil_write(16'h010C,PD_THRESH_LSB);
    axil_write(16'h0180,32'h1); axil_write(16'h018C,PD_THRESH_LSB);
    // 20 ms lock preamble + 40 ms MATLAB data at 130 MHz.
    repeat(7800000) @(posedge clk);
    if (event_n == 0) $fatal(1,"DDS regression: no AXI-Stream event observed");
    if (channel_seen != 4'hf) $fatal(1,"DDS regression: not all channels emitted events: %b",channel_seen);
    $display("DDS_TEST_PASS events=%0d channels=%b",event_n,channel_seen);
    $fclose(event_fd);
    $finish;
  end
  initial begin repeat(7900000) @(posedge clk); $fatal(1,"DDS regression timeout"); end
endmodule
