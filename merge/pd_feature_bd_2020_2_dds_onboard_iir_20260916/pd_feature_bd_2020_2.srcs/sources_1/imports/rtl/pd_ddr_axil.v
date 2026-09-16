`timescale 1ns / 1ps
// =============================================================================
// pd_ddr_axil.v  --  DDR 环形缓存 / 冻结快照 AXI4-Lite 从机
// -----------------------------------------------------------------------------
// 契约 §4.3：FREEZE_BASE_LO(0x1028) / FREEZE_BASE_HI(0x102C) /
//            FREEZE_LEN(0x1030) / FREEZE_CTRL(0x1034)
// 说明：现有 pd_axil_regs 已占用 0x1000~0x101C 全局区。为免与正在演进的
//       v3 寄存器冲突，本从机使用**独立 4KB 页 0x2000**（addr[15:12]=2），
//       但冻结组保持契约的组内偏移 0x28/0x2C/0x30/0x34，
//       后续若要并入 pd_axil_regs，把页基址换成 0x1000 即可，无需改 RTL。
//
// 寄存器表（页内偏移）：
//   0x00 DDR_CTRL      RW  [0]acq_en  [1]sw_rst(W1P)  [2]snap_start(W1P,调试)
//   0x04 DDR_STATUS    RO  [0]acq_en [1]copy_busy [2]hiwm [3]freeze_done
//                          [4]snap_overrun [5]err [6]copy_done(sticky)
//                          [7]copy_pending [8]copy_err [9]snapshot_cfg_err
//   0x08 RING_BASE     RO  编译期固定，采集运行中不可改
//   0x0C RING_SIZE     RO  编译期固定，采集运行中不可改
//   0x10 RING_WR_PTR   RO  环内字节偏移
//   0x14 RING_WRAP     RO  回绕次数
//   0x18 WR_BYTES_LO   RO
//   0x1C WR_BYTES_HI   RO
//   0x20 SNAP_BASE     RW
//   0x24 SNAP_SIZE     RW
//   0x28 FREEZE_BASE_LO RO  ← 契约偏移
//   0x2C FREEZE_BASE_HI RO  ← 契约偏移
//   0x30 FREEZE_LEN     RO  ← 契约偏移
//   0x34 FREEZE_CTRL    RW  [0]trig(W1P) [1]resume(W1P) / RO [8]done
//   0x38 CMD_CNT        RO
//   0x3C AVAIL_BYTES    RO
//   0x40 SNAP_CHUNKS    RO  已完成的拷贝分片数
//   0x44 SNAP_BYTES     RO  本次已完成的拷贝字节数
//   0x48 SNAP_SEQ       RO  成功快照序号（每次 copy_done +1）
// =============================================================================
`include "pd_ddr_defines.vh"

