// =============================================================================
// pd_axis_fifo.v  --  轻量同步 FIFO (AXI-Stream 兼容, 单 RAMB36 实现)
// 用于平滑峰值事件的突发, 并吸收下游(AXI DMA / 仲裁器)的瞬时背压。
// -----------------------------------------------------------------------------
// 【重要 · 勿改回】Block RAM 推断与同步读 FIFO 的六条硬性规则
//   违反任意一条, Vivado 都会*静默*降级为 LUTRAM 甚至把存储体打散成寄存器
//   ——只有 INFO 级提示, 没有 warning, 极易漏掉。
//
//   1) 存储体的写过程块**敏感表里不能出现 rst_n**。
//      否则报 "RAM is sensitive to asynchronous reset signal. this RTL style
//      is not supported." 然后 "RAM dissolved into registers"。
//      指针/计数器的复位请留在另一个独立的 always 块里。
//
//   2) 读必须是**同步读**: 地址打一拍, 数据在下一拍才有效。
//      写成 `assign m_tdata = mem[rp]` 这类组合读, 会综合出一个 DEPTH:1 的
//      巨型多选器(实测单实例 2210 个 MUXF7 / 1105 个 MUXF8), 且无法映射 BRAM。
//
//   3) 读必须**无条件**执行, 不能用 `if (fetch) dout <= mem[rp]` 去门控。
//      门控读会让 Vivado 判定为"非标准模板"从而退回 LUTRAM
//      (实测: 门控读 -> 352 LUT 的 LUTRAM; 无条件读 -> 1 个 RAMB36)。
//      正确做法: 读始终进行, 只让**读地址**和 valid 受条件控制。
//
//   4) 输出保持: 无条件读意味着"输出恒等于 mem[读地址]"。因此读地址必须
//      在 num==0 期间**回指最后一条有效条目**(raddr-1), 否则无条件读会把
//      未写入的存储单元内容(垃圾)冲掉输出寄存器, 造成"valid=1 但数据错"。
//      这是本模块踩过的坑: 首版无条件读 + raddr 停在"下一条待读", 导致
//      ch1~ch3 的事件在等待仲裁器轮询期间被垃圾覆盖(实测 90/120 个 beat 为 X)。
//
//   5) 一个物理 BRAM 端口只有一组地址线。本模块读写地址不同, 因此走
//      simple-dual-port(一写一读), 这是 BRAM 最容易识别的结构。
//
// 实测 (xc7z020clg484-2, DATA_W=64 / DEPTH=256) 单实例:
//   原始(组合读+异步复位): 4852 LUT + 16764 FF + 2210 MUXF7 + 1105 MUXF8 + 0 BRAM
//   最终(同步读+地址回指):   36 LUT +    27 FF +    0 MUXF7 +    0 MUXF8 + 1 RAMB36
//
// 时序契约: 端口与旧版一致, 仅"空 FIFO 写入后首字输出"比组合读晚 1 拍;
//          连续流传输时 dout_valid 保持连续, 吞吐率不变。
//          下游 pd_axis_arb 为纯组合握手(valid/ready 与数据无关), 无需同步修改。
// 注: count 的语义为 "已写入且尚未被取走的条目数", 上限 DEPTH。
// =============================================================================
`timescale 1ns / 1ps

module pd_axis_fifo #(
    parameter integer DATA_W  = 64,
    parameter integer ADDR_W  = 8,        // 深度 = 2^ADDR_W
    parameter integer FWFT    = 0         // 保留参数, 未使用
)(
    input  wire                 clk,
    input  wire                 rst_n,

    // 从端 (写入)
    input  wire [DATA_W-1:0]    s_tdata,
    input  wire                 s_tvalid,
    input  wire                 s_tlast,
    output wire                 s_tready,

    // 主端 (读出)
    output wire [DATA_W-1:0]    m_tdata,
    output wire                 m_tvalid,
    output wire                 m_tlast,
    input  wire                 m_tready,

    output wire [ADDR_W:0]      count,
    output wire                 overflow     // 写入时 FIFO 满 -> 丢数指示
);

    localparam DEPTH = 1 << ADDR_W;

    // tlast 与数据打包, 只占用一块存储体, 避免第二个数组再拉一份 BRAM
    localparam MEM_W = DATA_W + 1;

    (* ram_style = "block" *) reg [MEM_W-1:0] mem [0:DEPTH-1];

    reg [ADDR_W-1:0] wp;      // 写指针
    reg [ADDR_W-1:0] raddr;   // 读地址: 下一条待装载条目的地址
    reg [ADDR_W:0]   num;     // 已写入、尚未装载到输出寄存器的条目数

    reg [MEM_W-1:0]  dout_raw;   // 同步读输出(含打包的 tlast)
    reg              dout_valid;
    reg              ovf_r;

    wire wr_en = s_tvalid && s_tready;                    // 写入
    wire rd_en = m_tready && dout_valid;                  // 本拍 dout 被取走
    wire fetch = (num != 0) && (!dout_valid || m_tready); // 装载下一条
    wire [ADDR_W:0] used = num + dout_valid;              // 实际占用条目数

    assign s_tready = (used != DEPTH);
    assign m_tdata  = dout_raw[DATA_W-1:0];
    assign m_tlast  = dout_raw[DATA_W];
    assign m_tvalid = dout_valid;
    assign count    = used;
    assign overflow = ovf_r;

    // 读地址选择: 有待装载条目 -> raddr(下一条);
    //             无待装载条目 -> raddr-1(回看最后一条已装载条目), 让无条件
    //             读始终保持输出寄存器内容, 防止被未写入单元的垃圾冲掉。
    wire [ADDR_W-1:0] rd_addr = (num != 0) ? raddr : (raddr - 1'b1);

    // ---- 存储体: 无复位, 条件写 + 无条件同步读 (BRAM 推断的关键) ----------
    always @(posedge clk) begin
        if (wr_en) mem[wp] <= {s_tlast, s_tdata};
        dout_raw <= mem[rd_addr];
    end

    // ---- 指针 / 计数 / 有效标志 / 溢出 -------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wp         <= {ADDR_W{1'b0}};
            raddr      <= {ADDR_W{1'b0}};
            num        <= {(ADDR_W+1){1'b0}};
            dout_valid <= 1'b0;
            ovf_r      <= 1'b0;
        end else begin
            if (s_tvalid && !s_tready) ovf_r <= 1'b1;   // 满时写入 -> 丢数

            if (wr_en) wp <= wp + 1'b1;
            if (fetch) raddr <= raddr + 1'b1;

            if (wr_en && !fetch)
                num <= num + 1'b1;
            else if (!wr_en && fetch)
                num <= num - 1'b1;

            if (fetch)
                dout_valid <= 1'b1;
            else if (rd_en)
                dout_valid <= 1'b0;
        end
    end

endmodule
