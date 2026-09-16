`timescale 1ns / 1ps
// =============================================================================
// pd_ddr_ring_wr.v  --  DDR 环形写入管理器（PL 侧 Path A 核心）
// -----------------------------------------------------------------------------
// 依据：
//   [1] 《接口契约 v1.0》§3 IF-3（48bit 字 / 192bit 块 / 24B 对齐 / 分区 / 冻结）
//   [2] 《接口契约 v1.0》§4.3（FREEZE_BASE_LO/HI、FREEZE_LEN、FREEZE_CTRL）
//   [3] 《开发目标.docx》§2.3.1「DDR 写入管理模块」与风险 3（带宽/覆盖）
//   [4] PG022 AXI DataMover 命令字 / 状态字
//
// ─────────────────────────────────────────────────────────────────────────────
// 一、为什么必须自己做环形指针，DataMover 不负责这件事
// ─────────────────────────────────────────────────────────────────────────────
//   AXI DataMover 只认「(起始地址, 字节数)」这一条命令，它按顺序把数据写到
//   DA 开始的连续物理地址上，**不会**在到达环形区末尾时自动折回基址。
//   因此"回绕"必须由本模块在数据链路层之上处理：把一次逻辑突发拆成
//   「尾段 + 头段」两条命令，第二条命令的地址重新指向 RING_BASE。
//
// ─────────────────────────────────────────────────────────────────────────────
// 二、三条地址对齐铁律（本模块全部参数的取值依据）
// ─────────────────────────────────────────────────────────────────────────────
//   ① 24B 块对齐（契约 §3.3）：RING_BASE % 24 == 0 且 RING_SIZE % 24 == 0。
//      这样"第 k 个 192bit 块"永远完整落在 [BASE+24k, BASE+24k+24) 内，
//      绝不会横跨回绕点，PS 侧按 24B 对齐读即可无错位解出 4 通道。
//   ② 8B AXI 对齐：DataMover 关闭 DRE 时起始地址必须对齐 AXI 数据位宽(8B)。
//      24 = 8x3，故满足 ① 即自动满足 ②，无需额外处理。
//   ③ 4KB 边界：AXI 协议禁止单次突发跨越 4KB。**这条由 DataMover 内部自动
//      拆包处理**（PG022 明确说明），本模块无需干预，但 RING_BASE 取
//      LCM(4096,24)=12288 的整数倍，可从源头减少拆分次数。
//
// ─────────────────────────────────────────────────────────────────────────────
// 三、字节信用（credit）机制：保证 DataMover 绝不"等米下锅"
// ─────────────────────────────────────────────────────────────────────────────
//   avail_bytes = 已推入 AXI-Stream Data FIFO 但尚未下发命令的字节数。
//     生产侧：pd_pack192 每被接收 1 个 64bit beat -> +8（i_beat_en）
//     消费侧：本模块每发出 1 条命令        -> -seg_len
//   **铁律：只有当 avail_bytes >= BURST_BYTES 时才下发命令。**
//   即整段数据已经躺在 FIFO 里才命令 DataMover 来取，DataMover 永远不会因为
//   上游供不上而挂住 AXI 总线（那会拖死整条 HP 通道）。
//   推论：FIFO 深度必须 >= BURST_BYTES，本设计 512x8B=4096B > 1536B，余量充足。
//
// ─────────────────────────────────────────────────────────────────────────────
// 四、状态机（2 态，命令与数据解耦，可连续流水）
// ─────────────────────────────────────────────────────────────────────────────
//        ┌──────────┐  avail>=BURST_BYTES 且 acq_en   ┌──────────┐
//        │  S_IDLE  │ ──────────────────────────────> │  S_CMD   │
//        │ (攒数据) │                                 │(发命令)  │
//        └──────────┘ <────────────────────────────── └──────────┘
//                        cmd_tready：指针前进/回绕、扣减信用
//   注意：命令通道只传"地址+长度"，数据由 DataMover 直接从 Stream FIFO 取，
//         两者并行。当前 26MSPS 档为 156MB/s，1536B/命令约 101.6k 命令/秒。
//
// ─────────────────────────────────────────────────────────────────────────────
// 五、冻结快照的"地址锁存"（契约 §3.5 步骤 2）
// ─────────────────────────────────────────────────────────────────────────────
//   PS 写 FREEZE_TRIG -> 本模块置 freeze_arm
//   第 1 个周期边界(i_cycle_start)：锁存 起始块地址  -> freeze_wait_end
//   第 2 个周期边界(i_cycle_start)：锁存 结束块地址  -> freeze_done，给出
//        FREEZE_BASE / FREEZE_LEN（字节），并触发 pd_ddr_snap_copy 搬快照区。
//   **采集环路不中断**：本模块只锁存地址，不停止写指针（契约 §1 性能指标）。
//   **覆盖保护**：环形深度 344ms >> 1 个工频周期 20ms，快照搬完前不会被冲掉；
//      若写指针二次进入冻结区间，o_snap_overrun 拉高上报（契约 风险 3 兜底）。
// =============================================================================
`include "pd_ddr_defines.vh"