module pd_ddr_axil #(
    parameter integer AW = 16,     // 地址位宽（页内 12bit + 页号 4bit）
    parameter integer DW = 32
)(
    input  wire            s_axi_aclk,
    input  wire            s_axi_aresetn,

    // ---- AXI4-Lite 从机口 ----
    input  wire [AW-1:0]   s_axi_awaddr,
    input  wire            s_axi_awvalid,
    output reg             s_axi_awready,
    input  wire [DW-1:0]   s_axi_wdata,
    input  wire [DW/8-1:0] s_axi_wstrb,
    input  wire            s_axi_wvalid,
    output reg             s_axi_wready,
    output reg  [1:0]      s_axi_bresp,
    output reg             s_axi_bvalid,
    input  wire            s_axi_bready,

    input  wire [AW-1:0]   s_axi_araddr,
    input  wire            s_axi_arvalid,
    output reg             s_axi_arready,
    output reg  [DW-1:0]   s_axi_rdata,
    output reg  [1:0]      s_axi_rresp,
    output reg             s_axi_rvalid,
    input  wire            s_axi_rready,

    // ---- 与 pd_ddr_ring_wr / pd_ddr_snap_copy 的扁平接口 ----
    output reg             o_acq_en,
    output reg             o_sw_rst,
    output reg  [31:0]     o_snap_base,
    output reg  [31:0]     o_snap_size,
    output reg             o_freeze_trig,
    output reg             o_freeze_resume,
    output reg             o_snap_start,      // 调试用：手动触发一次拷贝

    input  wire            i_copy_busy,
    input  wire            i_copy_done,
    input  wire            i_copy_err,
    input  wire            i_copy_pending,
    input  wire            i_snapshot_cfg_err,
    input  wire            i_hiwm,
    input  wire            i_freeze_done,
    input  wire            i_snap_overrun,
    input  wire            i_err,
    input  wire [31:0]     i_wr_off,
    input  wire [31:0]     i_wrap_cnt,
    input  wire [63:0]     i_wr_bytes,
    input  wire [31:0]     i_cmd_cnt,
    input  wire [31:0]     i_avail_bytes,
    input  wire [31:0]     i_freeze_base,
    input  wire [31:0]     i_freeze_len,
    input  wire [31:0]     i_copy_chunk_cnt,
    input  wire [31:0]     i_copy_bytes_done
);

    localparam [11:0] A_CTRL      = 12'h00;
    localparam [11:0] A_STATUS    = 12'h04;
    localparam [11:0] A_RING_BASE = 12'h08;
    localparam [11:0] A_RING_SIZE = 12'h0C;
    localparam [11:0] A_WR_PTR    = 12'h10;
    localparam [11:0] A_WRAP      = 12'h14;
    localparam [11:0] A_WB_LO     = 12'h18;
    localparam [11:0] A_WB_HI     = 12'h1C;
    localparam [11:0] A_SNAP_BASE = 12'h20;
    localparam [11:0] A_SNAP_SIZE = 12'h24;
    localparam [11:0] A_FZ_BASE_L = 12'h28;
    localparam [11:0] A_FZ_BASE_H = 12'h2C;
    localparam [11:0] A_FZ_LEN    = 12'h30;
    localparam [11:0] A_FZ_CTRL   = 12'h34;
    localparam [11:0] A_CMD_CNT   = 12'h38;
    localparam [11:0] A_AVAIL     = 12'h3C;
    localparam [11:0] A_SNAP_CHUNK= 12'h40;
    localparam [11:0] A_SNAP_BYTES= 12'h44;
    localparam [11:0] A_SNAP_SEQ  = 12'h48;

    // ---------------- 写通道：AW/W 同时收，再回 B ----------------
    reg [11:0] waddr;
    reg [DW-1:0] wdata;
    reg [DW/8-1:0] wstrb;
    reg        aw_done, w_done;
    reg        copy_done_sticky;
    reg [31:0] snap_seq;

    always @(posedge s_axi_aclk or negedge s_axi_aresetn) begin
        if (!s_axi_aresetn) begin
            s_axi_awready <= 1'b0; s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0; s_axi_bresp   <= 2'b00;
            aw_done <= 1'b0; w_done <= 1'b0; waddr <= 12'd0;
            wdata <= {DW{1'b0}}; wstrb <= {DW/8{1'b0}};
            copy_done_sticky <= 1'b0; snap_seq <= 32'd0;
        end else begin
            if (aw_done && w_done && !s_axi_bvalid &&
                (((waddr == A_CTRL) && wstrb[0] && wdata[2]) ||
                 ((waddr == A_FZ_CTRL) && wstrb[0] && (wdata[0] || wdata[1]))))
                copy_done_sticky <= 1'b0;
            else if (i_copy_done)
                copy_done_sticky <= 1'b1;
            if (i_copy_done)
                snap_seq <= snap_seq + 32'd1;
            if (s_axi_awvalid && !aw_done) begin
                waddr         <= s_axi_awaddr[11:0];
                aw_done       <= 1'b1;
                s_axi_awready <= 1'b1;
            end else
                s_axi_awready <= 1'b0;

            if (s_axi_wvalid && !w_done) begin
                wdata         <= s_axi_wdata;
                wstrb         <= s_axi_wstrb;
                w_done        <= 1'b1;
                s_axi_wready  <= 1'b1;
            end else
                s_axi_wready  <= 1'b0;

            if (aw_done && w_done && !s_axi_bvalid) begin
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;      // OKAY
                aw_done      <= 1'b0;
                w_done       <= 1'b0;
            end else if (s_axi_bvalid && s_axi_bready)
                s_axi_bvalid <= 1'b0;
        end
    end

    // ---------------- 寄存器写入（W1P 脉冲在 B 响应时产生一次）----------------
    wire wr_fire = aw_done && w_done && !s_axi_bvalid;

    always @(posedge s_axi_aclk or negedge s_axi_aresetn) begin
        if (!s_axi_aresetn) begin
            o_acq_en        <= 1'b0;
            o_snap_base     <= `DDR_SNAP_BASE;
            o_snap_size     <= `DDR_SNAP_SIZE;
            o_freeze_trig   <= 1'b0;
            o_freeze_resume <= 1'b0;
            o_snap_start    <= 1'b0;
            o_sw_rst        <= 1'b0;
        end else begin
            o_freeze_trig   <= 1'b0;   // 脉冲型，默认撤销
            o_freeze_resume <= 1'b0;
            o_snap_start    <= 1'b0;
            o_sw_rst        <= 1'b0;
            if (wr_fire) begin
                case (waddr)
                    A_CTRL: begin
                        if (wstrb[0]) o_acq_en <= wdata[0];
                        if (wstrb[0] && wdata[1]) o_sw_rst <= 1'b1;
                        if (wstrb[0] && wdata[2]) begin
                            o_snap_start <= 1'b1;
                        end
                    end
                    A_SNAP_BASE: o_snap_base <= wdata;
                    A_SNAP_SIZE: o_snap_size <= wdata;
                    A_FZ_CTRL: begin
                        if (wstrb[0] && wdata[0]) begin
                            o_freeze_trig <= 1'b1;
                        end
                        if (wstrb[0] && wdata[1]) begin
                            o_freeze_resume <= 1'b1;
                        end
                    end
                    default: ;
                endcase
            end
        end
    end

    // ---------------- 读通道 ----------------
    reg [11:0] raddr;

    always @(posedge s_axi_aclk or negedge s_axi_aresetn) begin
        if (!s_axi_aresetn) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rdata   <= 32'd0;
            s_axi_rresp   <= 2'b00;
            raddr         <= 12'd0;
        end else begin
            if (s_axi_arvalid && !s_axi_rvalid) begin
                raddr         <= s_axi_araddr[11:0];
                s_axi_arready <= 1'b1;
                s_axi_rvalid  <= 1'b1;
                s_axi_rresp   <= 2'b00;
                case (s_axi_araddr[11:0])
                    A_CTRL:      s_axi_rdata <= {29'd0, o_snap_start, o_sw_rst, o_acq_en};
                    A_STATUS:    s_axi_rdata <= {22'd0, i_snapshot_cfg_err, i_copy_err,
                                                 i_copy_pending, copy_done_sticky, i_err,
                                                 i_snap_overrun, i_freeze_done, i_hiwm,
                                                 i_copy_busy, o_acq_en};
                    A_RING_BASE: s_axi_rdata <= `DDR_RING_BASE;
                    A_RING_SIZE: s_axi_rdata <= `DDR_RING_SIZE;
                    A_WR_PTR:    s_axi_rdata <= i_wr_off;
                    A_WRAP:      s_axi_rdata <= i_wrap_cnt;
                    A_WB_LO:     s_axi_rdata <= i_wr_bytes[31:0];
                    A_WB_HI:     s_axi_rdata <= i_wr_bytes[63:32];
                    A_SNAP_BASE: s_axi_rdata <= o_snap_base;
                    A_SNAP_SIZE: s_axi_rdata <= o_snap_size;
                    A_FZ_BASE_L: s_axi_rdata <= i_freeze_base;
                    A_FZ_BASE_H: s_axi_rdata <= 32'd0;      // 32bit 地址，高半字预留
                    A_FZ_LEN:    s_axi_rdata <= i_freeze_len;
                    A_FZ_CTRL:   s_axi_rdata <= {17'd0, i_snapshot_cfg_err, i_copy_err,
                                                 i_snap_overrun, i_copy_pending,
                                                 copy_done_sticky, i_copy_busy,
                                                 i_freeze_done, 7'd0, o_acq_en};
                    A_CMD_CNT:   s_axi_rdata <= i_cmd_cnt;
                    A_AVAIL:     s_axi_rdata <= i_avail_bytes;
                    A_SNAP_CHUNK:s_axi_rdata <= i_copy_chunk_cnt;
                    A_SNAP_BYTES:s_axi_rdata <= i_copy_bytes_done;
                    A_SNAP_SEQ:  s_axi_rdata <= snap_seq;
                    default:     s_axi_rdata <= 32'hDEAD_BEEF;
                endcase
            end else begin
                s_axi_arready <= 1'b0;
                if (s_axi_rvalid && s_axi_rready)
                    s_axi_rvalid <= 1'b0;
            end
        end
    end

endmodule
