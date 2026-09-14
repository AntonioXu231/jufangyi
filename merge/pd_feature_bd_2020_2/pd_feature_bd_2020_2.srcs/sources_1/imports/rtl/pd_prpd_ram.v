// =============================================================================
// pd_prpd_ram.v  --  PRPD (相位分辨局部放电) 图谱存储
// -----------------------------------------------------------------------------
// 每通道一片 1024(相位窗) x 16bit(视在电荷量) 的真双口 RAM, 目标实现为 Block RAM:
//   端口 A: PL 侧读-改-写 (取该相位窗的历史最大值) —— 读写同址
//   端口 B: PS 侧经 AXI-Lite 只读
// 支持整片清零 (cfg_clear 脉冲后逐地址写 0)。
//
// 【重要 · 勿改回】BRAM 每个物理端口只有一组地址线, 读写必须同址。
//   若端口 A 出现"写 clr_addr / 读 pl_addr"两址并存, Vivado 会无视
//   ram_style="block" 退回分布式 RAM(每通道 704 LUT, 4 通道 2816 LUT,
//   Slice 占用冲到 97%)。改动端口 A 时务必保持 addr_a 读写共用。
// =============================================================================
`timescale 1ns / 1ps
`include "pd_defines.vh"

module pd_prpd_ram #(
    parameter integer PH_W   = `PD_PH_W,
    parameter integer DEPTH  = 1024,
    parameter integer DATA_W = 16
)(
    input  wire                  clk,
    input  wire                  rst_n,

    // ---- PL 侧 (core) ----
    input  wire [PH_W-1:0]       pl_addr,
    output reg  [DATA_W-1:0]     pl_rdata,
    input  wire [DATA_W-1:0]     pl_wdata,
    input  wire                  pl_we,

    // ---- PS 侧 (AXI-Lite) ----
    input  wire [PH_W-1:0]       ps_addr,
    output reg  [DATA_W-1:0]     ps_rdata,

    // ---- 控制 ----
    input  wire                  clear_req,     // 1 = 请求整片清零
    output wire                  clear_busy
);

    localparam AW = (DEPTH == 1024) ? 10 :
                    (DEPTH ==  512) ?  9 :
                    (DEPTH == 2048) ? 11 : 10;

    (* ram_style = "block" *) reg [DATA_W-1:0] mem [0:DEPTH-1];

    reg [AW-1:0] clr_addr;
    reg          clr_run;

    assign clear_busy = clr_run;

    // ---- 端口 A: 地址 / 数据 / 写使能 统一 --------------------------------
    // BRAM 单个物理端口只有一组地址线, 读写必须同址。
    // 原写法在 clr_run 时"写 clr_addr / 读 pl_addr"两个地址并存, 物理 BRAM 无法实现,
    // 综合器因此无视 ram_style="block" 退回 LUTRAM(每通道 704 LUT, 4 通道共 2816)。
    // 修正: 读写共用 addr_a。清零扫描期间 PL 不做 RMW, 读出为无关值, 语义不受影响。
    wire [AW-1:0]     addr_a = clr_run ? clr_addr : pl_addr[AW-1:0];
    wire              we_a   = clr_run | pl_we;
    wire [DATA_W-1:0] din_a  = clr_run ? {DATA_W{1'b0}} : pl_wdata;

    // ---- 端口 A: PL 读-改-写 (读写同址, 读出 1 拍延迟, READ_FIRST 语义) ----
    always @(posedge clk) begin
        if (we_a) mem[addr_a] <= din_a;
        pl_rdata <= mem[addr_a];
    end

    // ---- 端口 B: PS 只读 ----
    always @(posedge clk) begin
        ps_rdata <= mem[ps_addr[AW-1:0]];
    end

    // ---- 清零状态机: 逐地址写 0 ----
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            clr_run  <= 1'b0;
            clr_addr <= {AW{1'b0}};
        end else begin
            if (!clr_run && clear_req) begin
                clr_run  <= 1'b1;
                clr_addr <= {AW{1'b0}};
            end else if (clr_run) begin
                if (clr_addr == {AW{1'b1}})
                    clr_run <= 1'b0;
                else
                    clr_addr <= clr_addr + 1'b1;
            end
        end
    end

endmodule
