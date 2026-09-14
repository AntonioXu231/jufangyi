`timescale 1ns / 1ps
// =============================================================================
// pd_ddr_bd_adapter.v -- pd_ddr_wr_top 的 IP Integrator 接口适配层
// -----------------------------------------------------------------------------
// 业务 RTL 保持扁平端口，所有 X_INTERFACE 属性集中在本文件。这样 Block
// Design 的 AXI 自动连线是显式、可检查的，同时不会把 Vivado 元数据渗入
// 环形写入和快照业务逻辑。
// =============================================================================
module pd_ddr_bd_adapter #(
    parameter integer CH_NUM     = 4,
    parameter integer ADC_W      = 12,
    parameter integer SAMPLE_W   = 48,
    parameter integer AXI_DATA_W = 64,
    parameter integer AXI_ADDR_W = 32
)(
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME CLK, FREQ_HZ 130000000, ASSOCIATED_BUSIF s_axi:m_axi_wr:m_axi_rd:m_axi_cw, ASSOCIATED_RESET rst_n" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
    input  wire                      clk,
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME RST, POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst_n RST" *)
    input  wire                      rst_n,
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME ADC_CLK, FREQ_HZ 26000000" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 adc_clk CLK" *)
    input  wire                      adc_clk,
    input  wire [CH_NUM*ADC_W-1:0]   adc_data,
    input  wire [CH_NUM-1:0]         adc_dv,
    input  wire                      cycle_start,

    output wire [CH_NUM*ADC_W-1:0]   o_feat_data,
    output wire [CH_NUM-1:0]         o_feat_dv,
    output wire                      o_feat_ovf,

    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME S_AXI, PROTOCOL AXI4LITE, ADDR_WIDTH 16, DATA_WIDTH 32" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi AWADDR" *)
    input  wire [15:0]               s_axi_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi AWPROT" *)
    input  wire [2:0]                s_axi_awprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi AWVALID" *)
    input  wire                      s_axi_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi AWREADY" *)
    output wire                      s_axi_awready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi WDATA" *)
    input  wire [31:0]               s_axi_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi WSTRB" *)
    input  wire [3:0]                s_axi_wstrb,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi WVALID" *)
    input  wire                      s_axi_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi WREADY" *)
    output wire                      s_axi_wready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi BRESP" *)
    output wire [1:0]                s_axi_bresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi BVALID" *)
    output wire                      s_axi_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi BREADY" *)
    input  wire                      s_axi_bready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi ARADDR" *)
    input  wire [15:0]               s_axi_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi ARPROT" *)
    input  wire [2:0]                s_axi_arprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi ARVALID" *)
    input  wire                      s_axi_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi ARREADY" *)
    output wire                      s_axi_arready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi RDATA" *)
    output wire [31:0]               s_axi_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi RRESP" *)
    output wire [1:0]                s_axi_rresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi RVALID" *)
    output wire                      s_axi_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi RREADY" *)
    input  wire                      s_axi_rready,

    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME M_AXI_WR, PROTOCOL AXI4, ADDR_WIDTH 32, DATA_WIDTH 64, ID_WIDTH 4, READ_WRITE_MODE WRITE_ONLY" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr AWID" *)
    output wire [3:0]                m_axi_wr_awid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr AWADDR" *)
    output wire [AXI_ADDR_W-1:0]     m_axi_wr_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr AWLEN" *)
    output wire [7:0]                m_axi_wr_awlen,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr AWSIZE" *)
    output wire [2:0]                m_axi_wr_awsize,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr AWBURST" *)
    output wire [1:0]                m_axi_wr_awburst,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr AWPROT" *)
    output wire [2:0]                m_axi_wr_awprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr AWCACHE" *)
    output wire [3:0]                m_axi_wr_awcache,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr AWVALID" *)
    output wire                      m_axi_wr_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr AWREADY" *)
    input  wire                      m_axi_wr_awready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr WDATA" *)
    output wire [AXI_DATA_W-1:0]     m_axi_wr_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr WSTRB" *)
    output wire [AXI_DATA_W/8-1:0]   m_axi_wr_wstrb,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr WLAST" *)
    output wire                      m_axi_wr_wlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr WVALID" *)
    output wire                      m_axi_wr_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr WREADY" *)
    input  wire                      m_axi_wr_wready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr BRESP" *)
    input  wire [1:0]                m_axi_wr_bresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr BVALID" *)
    input  wire                      m_axi_wr_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_wr BREADY" *)
    output wire                      m_axi_wr_bready,

    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME M_AXI_RD, PROTOCOL AXI4, ADDR_WIDTH 32, DATA_WIDTH 64, ID_WIDTH 4, READ_WRITE_MODE READ_ONLY" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd ARID" *)
    output wire [3:0]                m_axi_rd_arid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd ARADDR" *)
    output wire [AXI_ADDR_W-1:0]     m_axi_rd_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd ARLEN" *)
    output wire [7:0]                m_axi_rd_arlen,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd ARSIZE" *)
    output wire [2:0]                m_axi_rd_arsize,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd ARBURST" *)
    output wire [1:0]                m_axi_rd_arburst,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd ARPROT" *)
    output wire [2:0]                m_axi_rd_arprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd ARCACHE" *)
    output wire [3:0]                m_axi_rd_arcache,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd ARVALID" *)
    output wire                      m_axi_rd_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd ARREADY" *)
    input  wire                      m_axi_rd_arready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd RDATA" *)
    input  wire [AXI_DATA_W-1:0]     m_axi_rd_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd RRESP" *)
    input  wire [1:0]                m_axi_rd_rresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd RLAST" *)
    input  wire                      m_axi_rd_rlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd RVALID" *)
    input  wire                      m_axi_rd_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_rd RREADY" *)
    output wire                      m_axi_rd_rready,

    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME M_AXI_CW, PROTOCOL AXI4, ADDR_WIDTH 32, DATA_WIDTH 64, ID_WIDTH 4, READ_WRITE_MODE WRITE_ONLY" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw AWID" *)
    output wire [3:0]                m_axi_cw_awid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw AWADDR" *)
    output wire [AXI_ADDR_W-1:0]     m_axi_cw_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw AWLEN" *)
    output wire [7:0]                m_axi_cw_awlen,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw AWSIZE" *)
    output wire [2:0]                m_axi_cw_awsize,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw AWBURST" *)
    output wire [1:0]                m_axi_cw_awburst,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw AWPROT" *)
    output wire [2:0]                m_axi_cw_awprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw AWCACHE" *)
    output wire [3:0]                m_axi_cw_awcache,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw AWVALID" *)
    output wire                      m_axi_cw_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw AWREADY" *)
    input  wire                      m_axi_cw_awready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw WDATA" *)
    output wire [AXI_DATA_W-1:0]     m_axi_cw_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw WSTRB" *)
    output wire [AXI_DATA_W/8-1:0]   m_axi_cw_wstrb,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw WLAST" *)
    output wire                      m_axi_cw_wlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw WVALID" *)
    output wire                      m_axi_cw_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw WREADY" *)
    input  wire                      m_axi_cw_wready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw BRESP" *)
    input  wire [1:0]                m_axi_cw_bresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw BVALID" *)
    input  wire                      m_axi_cw_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 m_axi_cw BREADY" *)
    output wire                      m_axi_cw_bready,

    output wire                      irq,
    output wire [15:0]               dbg_ddr
);

    // AWPROT/ARPROT are AXI4-Lite metadata. pd_ddr_axil has no policy on them
    // and therefore intentionally ignores these two adapter-only inputs.
    wire unused_prot = &{1'b0, s_axi_awprot, s_axi_arprot};
    // PS7 HP ports expose no USER sideband. Keep DataMover's fixed-width
    // USER outputs local instead of advertising a mismatched AXI interface.
    wire [3:0] m_axi_wr_awuser_i;
    wire [3:0] m_axi_rd_aruser_i;
    wire [3:0] m_axi_cw_awuser_i;

    pd_ddr_wr_top #(
        .CH_NUM     (CH_NUM),
        .ADC_W      (ADC_W),
        .SAMPLE_W   (SAMPLE_W),
        .AXI_DATA_W (AXI_DATA_W),
        .AXI_ADDR_W (AXI_ADDR_W)
    ) u_ddr (
        .clk             (clk),
        .rst_n           (rst_n),
        .adc_clk         (adc_clk),
        .adc_data        (adc_data),
        .adc_dv          (adc_dv),
        .o_feat_data     (o_feat_data),
        .o_feat_dv       (o_feat_dv),
        .o_feat_ovf      (o_feat_ovf),
        .cycle_start     (cycle_start),
        .s_axi_awaddr    (s_axi_awaddr),
        .s_axi_awvalid   (s_axi_awvalid),
        .s_axi_awready   (s_axi_awready),
        .s_axi_wdata     (s_axi_wdata),
        .s_axi_wstrb     (s_axi_wstrb),
        .s_axi_wvalid    (s_axi_wvalid),
        .s_axi_wready    (s_axi_wready),
        .s_axi_bresp     (s_axi_bresp),
        .s_axi_bvalid    (s_axi_bvalid),
        .s_axi_bready    (s_axi_bready),
        .s_axi_araddr    (s_axi_araddr),
        .s_axi_arvalid   (s_axi_arvalid),
        .s_axi_arready   (s_axi_arready),
        .s_axi_rdata     (s_axi_rdata),
        .s_axi_rresp     (s_axi_rresp),
        .s_axi_rvalid    (s_axi_rvalid),
        .s_axi_rready    (s_axi_rready),
        .m_axi_wr_awid   (m_axi_wr_awid),
        .m_axi_wr_awaddr (m_axi_wr_awaddr),
        .m_axi_wr_awlen  (m_axi_wr_awlen),
        .m_axi_wr_awsize (m_axi_wr_awsize),
        .m_axi_wr_awburst(m_axi_wr_awburst),
        .m_axi_wr_awprot (m_axi_wr_awprot),
        .m_axi_wr_awcache(m_axi_wr_awcache),
        .m_axi_wr_awuser (m_axi_wr_awuser_i),
        .m_axi_wr_awvalid(m_axi_wr_awvalid),
        .m_axi_wr_awready(m_axi_wr_awready),
        .m_axi_wr_wdata  (m_axi_wr_wdata),
        .m_axi_wr_wstrb  (m_axi_wr_wstrb),
        .m_axi_wr_wlast  (m_axi_wr_wlast),
        .m_axi_wr_wvalid (m_axi_wr_wvalid),
        .m_axi_wr_wready (m_axi_wr_wready),
        .m_axi_wr_bresp  (m_axi_wr_bresp),
        .m_axi_wr_bvalid (m_axi_wr_bvalid),
        .m_axi_wr_bready (m_axi_wr_bready),
        .m_axi_rd_arid   (m_axi_rd_arid),
        .m_axi_rd_araddr (m_axi_rd_araddr),
        .m_axi_rd_arlen  (m_axi_rd_arlen),
        .m_axi_rd_arsize (m_axi_rd_arsize),
        .m_axi_rd_arburst(m_axi_rd_arburst),
        .m_axi_rd_arprot (m_axi_rd_arprot),
        .m_axi_rd_arcache(m_axi_rd_arcache),
        .m_axi_rd_aruser (m_axi_rd_aruser_i),
        .m_axi_rd_arvalid(m_axi_rd_arvalid),
        .m_axi_rd_arready(m_axi_rd_arready),
        .m_axi_rd_rdata  (m_axi_rd_rdata),
        .m_axi_rd_rresp  (m_axi_rd_rresp),
        .m_axi_rd_rlast  (m_axi_rd_rlast),
        .m_axi_rd_rvalid (m_axi_rd_rvalid),
        .m_axi_rd_rready (m_axi_rd_rready),
        .m_axi_cw_awid   (m_axi_cw_awid),
        .m_axi_cw_awaddr (m_axi_cw_awaddr),
        .m_axi_cw_awlen  (m_axi_cw_awlen),
        .m_axi_cw_awsize (m_axi_cw_awsize),
        .m_axi_cw_awburst(m_axi_cw_awburst),
        .m_axi_cw_awprot (m_axi_cw_awprot),
        .m_axi_cw_awcache(m_axi_cw_awcache),
        .m_axi_cw_awuser (m_axi_cw_awuser_i),
        .m_axi_cw_awvalid(m_axi_cw_awvalid),
        .m_axi_cw_awready(m_axi_cw_awready),
        .m_axi_cw_wdata  (m_axi_cw_wdata),
        .m_axi_cw_wstrb  (m_axi_cw_wstrb),
        .m_axi_cw_wlast  (m_axi_cw_wlast),
        .m_axi_cw_wvalid (m_axi_cw_wvalid),
        .m_axi_cw_wready (m_axi_cw_wready),
        .m_axi_cw_bresp  (m_axi_cw_bresp),
        .m_axi_cw_bvalid (m_axi_cw_bvalid),
        .m_axi_cw_bready (m_axi_cw_bready),
        .irq             (irq),
        .dbg_ddr         (dbg_ddr)
    );

endmodule
