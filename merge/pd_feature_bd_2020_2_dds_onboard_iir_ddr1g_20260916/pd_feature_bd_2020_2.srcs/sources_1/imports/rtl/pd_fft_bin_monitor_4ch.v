`timescale 1ns / 1ps
// -----------------------------------------------------------------------------
// pd_fft_bin_monitor_4ch
//
// Four independent FFT result consumers.  Every S_AXIS channel is permanently
// ready so the Pipelined Streaming FFT never stalls; all bins except the
// selected bin are intentionally discarded.  This is the correct first
// frequency-selection endpoint for the present design: it retains useful,
// software-readable spectra information without trying to force four full-rate
// spectra through the existing event DMA.
//
// FFT input contract (Xilinx FFT v9.1 configuration used in this project):
//   TDATA[15:0]  = XK_RE, signed
//   TDATA[31:16] = XK_IM, signed
//   TUSER[9:0]   = XK_INDEX (0..1023)
//
// Register map (AXI-Lite offsets):
//   0x00 CTRL      [3:0] channel enable, bit8 W1C result_valid
//   0x04 STATUS    [3:0] result_valid, [7:4] frame_seen
//   0x10..0x1C     BIN_SEL0..3, bits[9:0]
//   0x20..0x2C     MAG0..3, bits[16:0], max(|Re|,|Im|)+min/2
//   0x30..0x3C     FRAME_CNT0..3, increments at every FFT TLAST
//
// Note: a 1024-point FFT has bin spacing Fs/1024.  BIN_SEL selects an FFT bin,
// not an arbitrary 1-kHz frequency.  The later frequency-to-bin software
// layer must round target_hz * 1024 / sample_hz and report the actual bin.
// -----------------------------------------------------------------------------
module pd_fft_bin_monitor_4ch #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 16
)(
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME clk, FREQ_HZ 130000000, ASSOCIATED_BUSIF S_AXI:S_AXIS_CH0:S_AXIS_CH1:S_AXIS_CH2:S_AXIS_CH3" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *) input wire clk,
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME rst_n, POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst_n RST" *) input wire rst_n,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH0 TDATA" *) input wire [31:0] s_axis_ch0_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH0 TUSER" *) input wire [15:0] s_axis_ch0_tuser,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH0 TVALID" *) input wire s_axis_ch0_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH0 TREADY" *) output wire s_axis_ch0_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH0 TLAST" *) input wire s_axis_ch0_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH1 TDATA" *) input wire [31:0] s_axis_ch1_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH1 TUSER" *) input wire [15:0] s_axis_ch1_tuser,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH1 TVALID" *) input wire s_axis_ch1_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH1 TREADY" *) output wire s_axis_ch1_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH1 TLAST" *) input wire s_axis_ch1_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH2 TDATA" *) input wire [31:0] s_axis_ch2_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH2 TUSER" *) input wire [15:0] s_axis_ch2_tuser,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH2 TVALID" *) input wire s_axis_ch2_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH2 TREADY" *) output wire s_axis_ch2_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH2 TLAST" *) input wire s_axis_ch2_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH3 TDATA" *) input wire [31:0] s_axis_ch3_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH3 TUSER" *) input wire [15:0] s_axis_ch3_tuser,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH3 TVALID" *) input wire s_axis_ch3_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH3 TREADY" *) output wire s_axis_ch3_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_CH3 TLAST" *) input wire s_axis_ch3_tlast,

    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME S_AXI, PROTOCOL AXI4LITE, ADDR_WIDTH 16, DATA_WIDTH 32, FREQ_HZ 130000000" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWADDR" *) input wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_AWADDR,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWVALID" *) input wire S_AXI_AWVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWREADY" *) output wire S_AXI_AWREADY,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WDATA" *) input wire [31:0] S_AXI_WDATA,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WSTRB" *) input wire [3:0] S_AXI_WSTRB,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WVALID" *) input wire S_AXI_WVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WREADY" *) output wire S_AXI_WREADY,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BRESP" *) output wire [1:0] S_AXI_BRESP,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BVALID" *) output wire S_AXI_BVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BREADY" *) input wire S_AXI_BREADY,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARADDR" *) input wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_ARADDR,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARVALID" *) input wire S_AXI_ARVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARREADY" *) output wire S_AXI_ARREADY,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RDATA" *) output wire [31:0] S_AXI_RDATA,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RRESP" *) output wire [1:0] S_AXI_RRESP,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RVALID" *) output wire S_AXI_RVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RREADY" *) input wire S_AXI_RREADY
);
    reg aw_hold, w_hold, bvalid, rvalid;
    reg [C_S_AXI_ADDR_WIDTH-1:0] awaddr_hold;
    reg [31:0] wdata_hold, rdata;
    reg [3:0] wstrb_hold;
    wire aw_hs = S_AXI_AWVALID && S_AXI_AWREADY;
    wire w_hs  = S_AXI_WVALID  && S_AXI_WREADY;
    wire write_fire = !bvalid && (aw_hold || aw_hs) && (w_hold || w_hs);
    wire [C_S_AXI_ADDR_WIDTH-1:0] waddr = aw_hold ? awaddr_hold : S_AXI_AWADDR;
    wire [31:0] wdata = w_hold ? wdata_hold : S_AXI_WDATA;
    wire [3:0] wstrb = w_hold ? wstrb_hold : S_AXI_WSTRB;
    assign S_AXI_AWREADY = !aw_hold && !bvalid;
    assign S_AXI_WREADY  = !w_hold && !bvalid;
    assign S_AXI_BVALID = bvalid; assign S_AXI_BRESP = 2'b00;
    assign S_AXI_ARREADY = !rvalid; assign S_AXI_RVALID = rvalid;
    assign S_AXI_RRESP = 2'b00; assign S_AXI_RDATA = rdata;

    reg [3:0] enable, valid_sticky, frame_seen;
    reg [9:0] bin_sel [0:3];
    reg [16:0] magnitude [0:3];
    reg [31:0] frame_count [0:3];
    integer i;

    function [15:0] abs16;
        input signed [15:0] x;
        begin abs16 = x[15] ? (x == -32768 ? 16'h7fff : -x) : x; end
    endfunction
    function [16:0] mag_approx;
        input signed [15:0] re;
        input signed [15:0] im;
        reg [15:0] ar, ai, hi, lo;
        begin
            ar=abs16(re); ai=abs16(im);
            if (ar >= ai) begin hi=ar; lo=ai; end else begin hi=ai; lo=ar; end
            mag_approx={1'b0,hi}+({1'b0,lo}>>1);
        end
    endfunction

    wire [31:0] fft_data [0:3]; wire [15:0] fft_user[0:3];
    wire fft_valid[0:3]; wire fft_last[0:3];
    assign fft_data[0]=s_axis_ch0_tdata; assign fft_user[0]=s_axis_ch0_tuser; assign fft_valid[0]=s_axis_ch0_tvalid; assign fft_last[0]=s_axis_ch0_tlast;
    assign fft_data[1]=s_axis_ch1_tdata; assign fft_user[1]=s_axis_ch1_tuser; assign fft_valid[1]=s_axis_ch1_tvalid; assign fft_last[1]=s_axis_ch1_tlast;
    assign fft_data[2]=s_axis_ch2_tdata; assign fft_user[2]=s_axis_ch2_tuser; assign fft_valid[2]=s_axis_ch2_tvalid; assign fft_last[2]=s_axis_ch2_tlast;
    assign fft_data[3]=s_axis_ch3_tdata; assign fft_user[3]=s_axis_ch3_tuser; assign fft_valid[3]=s_axis_ch3_tvalid; assign fft_last[3]=s_axis_ch3_tlast;
    assign s_axis_ch0_tready=1'b1; assign s_axis_ch1_tready=1'b1;
    assign s_axis_ch2_tready=1'b1; assign s_axis_ch3_tready=1'b1;

    always @(posedge clk or negedge rst_n) begin
        if(!rst_n) begin
            aw_hold<=0; w_hold<=0; bvalid<=0; awaddr_hold<=0; wdata_hold<=0; wstrb_hold<=0;
        end else begin
            if(write_fire) begin aw_hold<=0; w_hold<=0; bvalid<=1; end
            else begin
                if(aw_hs) begin aw_hold<=1; awaddr_hold<=S_AXI_AWADDR; end
                if(w_hs) begin w_hold<=1; wdata_hold<=S_AXI_WDATA; wstrb_hold<=S_AXI_WSTRB; end
            end
            if(bvalid && S_AXI_BREADY) bvalid<=0;
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if(!rst_n) begin
            enable<=4'hf; valid_sticky<=0; frame_seen<=0;
            for(i=0;i<4;i=i+1) begin bin_sel[i]<=0; magnitude[i]<=0; frame_count[i]<=0; end
        end else begin
            if(write_fire) begin
                case(waddr[7:0])
                    8'h00: begin
                        if(wstrb[0]) begin
                            enable<=wdata[3:0];
                            if(wdata[8]) valid_sticky<=0;
                        end
                    end
                    8'h10: if(wstrb[0] || wstrb[1]) bin_sel[0]<=wdata[9:0];
                    8'h14: if(wstrb[0] || wstrb[1]) bin_sel[1]<=wdata[9:0];
                    8'h18: if(wstrb[0] || wstrb[1]) bin_sel[2]<=wdata[9:0];
                    8'h1c: if(wstrb[0] || wstrb[1]) bin_sel[3]<=wdata[9:0];
                    default: begin end
                endcase
            end
            for(i=0;i<4;i=i+1) begin
                if(fft_valid[i] && fft_last[i]) begin frame_seen[i]<=1'b1; frame_count[i]<=frame_count[i]+1'b1; end
                if(fft_valid[i] && enable[i] && (fft_user[i][9:0] == bin_sel[i])) begin
                    magnitude[i]<=mag_approx(fft_data[i][15:0], fft_data[i][31:16]);
                    valid_sticky[i]<=1'b1;
                end
            end
        end
    end

    reg [31:0] read_mux;
    always @* begin
        read_mux=0;
        case(S_AXI_ARADDR[7:0])
            8'h00: read_mux={28'd0,enable};
            8'h04: read_mux={24'd0,frame_seen,valid_sticky};
            8'h10: read_mux={22'd0,bin_sel[0]}; 8'h14: read_mux={22'd0,bin_sel[1]};
            8'h18: read_mux={22'd0,bin_sel[2]}; 8'h1c: read_mux={22'd0,bin_sel[3]};
            8'h20: read_mux={15'd0,magnitude[0]}; 8'h24: read_mux={15'd0,magnitude[1]};
            8'h28: read_mux={15'd0,magnitude[2]}; 8'h2c: read_mux={15'd0,magnitude[3]};
            8'h30: read_mux=frame_count[0]; 8'h34: read_mux=frame_count[1];
            8'h38: read_mux=frame_count[2]; 8'h3c: read_mux=frame_count[3];
            default: read_mux=0;
        endcase
    end
    always @(posedge clk or negedge rst_n) begin
        if(!rst_n) begin rvalid<=0; rdata<=0; end
        else if(S_AXI_ARVALID && S_AXI_ARREADY) begin rvalid<=1; rdata<=read_mux; end
        else if(rvalid && S_AXI_RREADY) rvalid<=0;
    end
endmodule
