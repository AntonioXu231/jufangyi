// =============================================================================
// pd_feature_sys_top.v  --  统一 IP 顶层 (A 侧采集 + B 侧特征提取)
// -----------------------------------------------------------------------------
// 合并来源:
//   * hgcode/pd_feature  -> pd_feature_top (B 侧特征提取, 契约接口主体)
//   * project_2          -> adc_capture + async_fifo (A 侧采集 + 跨时钟 CDC)
//
// 对外暴露《pd_feature IP 接口契约 v2》规定的端口形态:
//   clk / rst_n / adc_clk / adc_data[4*12] / adc_dv[4] / sync_in
//   + AXI4-Lite slave(s_axi_*) + AXI4-Stream master(m_axis_*) + irq
//
// 数据流:
//   ADC(SAMPLE_HZ, adc_clk 域) -> [pd_adc_cdc x4](SAMPLE_HZ->CLK_HZ CDC, 12bit)
//        -> pd_feature_top (峰值/1024 相位窗/n·I·P·Q/PRPD/8B 事件包)
//        -> M_AXIS (事件流, type=0x00/0x01) / AXI-Lite (配置与状态)
//
// 速率配置: 当前验证默认 130MHz / 26MSPS (CDC 5:1)。
//           若后续切回生产更高速率，必须同步修改 BD 的 SAMPLE_HZ、ADC
//           时钟属性与 XDC，且保持 CLK_HZ/SAMPLE_HZ 为整数。
//
// 说明: project_2 自带的 axil_regs_4ch(采集侧寄存器) 与 axis_master_4ch
//       (原始样本流 type=0xA1) 与契约冲突, 已在此合并中剔除, 统一使用
//       pd_feature_top 的契约寄存器映射与事件包格式。
// =============================================================================
`timescale 1ns / 1ps
`include "pd_defines.vh"

