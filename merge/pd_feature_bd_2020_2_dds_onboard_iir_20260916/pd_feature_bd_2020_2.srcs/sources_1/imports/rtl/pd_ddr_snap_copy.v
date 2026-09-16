`timescale 1ns / 1ps
// =============================================================================
// pd_ddr_snap_copy.v  --  冻结快照区拷贝引擎（环形区 -> 快照区）
// -----------------------------------------------------------------------------
// 契约 §3.4 / §3.5：
//   冻结完成后必须把"完整周期原始数据"搬进独立的冻结快照区，使 PS 后续
//   TCP 下载时读到的是一份**不会被环形写指针覆盖**的静态副本，
//   同时采集环路不中断（读快照不干扰采集写入）。
//
// 实现方式：复用 AXI DataMover(dm_cp) 的 MM2S + S2MM 两个通道做
// 「内存 -> 流 -> 内存」的环回：
//      MM2S: 从环形区 src 读  ──AXI-Stream──>  S2MM: 写入快照区 dst
//   这是 AMD 官方推荐的 DataMover memory-to-memory 环回接法，
//   比自己写 AXI 主状态机更省事，且天然获得 4KB 边界自动拆包能力。
//
// 分片：一次命令的长度上限受 BTT(23bit) 与 Stream FIFO 深度限制，
//   本模块按 CHUNK_BYTES 分片（默认 6144B = 256 个 192bit 块），
//   每片发两条命令（S2MM 写 + MM2S 读），等 S2MM 状态返回后再发下一片，
//   **严格串行**，避免两条命令在同一通道里乱序。
//
// 地址对齐：CHUNK_BYTES 取 24 的整数倍，src/dst/len 必须满足接口契约的
//   24B 原始块对齐（同时自然满足 DataMover 无 DRE 的 8B 对齐约束）。
// =============================================================================
`include "pd_ddr_defines.vh"

