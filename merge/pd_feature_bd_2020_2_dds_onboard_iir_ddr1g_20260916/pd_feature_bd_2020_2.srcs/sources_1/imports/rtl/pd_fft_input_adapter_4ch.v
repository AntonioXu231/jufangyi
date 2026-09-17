`timescale 1ns / 1ps
// -----------------------------------------------------------------------------
// pd_fft_input_adapter_4ch
//
// Converts the four parallel 12-bit offset-binary filter outputs into four
// independent Xilinx FFT v9.1 AXI4-Stream inputs.  This is deliberately an
// input-only adapter: each FFT output remains independent so a future
// frequency-selection or DDR writer can decide how much spectrum to retain.
//
// Contract for each FFT core configured in this project:
//   - 1024 point, fixed-point, one channel, Pipelined Streaming I/O
//   - S_AXIS_DATA.TDATA[15:0]  = signed real sample
//   - S_AXIS_DATA.TDATA[31:16] = signed imaginary sample (= 0)
//   - TLAST marks sample 1023 of every 1024 accepted samples
//   - S_AXIS_CONFIG drives forward FFT plus /4 scaling for each of the five
//     radix-4 pipeline stages.  Total scale is 1/1024, preventing growth from
//     overflowing the selected 16-bit output width.
//
// The filtered source has no ready signal.  Therefore each channel owns a
// 2048-entry BRAM FIFO.  If an FFT/downstream path is stalled for too long,
// overflow is latched instead of silently claiming lossless acquisition.
// -----------------------------------------------------------------------------
module pd_fft_input_adapter_4ch #(
    parameter integer ADC_W = 12,
    parameter integer FRAME_LOG2 = 10,
    parameter [15:0] FFT_CONFIG_TDATA = 16'h0555
)(
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME clk, FREQ_HZ 130000000" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
    input  wire                 clk,
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME rst_n, POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst_n RST" *)
    input  wire                 rst_n,

    input  wire [4*ADC_W-1:0]   filt_data,
    input  wire [3:0]           filt_dv,
    output wire [3:0]           input_overflow,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH0 TDATA" *) output wire [15:0] m_axis_cfg_ch0_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH0 TVALID" *) output wire        m_axis_cfg_ch0_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH0 TREADY" *) input  wire        m_axis_cfg_ch0_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH0 TDATA" *) output wire [31:0] m_axis_data_ch0_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH0 TVALID" *) output wire        m_axis_data_ch0_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH0 TREADY" *) input  wire        m_axis_data_ch0_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH0 TLAST" *) output wire        m_axis_data_ch0_tlast,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH1 TDATA" *) output wire [15:0] m_axis_cfg_ch1_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH1 TVALID" *) output wire        m_axis_cfg_ch1_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH1 TREADY" *) input  wire        m_axis_cfg_ch1_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH1 TDATA" *) output wire [31:0] m_axis_data_ch1_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH1 TVALID" *) output wire        m_axis_data_ch1_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH1 TREADY" *) input  wire        m_axis_data_ch1_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH1 TLAST" *) output wire        m_axis_data_ch1_tlast,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH2 TDATA" *) output wire [15:0] m_axis_cfg_ch2_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH2 TVALID" *) output wire        m_axis_cfg_ch2_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH2 TREADY" *) input  wire        m_axis_cfg_ch2_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH2 TDATA" *) output wire [31:0] m_axis_data_ch2_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH2 TVALID" *) output wire        m_axis_data_ch2_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH2 TREADY" *) input  wire        m_axis_data_ch2_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH2 TLAST" *) output wire        m_axis_data_ch2_tlast,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH3 TDATA" *) output wire [15:0] m_axis_cfg_ch3_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH3 TVALID" *) output wire        m_axis_cfg_ch3_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_CFG_CH3 TREADY" *) input  wire        m_axis_cfg_ch3_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH3 TDATA" *) output wire [31:0] m_axis_data_ch3_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH3 TVALID" *) output wire        m_axis_data_ch3_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH3 TREADY" *) input  wire        m_axis_data_ch3_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_DATA_CH3 TLAST" *) output wire        m_axis_data_ch3_tlast
);

    wire [15:0] cfg_tdata [0:3];
    wire        cfg_tvalid[0:3];
    wire        cfg_tready[0:3];
    wire [31:0] data_tdata[0:3];
    wire        data_tvalid[0:3];
    wire        data_tready[0:3];
    wire        data_tlast [0:3];

    assign cfg_tready[0] = m_axis_cfg_ch0_tready;
    assign cfg_tready[1] = m_axis_cfg_ch1_tready;
    assign cfg_tready[2] = m_axis_cfg_ch2_tready;
    assign cfg_tready[3] = m_axis_cfg_ch3_tready;
    assign m_axis_cfg_ch0_tdata  = cfg_tdata[0]; assign m_axis_cfg_ch0_tvalid = cfg_tvalid[0];
    assign m_axis_cfg_ch1_tdata  = cfg_tdata[1]; assign m_axis_cfg_ch1_tvalid = cfg_tvalid[1];
    assign m_axis_cfg_ch2_tdata  = cfg_tdata[2]; assign m_axis_cfg_ch2_tvalid = cfg_tvalid[2];
    assign m_axis_cfg_ch3_tdata  = cfg_tdata[3]; assign m_axis_cfg_ch3_tvalid = cfg_tvalid[3];

    assign data_tready[0] = m_axis_data_ch0_tready;
    assign data_tready[1] = m_axis_data_ch1_tready;
    assign data_tready[2] = m_axis_data_ch2_tready;
    assign data_tready[3] = m_axis_data_ch3_tready;
    assign m_axis_data_ch0_tdata = data_tdata[0]; assign m_axis_data_ch0_tvalid = data_tvalid[0]; assign m_axis_data_ch0_tlast = data_tlast[0];
    assign m_axis_data_ch1_tdata = data_tdata[1]; assign m_axis_data_ch1_tvalid = data_tvalid[1]; assign m_axis_data_ch1_tlast = data_tlast[1];
    assign m_axis_data_ch2_tdata = data_tdata[2]; assign m_axis_data_ch2_tvalid = data_tvalid[2]; assign m_axis_data_ch2_tlast = data_tlast[2];
    assign m_axis_data_ch3_tdata = data_tdata[3]; assign m_axis_data_ch3_tvalid = data_tvalid[3]; assign m_axis_data_ch3_tlast = data_tlast[3];

    genvar g;
    generate
        for (g=0; g<4; g=g+1) begin : G_CH
            reg cfg_pending;
            reg [FRAME_LOG2-1:0] frame_pos;
            wire [ADC_W-1:0] raw = filt_data[g*ADC_W +: ADC_W];
            wire signed [15:0] signed_sample = $signed({1'b0, raw}) - (1 << (ADC_W-1));
            wire fifo_s_ready;
            wire fifo_m_valid;
            wire fifo_m_last;
            wire [31:0] fifo_m_data;
            wire fifo_overflow;
            wire in_fire = filt_dv[g] && fifo_s_ready;

            always @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    cfg_pending <= 1'b1;
                    frame_pos   <= {FRAME_LOG2{1'b0}};
                end else begin
                    if (cfg_pending && cfg_tready[g]) cfg_pending <= 1'b0;
                    if (in_fire) frame_pos <= frame_pos + 1'b1;
                end
            end

            assign cfg_tdata[g]  = FFT_CONFIG_TDATA;
            assign cfg_tvalid[g] = cfg_pending;
            // The FFT must accept configuration before it sees the first data
            // beat.  The FIFO safely stores samples while config is accepted.
            assign data_tdata[g]  = fifo_m_data;
            assign data_tlast[g]  = fifo_m_last;
            assign data_tvalid[g] = !cfg_pending && fifo_m_valid;

            pd_axis_fifo #(.DATA_W(32), .ADDR_W(11)) u_input_fifo (
                .clk(clk), .rst_n(rst_n),
                .s_tdata({16'd0, signed_sample}),
                .s_tvalid(filt_dv[g]),
                .s_tlast(frame_pos == {FRAME_LOG2{1'b1}}),
                .s_tready(fifo_s_ready),
                .m_tdata(fifo_m_data), .m_tvalid(fifo_m_valid),
                .m_tlast(fifo_m_last),
                .m_tready(data_tready[g] && !cfg_pending),
                .count(), .overflow(fifo_overflow)
            );
            assign input_overflow[g] = fifo_overflow;
        end
    endgenerate
endmodule