module pd_ddr_ring_wr #(
    parameter integer AXI_ADDR_W   = 32,
    parameter integer AXI_DATA_W   = 64,
    parameter [31:0]  RING_BASE    = `DDR_RING_BASE,      // 32'h1000_2000
    parameter [31:0]  RING_SIZE    = `DDR_RING_SIZE,      // 32'h07FF_E000
    parameter integer BURST_BLOCKS = `DDR_BURST_BLOCKS,   // 64 块 = 1536 B
    parameter integer AVAIL_W      = 20,                  // 信用计数器位宽(1MB)
    parameter [31:0]  OVF_THRESH   = 3072                 // 高水位告警(Bytes)
)(
    input  wire                 clk,
    input  wire                 rst_n,

    // ---------------- 采集控制 ----------------
    input  wire                 i_acq_en,        // 采集使能（PS 经 AXI-Lite 下发）

    // ---------------- 数据生产节拍 ----------------
    // 由 pd_pack192 的 (m_axis_tvalid & m_axis_tready) 直接驱动，
    // 表示"又有 8 字节进入了 AXI-Stream Data FIFO"。
    input  wire                 i_beat_en,

    // 上游溢出指示（异步 FIFO 满 / Data FIFO 满导致丢样本）
    input  wire                 i_up_ovf,

    // ---------------- AXI DataMover S2MM 命令通道（本模块 -> IP）----------------
    output wire [`DM_CMD_W-1:0] m_cmd_tdata,
    output reg                  m_cmd_tvalid,
    input  wire                 m_cmd_tready,

    // ---------------- AXI DataMover S2MM 状态通道（IP -> 本模块）----------------
    input  wire [7:0]           s_sts_tdata,
    input  wire                 s_sts_tvalid,
    output wire                 s_sts_tready,
    input  wire                 i_s2mm_err,      // DataMover 硬错误指示

    // ---------------- 周期边界 / 冻结控制 ----------------
    input  wire                 i_cycle_start,   // AC: 工频周期边界; DC: 固定窗边界
    input  wire                 i_freeze_trig,   // 冻结触发 (W1P 脉冲)
    input  wire                 i_freeze_resume, // 恢复 (W1P 脉冲)

    // ---------------- 环形状态输出 ----------------
    output reg  [31:0]          o_wr_addr,       // 当前写绝对地址 = BASE + off
    output reg  [31:0]          o_wr_off,        // 环内字节偏移
    output reg  [31:0]          o_blk_idx,       // 当前块号 (off/24)
    output reg  [31:0]          o_wrap_cnt,      // 回绕次数
    output reg  [31:0]          o_cmd_cnt,       // 已下发命令数
    output reg  [63:0]          o_wr_bytes,      // 累计写入字节数 (64bit 不溢出)
    output wire [AVAIL_W-1:0]   o_avail_bytes,   // 当前信用
    output reg                  o_hiwm,          // 高水位（FIFO 将满，需关注带宽）
    output reg                  o_ovf,           // 采集侧丢数（上游 FIFO 满）

    // ---------------- 冻结快照寄存器（契约 §4.3）----------------
    output reg                  o_freeze_done,   // FREEZE_CTRL[8]
    output reg  [31:0]          o_freeze_base,   // FREEZE_BASE_LO
    output reg  [31:0]          o_freeze_len,    // FREEZE_LEN (字节)
    output reg                  o_snap_overrun,  // 冻结区被新数据追上

    // ---------------- 调试 ----------------
    output reg  [31:0]          o_last_btt,      // 最近一条命令的 BTT
    output reg  [7:0]           o_sts_tdata,     // 最近一条状态字
    output reg                  o_err,           // 错误汇总
    output reg  [1:0]           o_state,
    // 已接受但尚未收到 DataMover 状态的写命令全部完成。
    // 冻结快照必须等待该条件，不能在 DDR 尾部突发尚未落地时开始读回。
    output wire                 o_write_idle
);

    // =========================================================================
    // 0) 参数自检（仅仿真期生效，综合忽略）
    // =========================================================================
    localparam integer BLK_BYTES   = `PD_BLK_BYTES;                 // 24
    localparam integer AXI_BYTES   = AXI_DATA_W / 8;                // 8
    localparam [31:0]  BURST_BYTES = BURST_BLOCKS * BLK_BYTES;      // 1536

    // synthesis translate_off
    initial begin
        if (RING_BASE % BLK_BYTES != 0)
            $display("[ERROR] pd_ddr_ring_wr: RING_BASE=%h 不是 %0d 字节对齐（契约 §3.3）",
                     RING_BASE, BLK_BYTES);
        if (RING_SIZE % BLK_BYTES != 0)
            $display("[ERROR] pd_ddr_ring_wr: RING_SIZE=%h 不是 %0d 字节对齐，回绕后块会跨边界",
                     RING_SIZE, BLK_BYTES);
        if (BURST_BYTES % BLK_BYTES != 0)
            $display("[ERROR] pd_ddr_ring_wr: BURST_BYTES=%0d 不是 %0d 的整数倍",
                     BURST_BYTES, BLK_BYTES);
        if (BURST_BYTES / AXI_BYTES > 256)
            $display("[ERROR] pd_ddr_ring_wr: 突发 %0d beat 超过 DataMover 单次上限 256",
                     BURST_BYTES / AXI_BYTES);
        else
            $display("[INFO ] pd_ddr_ring_wr: RING_BASE=%h SIZE=%h BURST=%0dB(%0d beat)",
                     RING_BASE, RING_SIZE, BURST_BYTES, BURST_BYTES / AXI_BYTES);
    end
    // synthesis translate_on

    // =========================================================================
    // 1) 状态定义与内部寄存器
    // =========================================================================
    localparam [1:0] S_IDLE = 2'd0;
    localparam [1:0] S_CMD  = 2'd1;

    reg [31:0]          wr_off;        // 环内字节偏移 [0, RING_SIZE)
    reg [31:0]          seg_len_r;     // 本条命令长度
    reg [31:0]          cmd_addr_r;    // 本条命令的绝对地址
    reg [AVAIL_W-1:0]   avail_bytes;   // 字节信用
    reg [31:0]          ring_remain_r; // 本条命令起点到环尾的剩余字节
    reg [7:0]           wr_inflight;   // 已被 DataMover 接受、尚未完成的写命令数

    // 冻结相关
    reg                 freeze_arm;    // 已收到触发，等第一个周期边界
    reg                 freeze_wait;   // 已锁起始，等第二个周期边界
    reg                 freeze_commit_wait; // 已锁结束，等边界数据真正写入 DDR
    reg [31:0]          f_start_off;
    reg [31:0]          f_end_off;
    reg [63:0]          f_start_total;
    reg [31:0]          commit_bytes_left; // 周期终点后仍需下发的 24B 对齐尾量

    // 以 3 个 64-bit beat = 1 个 24-byte 原始块跟踪“已进入 Data FIFO”的
    // 逻辑流位置。它独立于大突发命令指针，因此周期边界不会被 1536B
    // 命令粒度量化。
    reg [1:0]           stream_beat_phase;
    reg [31:0]          stream_off;
    reg [63:0]          stream_total;
    wire                stream_block_done = i_beat_en && (stream_beat_phase == 2'd2);
    wire [31:0]         stream_off_at_edge = stream_block_done
                              ? (((stream_off + BLK_BYTES) >= RING_SIZE)
                                 ? 32'd0 : (stream_off + BLK_BYTES))
                              : stream_off;
    wire [63:0]         stream_total_at_edge = stream_total
                              + (stream_block_done ? BLK_BYTES : 0);

    // =========================================================================
    // 2) 组合逻辑：本条命令的长度（含回绕拆分）与地址
    // =========================================================================
    wire [31:0] ring_remain = RING_SIZE - wr_off;
    wire [31:0] normal_seg_len = (ring_remain >= BURST_BYTES)
                               ? BURST_BYTES : ring_remain;
    wire [31:0] commit_cap_len = (commit_bytes_left < BURST_BYTES)
                               ? commit_bytes_left : BURST_BYTES;
    wire [31:0] commit_seg_len = (ring_remain >= commit_cap_len)
                               ? commit_cap_len : ring_remain;
    wire [31:0] cmd_addr   = RING_BASE + wr_off;

    // 命令字组装（PG022，72bit）
    //   [71:68]RSVD [67:64]TAG [63:32]ADDR [31]DRR [30]EOF [29:24]DSA [23]TYPE [22:0]BTT
    assign m_cmd_tdata = {
        {`DM_RSVD_W{1'b0}},                 // [71:68] RSVD
        {`DM_TAG_W{1'b0}},                  // [67:64] TAG（不用）
        cmd_addr_r,                         // [63:32] 目标地址（写 DDR）
        1'b0,                               // [31]    DRR：关闭 DRE，无需重对齐
        1'b0,                               // [30]    EOF：不用 TLAST，长度由 BTT 定
        {`DM_DSA_W{1'b0}},                  // [29:24] DSA
        1'b1,                               // [23]    TYPE=1 INCR（地址递增）
        seg_len_r[`DM_BTT_W-1:0]            // [22:0]  BTT
    };

    assign s_sts_tready = 1'b1;             // 状态通道恒接收，避免 IP 侧堵塞

    // =========================================================================
    // 3) 主状态机：攒够一个突发 -> 发命令 -> 推进指针
    // =========================================================================
    wire cmd_fire = m_cmd_tvalid & m_cmd_tready;
    wire sts_fire = s_sts_tvalid & s_sts_tready;
    wire [63:0] issued_bytes_at_edge = o_wr_bytes
                      + (cmd_fire ? {32'd0, seg_len_r} : 64'd0);
    wire [63:0] freeze_tail_at_edge =
                      (stream_total_at_edge > issued_bytes_at_edge)
                      ? (stream_total_at_edge - issued_bytes_at_edge) : 64'd0;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            o_state      <= S_IDLE;
            m_cmd_tvalid <= 1'b0;
            wr_off       <= 32'd0;
            seg_len_r    <= 32'd0;
            cmd_addr_r   <= 32'd0;
            ring_remain_r<= RING_SIZE;
            wr_inflight   <= 8'd0;
            avail_bytes  <= {AVAIL_W{1'b0}};
            o_wr_addr    <= RING_BASE;
            o_wr_off     <= 32'd0;
            o_blk_idx    <= 32'd0;
            o_wrap_cnt   <= 32'd0;
            o_cmd_cnt    <= 32'd0;
            o_wr_bytes   <= 64'd0;
            o_last_btt   <= 32'd0;
            o_hiwm       <= 1'b0;
            stream_beat_phase <= 2'd0;
            stream_off   <= 32'd0;
            stream_total <= 64'd0;
        end else begin
            // ---- 3.1 信用更新（生产 +8 / 消费 -seg_len）----
            // 同一个时钟周期既可能有新 beat 入 FIFO，也可能有 DataMover
            // 命令被接受。两次非阻塞赋值会让后者覆盖前者，长期会少记 8B/命令，
            // 最终出现 FIFO 有数据但信用为零的停滞。S_CMD 的 cmd_fire 分支
            // 必须合并这两个变化。
            if (i_beat_en)
                avail_bytes <= avail_bytes + AXI_BYTES[AVAIL_W-1:0];

            // 命令/状态可同拍出现，计数必须保持净变化。状态在没有已发命令
            // 时忽略，防止第三方 IP 复位边沿的孤立状态字导致下溢。
            case ({cmd_fire, sts_fire})
                2'b10: wr_inflight <= wr_inflight + 8'd1;
                2'b01: if (wr_inflight != 8'd0)
                           wr_inflight <= wr_inflight - 8'd1;
                default: ;
            endcase

            case (o_state)
                // ----------------------------------------------------------
                S_IDLE: begin
                    m_cmd_tvalid <= 1'b0;
                    // 冻结尾量在周期终点已经全部进入 FIFO，可直接按寄存的剩余
                    // 字节数规划命令；避免把 64-bit 累计写计数的减法串到发命令
                    // 使能路径。正常采集仍攒够一条命令后才发，防止 DataMover 饿死。
                    if (freeze_commit_wait && (commit_seg_len != 0)) begin
                        cmd_addr_r    <= cmd_addr;
                        seg_len_r     <= commit_seg_len;
                        ring_remain_r <= ring_remain;
                        m_cmd_tvalid  <= 1'b1;
                        o_state       <= S_CMD;
                    end else if (!freeze_commit_wait && i_acq_en &&
                                 (normal_seg_len != 0) &&
                                 (avail_bytes >= normal_seg_len[AVAIL_W-1:0])) begin
                        cmd_addr_r    <= cmd_addr;
                        seg_len_r     <= normal_seg_len;
                        ring_remain_r <= ring_remain;
                        m_cmd_tvalid  <= 1'b1;
                        o_state       <= S_CMD;
                    end
                end

                // ----------------------------------------------------------
                S_CMD: begin
                    m_cmd_tvalid <= 1'b1;
                    if (cmd_fire) begin
                        m_cmd_tvalid <= 1'b0;
                        o_last_btt   <= seg_len_r;
                        o_cmd_cnt    <= o_cmd_cnt + 32'd1;
                        o_wr_bytes   <= o_wr_bytes + {32'd0, seg_len_r};
                        avail_bytes  <= avail_bytes
                                      + (i_beat_en ? AXI_BYTES[AVAIL_W-1:0] : {AVAIL_W{1'b0}})
                                      - seg_len_r[AVAIL_W-1:0];

                        // ---- 指针推进与回绕 ----
                        if (seg_len_r >= ring_remain_r) begin
                            wr_off      <= 32'd0;                   // 回绕到环首
                            o_wrap_cnt  <= o_wrap_cnt + 32'd1;
                        end else begin
                            wr_off      <= wr_off + seg_len_r;
                        end
                        o_state <= S_IDLE;
                    end
                end

                default: o_state <= S_IDLE;
            endcase

            o_wr_off  <= wr_off;
            o_wr_addr <= RING_BASE + wr_off;
            o_blk_idx <= wr_off / BLK_BYTES;
            o_hiwm    <= (avail_bytes >= OVF_THRESH[AVAIL_W-1:0]);

            if (i_beat_en) begin
                if (stream_beat_phase == 2'd2) begin
                    stream_beat_phase <= 2'd0;
                    stream_off   <= stream_off_at_edge;
                    stream_total <= stream_total + BLK_BYTES;
                end else begin
                    stream_beat_phase <= stream_beat_phase + 2'd1;
                end
            end
        end
    end

    assign o_avail_bytes = avail_bytes;
    assign o_write_idle  = (wr_inflight == 8'd0) && !m_cmd_tvalid;

    // =========================================================================
    // 4) 冻结快照：周期边界锁存起止地址（契约 §3.5）
    // =========================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            freeze_arm     <= 1'b0;
            freeze_wait    <= 1'b0;
            freeze_commit_wait <= 1'b0;
            f_start_off    <= 32'd0;
            f_end_off      <= 32'd0;
            f_start_total  <= 64'd0;
            commit_bytes_left <= 32'd0;
            o_freeze_done  <= 1'b0;
            o_freeze_base  <= 32'd0;
            o_freeze_len   <= 32'd0;
            o_snap_overrun <= 1'b0;
        end else begin
            // 触发 / 恢复均为脉冲
            if (i_freeze_trig) begin
                freeze_arm     <= 1'b1;
                freeze_wait    <= 1'b0;
                freeze_commit_wait <= 1'b0;
                commit_bytes_left <= 32'd0;
                o_freeze_done  <= 1'b0;
                o_snap_overrun <= 1'b0;
            end
            if (i_freeze_resume) begin
                freeze_arm     <= 1'b0;
                freeze_wait    <= 1'b0;
                freeze_commit_wait <= 1'b0;
                commit_bytes_left <= 32'd0;
                o_freeze_done  <= 1'b0;
                o_snap_overrun <= 1'b0;
            end

            // 第 1 个周期边界：锁存起始地址（此时起就是一个完整周期的开端）
            if (freeze_arm && i_cycle_start) begin
                f_start_off   <= stream_off_at_edge;
                f_start_total <= stream_total_at_edge;
                freeze_arm    <= 1'b0;
                freeze_wait   <= 1'b1;
            end
            // 第 2 个周期边界：锁存逻辑结束块，随后把不足一个常规突发的
            // 尾部用 24B 整数倍短命令冲入 DDR。
            else if (freeze_wait && i_cycle_start) begin
                f_end_off     <= stream_off_at_edge;
                commit_bytes_left <= freeze_tail_at_edge[31:0];
                freeze_wait   <= 1'b0;
                freeze_commit_wait <= 1'b1;
                o_freeze_base <= RING_BASE + f_start_off;
                o_freeze_len  <= stream_total_at_edge - f_start_total;
            end else if (freeze_commit_wait && cmd_fire) begin
                commit_bytes_left <= (commit_bytes_left > seg_len_r)
                                   ? (commit_bytes_left - seg_len_r) : 32'd0;
            end

            // done 表示周期末尾已经真实写入 DDR，而不仅是描述符已锁存。
            if (freeze_commit_wait && !i_freeze_resume && !i_freeze_trig &&
                (commit_bytes_left == 0) && o_write_idle) begin
                freeze_commit_wait <= 1'b0;
                o_freeze_done <= 1'b1;
            end

            // 覆盖保护：冻结完成后，写指针若再次进入 [start,end) 区间则告警
            if (o_freeze_done) begin
                if (wr_off >= f_start_off && wr_off < f_end_off)
                    o_snap_overrun <= 1'b1;
                else if (f_end_off < f_start_off &&                 // 跨回绕的冻结区
                         (wr_off >= f_start_off || wr_off < f_end_off))
                    o_snap_overrun <= 1'b1;
            end
        end
    end

    // =========================================================================
    // 5) 状态字 / 错误汇总
    // =========================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            o_sts_tdata <= 8'h00;
            o_err       <= 1'b0;
            o_ovf       <= 1'b0;
        end else begin
            if (s_sts_tvalid)
                o_sts_tdata <= s_sts_tdata;
            // 状态字约定：0x80 = 正常完成；非 0x80 视为异常。错误同样
            // 粘滞到软件确认，避免轮询漏掉单拍 DataMover 状态。
            if (i_freeze_resume || !i_acq_en)
                o_err <= 1'b0;
            else if (i_s2mm_err || (s_sts_tvalid && (s_sts_tdata != `DM_STS_OK)))
                o_err <= 1'b1;
            // 采集侧丢数：上游(异步FIFO满 / Data FIFO 反压)汇总是异步事件，
            // 必须**粘滞锁存**后再给 PS 轮询，否则窄脉冲会被软件漏读。
            // 清除条件：停止采集(i_acq_en=0) 或 PS 下发 freeze_resume 作为确认。
            if (i_freeze_resume || !i_acq_en)
                o_ovf <= 1'b0;
            else if (i_up_ovf)
                o_ovf <= 1'b1;
        end
    end

endmodule