module pd_ddr_snap_copy #(
    parameter integer AXI_ADDR_W  = 32,
    parameter integer CHUNK_BLOCKS= 256                       // 每片块数
)(
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire                 i_clear,          // 清除粘滞错误（resume/软件确认）

    // ---------------- 启动/参数 ----------------
    input  wire                 i_start,          // 启动脉冲（freeze_done 触发）
    input  wire [AXI_ADDR_W-1:0] i_src_addr,      // 环形区起始（FREEZE_BASE）
    input  wire [AXI_ADDR_W-1:0] i_dst_addr,      // 快照区基址（SNAP_BASE）
    input  wire [31:0]          i_len_bytes,      // FREEZE_LEN
    input  wire [AXI_ADDR_W-1:0] i_ring_base,     // 环形区基址
    input  wire [31:0]          i_ring_size,      // 环形区长度
    output reg                  o_busy,
    output reg                  o_done,           // 拷贝完成脉冲
    output reg                  o_err,
    output reg  [31:0]          o_chunk_cnt,
    output reg  [31:0]          o_bytes_done,

    // ---------------- dm_cp MM2S（读环形区）----------------
    output wire [`DM_CMD_W-1:0] m_rd_cmd_tdata,
    output reg                  m_rd_cmd_tvalid,
    input  wire                 m_rd_cmd_tready,
    input  wire [7:0]           s_rd_sts_tdata,
    input  wire                 s_rd_sts_tvalid,
    output wire                 m_rd_sts_tready,
    input  wire                 i_mm2s_err,

    // ---------------- dm_cp S2MM（写快照区）----------------
    output wire [`DM_CMD_W-1:0] m_wr_cmd_tdata,
    output reg                  m_wr_cmd_tvalid,
    input  wire                 m_wr_cmd_tready,
    input  wire [7:0]           s_wr_sts_tdata,
    input  wire                 s_wr_sts_tvalid,
    output wire                 m_wr_sts_tready,
    input  wire                 i_s2mm_err
);

    localparam [31:0] CHUNK_BYTES = CHUNK_BLOCKS * `PD_BLK_BYTES;   // 6144

    localparam [2:0] S_IDLE  = 3'd0;
    localparam [2:0] S_CMD_W = 3'd1;    // 发 S2MM 命令（先备好"写入口"）
    localparam [2:0] S_CMD_R = 3'd2;    // 发 MM2S 命令（再打开"读出口"）
    localparam [2:0] S_WAIT  = 3'd3;    // 等 S2MM 状态返回
    localparam [2:0] S_NEXT  = 3'd4;    // 推进地址 / 结束

    reg [2:0]  state;
    reg [31:0] src_addr_r, dst_addr_r, remain_r, cur_len_r;
    reg [7:0]  rd_sts_r, wr_sts_r;
    reg        rd_sts_seen, wr_sts_seen;

    // 一个冻结窗口可跨越环形区尾部。每个 DataMover 命令都必须在环尾前
    // 截断；下一片从 ring_base 继续，不能线性读进 SNAP 分区。
    wire [31:0] ring_end       = i_ring_base + i_ring_size;
    wire [31:0] src_to_ring_end = ring_end - src_addr_r;
    wire [31:0] copy_chunk_len = (remain_r >= CHUNK_BYTES) ? CHUNK_BYTES : remain_r;
    wire [31:0] cur_len_next   = (copy_chunk_len <= src_to_ring_end)
                               ? copy_chunk_len : src_to_ring_end;

    // ---- 命令字（同 pd_ddr_ring_wr 的 72bit 布局）----
    assign m_wr_cmd_tdata = {
        {`DM_RSVD_W{1'b0}}, {`DM_TAG_W{1'b0}}, dst_addr_r,
        1'b0, 1'b0, {`DM_DSA_W{1'b0}}, 1'b1, cur_len_r[`DM_BTT_W-1:0]
    };
    assign m_rd_cmd_tdata = {
        {`DM_RSVD_W{1'b0}}, {`DM_TAG_W{1'b0}}, src_addr_r,
        1'b0, 1'b0, {`DM_DSA_W{1'b0}}, 1'b1, cur_len_r[`DM_BTT_W-1:0]
    };
    assign m_rd_sts_tready = 1'b1;
    assign m_wr_sts_tready = 1'b1;

    wire rd_sts_ok_now = s_rd_sts_tvalid && (s_rd_sts_tdata == `DM_STS_OK);
    wire wr_sts_ok_now = s_wr_sts_tvalid && (s_wr_sts_tdata == `DM_STS_OK);
    wire copy_fault_now = i_mm2s_err || i_s2mm_err ||
                          (s_rd_sts_tvalid && !rd_sts_ok_now) ||
                          (s_wr_sts_tvalid && !wr_sts_ok_now);
    wire src_align24;
    wire dst_align24;
    wire len_align24;
    pd_align24_check u_src_align24 (.value(i_src_addr),  .aligned(src_align24));
    pd_align24_check u_dst_align24 (.value(i_dst_addr),  .aligned(dst_align24));
    pd_align24_check u_len_align24 (.value(i_len_bytes), .aligned(len_align24));

    wire start_valid = (i_len_bytes != 32'd0) &&
                       (i_src_addr >= i_ring_base) &&
                       (i_src_addr < i_ring_base + i_ring_size) &&
                       (i_len_bytes <= i_ring_size) &&
                       src_align24 && dst_align24 && len_align24;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state        <= S_IDLE;
            m_wr_cmd_tvalid <= 1'b0;
            m_rd_cmd_tvalid <= 1'b0;
            src_addr_r   <= 32'd0;
            dst_addr_r   <= 32'd0;
            remain_r     <= 32'd0;
            cur_len_r    <= 32'd0;
            o_busy       <= 1'b0;
            o_done       <= 1'b0;
            o_err        <= 1'b0;
            o_chunk_cnt  <= 32'd0;
            o_bytes_done <= 32'd0;
            rd_sts_r     <= 8'h00;
            wr_sts_r     <= 8'h00;
            rd_sts_seen  <= 1'b0;
            wr_sts_seen  <= 1'b0;
        end else begin
            o_done <= 1'b0;
            // 状态字采样
            if (s_rd_sts_tvalid) begin
                rd_sts_r <= s_rd_sts_tdata;
                if (rd_sts_ok_now) rd_sts_seen <= 1'b1;
            end
            if (s_wr_sts_tvalid) begin
                wr_sts_r <= s_wr_sts_tdata;
                if (wr_sts_ok_now) wr_sts_seen <= 1'b1;
            end
            // 错误必须粘滞，避免 AXI-Lite 轮询漏掉单拍 DataMover 状态。
            if (i_clear)
                o_err <= 1'b0;
            else if (copy_fault_now)
                o_err <= 1'b1;

            case (state)
                // ---------------------------------------------------------
                S_IDLE: begin
                    m_wr_cmd_tvalid <= 1'b0;
                    m_rd_cmd_tvalid <= 1'b0;
                    if (i_start && start_valid) begin
                        src_addr_r   <= i_src_addr;
                        dst_addr_r   <= i_dst_addr;
                        remain_r     <= i_len_bytes;
                        o_busy       <= 1'b1;
                        o_chunk_cnt  <= 32'd0;
                        o_bytes_done <= 32'd0;
                        rd_sts_seen  <= 1'b0;
                        wr_sts_seen  <= 1'b0;
                        o_err        <= 1'b0;
                        state        <= S_CMD_W;
                    end
                end

                // ---- 先给 S2MM 下命令：先把"写入口"打开，防止读出来的
                //      数据无处可去而把 Stream 链路堵死 ----
                S_CMD_W: begin
                    // valid 必须至少保持到一个真实的 valid&&ready 握手。
                    // 不能仅看 ready：DataMover 常态 ready=1 时，原实现会
                    // 在 valid 首次置位的同一拍又将其撤销，命令从未送达。
                    if (!m_wr_cmd_tvalid) begin
                        cur_len_r       <= cur_len_next;
                        m_wr_cmd_tvalid <= 1'b1;
                    end else if (m_wr_cmd_tready) begin
                        m_wr_cmd_tvalid <= 1'b0;
                        state           <= S_CMD_R;
                    end
                end

                // ---- 再给 MM2S 下命令：数据开始从环形区流出 ----
                S_CMD_R: begin
                    if (!m_rd_cmd_tvalid) begin
                        m_rd_cmd_tvalid <= 1'b1;
                    end else if (m_rd_cmd_tready) begin
                        m_rd_cmd_tvalid <= 1'b0;
                        state           <= S_WAIT;
                    end
                end

                // ---- 同时等 MM2S 与 S2MM 正常状态，避免读状态延迟或
                //      报错时提前向软件宣告完成。 ----
                S_WAIT: begin
                    if ((rd_sts_seen || rd_sts_ok_now) &&
                        (wr_sts_seen || wr_sts_ok_now))
                        state <= S_NEXT;
                end

                // ---- 推进地址，或结束 ----
                S_NEXT: begin
                    o_chunk_cnt <= o_chunk_cnt + 32'd1;
                    o_bytes_done <= o_bytes_done + cur_len_r;
                    if (remain_r > cur_len_r) begin
                        remain_r   <= remain_r - cur_len_r;
                        src_addr_r <= ((src_addr_r + cur_len_r) >= ring_end)
                                    ? i_ring_base : (src_addr_r + cur_len_r);
                        dst_addr_r <= dst_addr_r + cur_len_r;
                        rd_sts_seen <= 1'b0;
                        wr_sts_seen <= 1'b0;
                        state      <= S_CMD_W;
                    end else begin
                        o_busy <= 1'b0;
                        o_done <= 1'b1;
                        state  <= S_IDLE;
                    end
                end

                default: state <= S_IDLE;
            endcase

            // 配置不合法时拒绝启动，防止一次错误的源地址覆盖快照区。
            if (state == S_IDLE && i_start && !start_valid)
                o_err <= 1'b1;

            // 任一 DataMover 硬错误或错误状态立即中止本次事务，不产生 done。
            if (o_busy && copy_fault_now) begin
                m_wr_cmd_tvalid <= 1'b0;
                m_rd_cmd_tvalid <= 1'b0;
                o_busy          <= 1'b0;
                state           <= S_IDLE;
            end
        end
    end

endmodule
