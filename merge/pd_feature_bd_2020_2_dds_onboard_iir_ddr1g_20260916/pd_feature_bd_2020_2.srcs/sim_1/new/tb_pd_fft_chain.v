`timescale 1ns / 1ps
// -----------------------------------------------------------------------------
// Self-checking behavioral regression for the four-channel FFT branch.
// It instantiates the actual four Xilinx FFT IP cores, not a behavioural
// substitute.  Every input tone is exactly on an FFT bin, so spectral leakage
// is avoided and the selected-bin test is deterministic.
// -----------------------------------------------------------------------------
module tb_pd_fft_chain;
    localparam integer FRAME_N = 1024;
    localparam integer FRAMES_TO_SEND = 3;
    localparam integer AMP = 512;
    localparam integer BIN0 = 1, BIN1 = 3, BIN2 = 5, BIN3 = 7;

    reg clk = 1'b0, rst_n = 1'b0;
    always #3.846 clk = ~clk;       // 130 MHz

    reg [47:0] filt_data = 48'd0;
    reg [3:0]  filt_dv = 4'd0;
    wire [3:0] input_overflow;

    // Adapter -> FFT configuration and input streams
    wire [15:0] cfg0_d, cfg1_d, cfg2_d, cfg3_d;
    wire cfg0_v, cfg1_v, cfg2_v, cfg3_v, cfg0_r, cfg1_r, cfg2_r, cfg3_r;
    wire [31:0] in0_d, in1_d, in2_d, in3_d;
    wire in0_v, in1_v, in2_v, in3_v, in0_r, in1_r, in2_r, in3_r;
    wire in0_l, in1_l, in2_l, in3_l;

    // FFT -> selected-bin monitor streams.  Kept at top level for WCFG/Tcl.
    wire [31:0] fft0_d, fft1_d, fft2_d, fft3_d;
    wire [15:0] fft0_u, fft1_u, fft2_u, fft3_u;
    wire fft0_v, fft1_v, fft2_v, fft3_v;
    wire fft0_r, fft1_r, fft2_r, fft3_r;
    wire fft0_l, fft1_l, fft2_l, fft3_l;

    // Direct AXI-Lite test master for pd_fft_bin_monitor_4ch.
    reg [15:0] ax_awaddr=0, ax_araddr=0;
    reg [31:0] ax_wdata=0;
    reg [3:0]  ax_wstrb=4'hf;
    reg ax_awvalid=0, ax_wvalid=0, ax_bready=1, ax_arvalid=0, ax_rready=1;
    wire ax_awready, ax_wready, ax_bvalid, ax_arready, ax_rvalid;
    wire [1:0] ax_bresp, ax_rresp;
    wire [31:0] ax_rdata;

    // Top-level aliases retained in the waveform script and checked below.
    wire [3:0]  mon_valid_sticky = u_mon.valid_sticky;
    wire [3:0]  mon_frame_seen   = u_mon.frame_seen;
    wire [31:0] mon_frame_count0 = u_mon.frame_count[0];
    wire [31:0] mon_frame_count1 = u_mon.frame_count[1];
    wire [31:0] mon_frame_count2 = u_mon.frame_count[2];
    wire [31:0] mon_frame_count3 = u_mon.frame_count[3];
    wire [16:0] mon_magnitude0   = u_mon.magnitude[0];
    wire [16:0] mon_magnitude1   = u_mon.magnitude[1];
    wire [16:0] mon_magnitude2   = u_mon.magnitude[2];
    wire [16:0] mon_magnitude3   = u_mon.magnitude[3];

    pd_fft_input_adapter_4ch u_adapter (
        .clk(clk), .rst_n(rst_n), .filt_data(filt_data), .filt_dv(filt_dv),
        .input_overflow(input_overflow),
        .m_axis_cfg_ch0_tdata(cfg0_d), .m_axis_cfg_ch0_tvalid(cfg0_v), .m_axis_cfg_ch0_tready(cfg0_r),
        .m_axis_data_ch0_tdata(in0_d), .m_axis_data_ch0_tvalid(in0_v), .m_axis_data_ch0_tready(in0_r), .m_axis_data_ch0_tlast(in0_l),
        .m_axis_cfg_ch1_tdata(cfg1_d), .m_axis_cfg_ch1_tvalid(cfg1_v), .m_axis_cfg_ch1_tready(cfg1_r),
        .m_axis_data_ch1_tdata(in1_d), .m_axis_data_ch1_tvalid(in1_v), .m_axis_data_ch1_tready(in1_r), .m_axis_data_ch1_tlast(in1_l),
        .m_axis_cfg_ch2_tdata(cfg2_d), .m_axis_cfg_ch2_tvalid(cfg2_v), .m_axis_cfg_ch2_tready(cfg2_r),
        .m_axis_data_ch2_tdata(in2_d), .m_axis_data_ch2_tvalid(in2_v), .m_axis_data_ch2_tready(in2_r), .m_axis_data_ch2_tlast(in2_l),
        .m_axis_cfg_ch3_tdata(cfg3_d), .m_axis_cfg_ch3_tvalid(cfg3_v), .m_axis_cfg_ch3_tready(cfg3_r),
        .m_axis_data_ch3_tdata(in3_d), .m_axis_data_ch3_tvalid(in3_v), .m_axis_data_ch3_tready(in3_r), .m_axis_data_ch3_tlast(in3_l)
    );

    pd_feature_bd_xfft_0_0 u_fft0 (
        .aclk(clk), .aresetn(rst_n), .s_axis_config_tdata(cfg0_d), .s_axis_config_tvalid(cfg0_v), .s_axis_config_tready(cfg0_r),
        .s_axis_data_tdata(in0_d), .s_axis_data_tvalid(in0_v), .s_axis_data_tready(in0_r), .s_axis_data_tlast(in0_l),
        .m_axis_data_tdata(fft0_d), .m_axis_data_tuser(fft0_u), .m_axis_data_tvalid(fft0_v), .m_axis_data_tready(fft0_r), .m_axis_data_tlast(fft0_l));
    pd_feature_bd_xfft_ch0_3 u_fft1 (
        .aclk(clk), .aresetn(rst_n), .s_axis_config_tdata(cfg1_d), .s_axis_config_tvalid(cfg1_v), .s_axis_config_tready(cfg1_r),
        .s_axis_data_tdata(in1_d), .s_axis_data_tvalid(in1_v), .s_axis_data_tready(in1_r), .s_axis_data_tlast(in1_l),
        .m_axis_data_tdata(fft1_d), .m_axis_data_tuser(fft1_u), .m_axis_data_tvalid(fft1_v), .m_axis_data_tready(fft1_r), .m_axis_data_tlast(fft1_l));
    pd_feature_bd_xfft_ch0_4 u_fft2 (
        .aclk(clk), .aresetn(rst_n), .s_axis_config_tdata(cfg2_d), .s_axis_config_tvalid(cfg2_v), .s_axis_config_tready(cfg2_r),
        .s_axis_data_tdata(in2_d), .s_axis_data_tvalid(in2_v), .s_axis_data_tready(in2_r), .s_axis_data_tlast(in2_l),
        .m_axis_data_tdata(fft2_d), .m_axis_data_tuser(fft2_u), .m_axis_data_tvalid(fft2_v), .m_axis_data_tready(fft2_r), .m_axis_data_tlast(fft2_l));
    pd_feature_bd_xfft_ch0_5 u_fft3 (
        .aclk(clk), .aresetn(rst_n), .s_axis_config_tdata(cfg3_d), .s_axis_config_tvalid(cfg3_v), .s_axis_config_tready(cfg3_r),
        .s_axis_data_tdata(in3_d), .s_axis_data_tvalid(in3_v), .s_axis_data_tready(in3_r), .s_axis_data_tlast(in3_l),
        .m_axis_data_tdata(fft3_d), .m_axis_data_tuser(fft3_u), .m_axis_data_tvalid(fft3_v), .m_axis_data_tready(fft3_r), .m_axis_data_tlast(fft3_l));

    pd_fft_bin_monitor_4ch u_mon (
        .clk(clk), .rst_n(rst_n),
        .s_axis_ch0_tdata(fft0_d), .s_axis_ch0_tuser(fft0_u), .s_axis_ch0_tvalid(fft0_v), .s_axis_ch0_tready(fft0_r), .s_axis_ch0_tlast(fft0_l),
        .s_axis_ch1_tdata(fft1_d), .s_axis_ch1_tuser(fft1_u), .s_axis_ch1_tvalid(fft1_v), .s_axis_ch1_tready(fft1_r), .s_axis_ch1_tlast(fft1_l),
        .s_axis_ch2_tdata(fft2_d), .s_axis_ch2_tuser(fft2_u), .s_axis_ch2_tvalid(fft2_v), .s_axis_ch2_tready(fft2_r), .s_axis_ch2_tlast(fft2_l),
        .s_axis_ch3_tdata(fft3_d), .s_axis_ch3_tuser(fft3_u), .s_axis_ch3_tvalid(fft3_v), .s_axis_ch3_tready(fft3_r), .s_axis_ch3_tlast(fft3_l),
        .S_AXI_AWADDR(ax_awaddr), .S_AXI_AWVALID(ax_awvalid), .S_AXI_AWREADY(ax_awready),
        .S_AXI_WDATA(ax_wdata), .S_AXI_WSTRB(ax_wstrb), .S_AXI_WVALID(ax_wvalid), .S_AXI_WREADY(ax_wready),
        .S_AXI_BRESP(ax_bresp), .S_AXI_BVALID(ax_bvalid), .S_AXI_BREADY(ax_bready),
        .S_AXI_ARADDR(ax_araddr), .S_AXI_ARVALID(ax_arvalid), .S_AXI_ARREADY(ax_arready),
        .S_AXI_RDATA(ax_rdata), .S_AXI_RRESP(ax_rresp), .S_AXI_RVALID(ax_rvalid), .S_AXI_RREADY(ax_rready)
    );

    function [11:0] tone_sample;
        input integer bin;
        input integer sample_index;
        real angle;
        integer code;
        begin
            angle = 6.283185307179586 * bin * sample_index / FRAME_N;
            code = 2048 + $rtoi(AMP * $sin(angle));
            if(code < 0) code=0;
            if(code > 4095) code=4095;
            tone_sample=code[11:0];
        end
    endfunction

    task axil_write;
        input [15:0] addr;
        input [31:0] data;
        begin
            @(posedge clk); ax_awaddr<=addr; ax_wdata<=data; ax_awvalid<=1; ax_wvalid<=1;
            while(!(ax_awready && ax_wready)) @(posedge clk);
            @(posedge clk); ax_awvalid<=0; ax_wvalid<=0;
            while(!ax_bvalid) @(posedge clk);
        end
    endtask

    integer oi0=0, oi1=0, oi2=0, oi3=0;
    integer fl0=0, fl1=0, fl2=0, fl3=0;
    task check_fft_beat;
        input [9:0] got_idx;
        input got_last;
        input integer expected_idx;
        input integer channel;
        begin
            if(got_idx !== expected_idx[9:0]) $fatal(1,"FFT ch%0d bin mismatch: got %0d expected %0d",channel,got_idx,expected_idx);
            if(got_last !== (expected_idx == FRAME_N-1)) $fatal(1,"FFT ch%0d TLAST mismatch at bin %0d",channel,expected_idx);
        end
    endtask
    always @(posedge clk) if(rst_n && fft0_v && fft0_r) begin check_fft_beat(fft0_u[9:0],fft0_l,oi0,0); if(oi0==FRAME_N-1) begin oi0=0; fl0=fl0+1; end else oi0=oi0+1; end
    always @(posedge clk) if(rst_n && fft1_v && fft1_r) begin check_fft_beat(fft1_u[9:0],fft1_l,oi1,1); if(oi1==FRAME_N-1) begin oi1=0; fl1=fl1+1; end else oi1=oi1+1; end
    always @(posedge clk) if(rst_n && fft2_v && fft2_r) begin check_fft_beat(fft2_u[9:0],fft2_l,oi2,2); if(oi2==FRAME_N-1) begin oi2=0; fl2=fl2+1; end else oi2=oi2+1; end
    always @(posedge clk) if(rst_n && fft3_v && fft3_r) begin check_fft_beat(fft3_u[9:0],fft3_l,oi3,3); if(oi3==FRAME_N-1) begin oi3=0; fl3=fl3+1; end else oi3=oi3+1; end

    integer n;
    initial begin
        repeat(12) @(posedge clk);
        rst_n=1'b1;
        // Explicitly program all selected bins through the monitor AXI-Lite.
        axil_write(16'h0000,32'h0000000f);
        axil_write(16'h0010,BIN0); axil_write(16'h0014,BIN1);
        axil_write(16'h0018,BIN2); axil_write(16'h001c,BIN3);

        for(n=0;n<FRAMES_TO_SEND*FRAME_N;n=n+1) begin
            repeat(5) @(posedge clk); // 26 MSPS sample rate under 130 MHz clock
            filt_data <= {tone_sample(BIN3,n),tone_sample(BIN2,n),tone_sample(BIN1,n),tone_sample(BIN0,n)};
            filt_dv   <= 4'hf;
            @(posedge clk); filt_dv<=4'h0;
        end
        // Allow pipeline latency and the final output frame to drain.
        repeat(30000) @(posedge clk);

        if(input_overflow !== 4'b0000) $fatal(1,"FFT input FIFO overflow: %b",input_overflow);
        if(mon_valid_sticky !== 4'hf) $fatal(1,"Selected-bin valid mismatch: %b",mon_valid_sticky);
        if(mon_frame_seen !== 4'hf) $fatal(1,"FFT frame-seen mismatch: %b",mon_frame_seen);
        if((mon_frame_count0==0)||(mon_frame_count1==0)||(mon_frame_count2==0)||(mon_frame_count3==0))
            $fatal(1,"No completed FFT frame: %0d %0d %0d %0d",mon_frame_count0,mon_frame_count1,mon_frame_count2,mon_frame_count3);
        if((mon_magnitude0 < 17'd64)||(mon_magnitude1 < 17'd64)||(mon_magnitude2 < 17'd64)||(mon_magnitude3 < 17'd64))
            $fatal(1,"Selected-bin magnitude too small: %0d %0d %0d %0d",mon_magnitude0,mon_magnitude1,mon_magnitude2,mon_magnitude3);
        $display("FFT_CHAIN_TEST_PASS frames=%0d/%0d/%0d/%0d mag=%0d/%0d/%0d/%0d",mon_frame_count0,mon_frame_count1,mon_frame_count2,mon_frame_count3,mon_magnitude0,mon_magnitude1,mon_magnitude2,mon_magnitude3);
        $finish;
    end
    initial begin
        repeat(60000) @(posedge clk);
        $fatal(1,"FFT chain regression timeout");
    end
endmodule