module pd_feature_sys_top #(
    parameter integer NUM_CH    = 4,
    parameter integer ADC_W     = `PD_ADC_W,
    parameter integer CLK_HZ    = 130000000,   // 系统时钟 (Hz), PL 主时钟
    parameter integer SAMPLE_HZ = 26000000,    // ADC 采样率 (Hz), 必须整除 CLK_HZ
    // 0: adc_data/adc_dv 已由上游公共 CDC 送入 clk 域。
    // 1: 保持兼容原有独立通道 adc_clk -> clk CDC 架构。
    parameter integer INPUT_CDC = 1
)(
    // ================= 时钟 / 复位 =================
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME CLK, FREQ_HZ 130000000, ASSOCIATED_BUSIF s_axi:m_axis, ASSOCIATED_RESET rst_n" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
    input  wire                     clk,          // 系统时钟 (默认 130 MHz)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME RST, POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst_n RST" *)
    input  wire                     rst_n,        // 低有效异步复位
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME ADC_CLK, FREQ_HZ 26000000" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 adc_clk CLK" *)
    input  wire                     adc_clk,      // ADC 位时钟 (默认 26MHz, CDC 源)

    // ================= ADC 接口 (adc_clk 域, 偏移二进制 12bit) =================
    input  wire [NUM_CH*ADC_W-1:0]  adc_data,     // ch0 在最低位
    input  wire [NUM_CH-1:0]        adc_dv,       // 每通道样本有效

    // ================= 同步输入 =================
    input  wire                     sync_in,      // 工频同步整形后方波 (50~400Hz)

    // ================= AXI4-Lite Slave =================
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME S_AXI, PROTOCOL AXI4LITE, ADDR_WIDTH 16, DATA_WIDTH 32" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi AWADDR" *)
    input  wire [15:0]   s_axi_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi AWPROT" *)
    input  wire [2:0]    s_axi_awprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi AWVALID" *)
    input  wire          s_axi_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi AWREADY" *)
    output wire          s_axi_awready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi WDATA" *)
    input  wire [31:0]   s_axi_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi WSTRB" *)
    input  wire [3:0]    s_axi_wstrb,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi WVALID" *)
    input  wire          s_axi_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi WREADY" *)
    output wire          s_axi_wready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi BRESP" *)
    output wire [1:0]    s_axi_bresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi BVALID" *)
    output wire          s_axi_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi BREADY" *)
    input  wire          s_axi_bready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi ARADDR" *)
    input  wire [15:0]   s_axi_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi ARPROT" *)
    input  wire [2:0]    s_axi_arprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi ARVALID" *)
    input  wire          s_axi_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi ARREADY" *)
    output wire          s_axi_arready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi RDATA" *)
    output wire [31:0]   s_axi_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi RRESP" *)
    output wire [1:0]    s_axi_rresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi RVALID" *)
    output wire          s_axi_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axi RREADY" *)
    input  wire          s_axi_rready,

    // ================= 事件输出 (AXI-Stream Master) =================
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME M_AXIS, TDATA_NUM_BYTES 8, HAS_TLAST 1, HAS_TKEEP 0, HAS_TSTRB 0" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 m_axis TDATA" *)
    output wire [63:0]   m_axis_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 m_axis TVALID" *)
    output wire          m_axis_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 m_axis TLAST" *)
    output wire          m_axis_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 m_axis TREADY" *)
    input  wire          m_axis_tready,

    // Internal PL event-accept indication for the DDR auto-snapshot path.
    // This is not an AXI interface and does not alter the AXI-Lite/AXI-Stream
    // interface contract of pd_feature_sys_top.
    output wire [NUM_CH-1:0] o_event_accept,

    // ================= 中断 =================
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME IRQ, SENSITIVITY LEVEL_HIGH" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:interrupt:1.0 irq INTERRUPT" *)
    output wire          irq
);

    // =========================================================================
    // clk 域样本流
    //
    // INPUT_CDC=0 用于统一采集架构：pd_pack48 在 ADC 域完成一次四通道
    // 对齐，随后由公共 48-bit FIFO 把 Path-B 送到这里。这样特征链不再对
    // 四通道分别做 CDC，避免各通道 FIFO 空标志独立同步造成的样本错位。
    // =========================================================================
    wire [NUM_CH*ADC_W-1:0] adc_100;
    wire [NUM_CH-1:0]       adc_dv_100;

    genvar i;
    generate
        if (INPUT_CDC != 0) begin : G_ADC_CDC
            // rst_n is generated in the 130 MHz PL domain. The ADC clock is
            // independent, so release its reset only after two adc_clk edges.
            (* ASYNC_REG = "TRUE" *) reg [1:0] adc_rst_sync;
            always @(posedge adc_clk or negedge rst_n) begin
                if (!rst_n)
                    adc_rst_sync <= 2'b00;
                else
                    adc_rst_sync <= {adc_rst_sync[0], 1'b1};
            end
            wire adc_rst_n = adc_rst_sync[1];

            for (i = 0; i < NUM_CH; i = i + 1) begin : GEN_CH
                pd_adc_cdc #(.ADC_W(ADC_W), .DEPTH(64),
                             .CLK_HZ(CLK_HZ), .SAMPLE_HZ(SAMPLE_HZ)) u_cdc (
                    .adc_clk      (adc_clk),
                    .adc_rst_n    (adc_rst_n),
                    .clk          (clk),
                    .rst_n        (rst_n),
                    .adc_data     (adc_data[i*ADC_W +: ADC_W]),
                    .adc_dv       (adc_dv[i]),
                    .adc_data_100 (adc_100[i*ADC_W +: ADC_W]),
                    .adc_dv_100   (adc_dv_100[i])
                );
            end
        end else begin : G_CLK_DOMAIN_INPUT
            assign adc_100    = adc_data;
            assign adc_dv_100 = adc_dv;
        end
    endgenerate

    // =========================================================================
    // 特征提取 IP (契约接口主体)
    // =========================================================================
    pd_feature_top #(
        .NUM_CH               (NUM_CH),
        .ADC_W                (`PD_ADC_W),
        .PH_W                 (`PD_PH_W),
        .TS_W                 (`PD_TS_W),
        .EV_W                 (`PD_EV_W),
        .FIFO_AW              (8),
        .C_S_AXI_DATA_WIDTH   (32),
        .C_S_AXI_ADDR_WIDTH   (16)
    ) u_feat (
        .clk           (clk),
        .rst_n         (rst_n),

        .adc_data      (adc_100),
        .adc_dv        (adc_dv_100),
        .sync_in       (sync_in),

        .s_axi_awaddr  (s_axi_awaddr),
        .s_axi_awprot  (s_axi_awprot),
        .s_axi_awvalid (s_axi_awvalid),
        .s_axi_awready (s_axi_awready),
        .s_axi_wdata   (s_axi_wdata),
        .s_axi_wstrb   (s_axi_wstrb),
        .s_axi_wvalid  (s_axi_wvalid),
        .s_axi_wready  (s_axi_wready),
        .s_axi_bresp   (s_axi_bresp),
        .s_axi_bvalid  (s_axi_bvalid),
        .s_axi_bready  (s_axi_bready),
        .s_axi_araddr  (s_axi_araddr),
        .s_axi_arprot  (s_axi_arprot),
        .s_axi_arvalid (s_axi_arvalid),
        .s_axi_arready (s_axi_arready),
        .s_axi_rdata   (s_axi_rdata),
        .s_axi_rresp   (s_axi_rresp),
        .s_axi_rvalid  (s_axi_rvalid),
        .s_axi_rready  (s_axi_rready),

        .m_axis_tdata  (m_axis_tdata),
        .m_axis_tvalid (m_axis_tvalid),
        .m_axis_tlast  (m_axis_tlast),
        .m_axis_tready (m_axis_tready),

        .o_event_accept(o_event_accept),

        .irq           (irq)
    );

endmodule
