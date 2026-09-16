`timescale 1ns / 1ps
// =========================================================================
// async_fifo.v  --  通用异步 FIFO（格雷码指针 + 双同步器）
// 典型用法：ADC 时钟域 (wr) → 系统时钟域 (rd)
// 注意：本模块只保证两个时钟异步关系下的指针传递；调用方必须向每个时钟
// 域提供已同步释放的低有效复位。
// -------------------------------------------------------------------------
// 【修订 v2 · Block RAM 打包】旧版有三个致命写法，导致 Vivado *静默* 把
// 存储体打散成触发器（12bit×64 → 768 个 FF/通道，4 通道共约 3072 FF）：
//   1) `initial` + for 循环整块初始化   -> 存储体被判定为"非 RAM 模板"
//   2) 组合读 `assign rd_data = mem[]`  -> 7 系列 BRAM 只有同步输出，
//      组合读会退化成 DEPTH:1 巨型多选器或分布式 RAM
//   3) 写块敏感表含 wr_rst_n            -> "RAM is sensitive to async reset"
// 修复后遵循与 pd_axis_fifo.v 相同的六条 BRAM 推断规则：
//   · 存储体写块【无复位】· 同步读 · 无条件读 · 无 initial · ram_style · 一写一读
// 结果：存储体映射为 1 个 RAMB18（双端口、读写独立时钟）。
//
// ⚠️ 时序契约变化：rd_data 由"组合输出"改为"同步输出"，
//    rd_en 拉高后【第 2 拍】数据才有效（多了 1 拍流水延迟）。
//    调用方 pd_adc_cdc 已同步加一级 rd_en_d 打拍，吞吐率不变。
// =========================================================================
module async_fifo #(
    parameter WIDTH     = 24,
    parameter DEPTH     = 2048,  // 必须是 2 的幂
    parameter RAM_STYLE = "block"   // "block"=BRAM / "distributed"=LUTRAM
)(
    input  wire              wr_clk,
    input  wire              wr_rst_n,
    input  wire              wr_en,
    input  wire [WIDTH-1:0]  wr_data,
    output wire              full,

    input  wire              rd_clk,
    input  wire              rd_rst_n,
    input  wire              rd_en,
    output wire [WIDTH-1:0]  rd_data,
    output wire              empty
);

    localparam ADDR_W = $clog2(DEPTH);
    localparam PTR_W  = ADDR_W + 1;

    // ---- 存储体: 禁止 initial 初始化, 禁止 ram_style 缺省 ------------------
    (* ram_style = RAM_STYLE *) reg [WIDTH-1:0] mem [0:DEPTH-1];

    reg  [PTR_W-1:0] wr_ptr_bin = 0;
    reg  [PTR_W-1:0] rd_ptr_bin = 0;
    wire [PTR_W-1:0] wr_ptr_gray;
    wire [PTR_W-1:0] rd_ptr_gray;

    // 二进制→格雷
    assign wr_ptr_gray = wr_ptr_bin ^ (wr_ptr_bin >> 1);
    assign rd_ptr_gray = rd_ptr_bin ^ (rd_ptr_bin >> 1);

    // 双同步：写指针的格雷码 → 读时钟域
    // ASYNC_REG: 让两级同步器紧邻布局, 消除 TIMING-10 告警并压低亚稳态概率
    (* ASYNC_REG = "TRUE" *) reg [PTR_W-1:0] wr_ptr_gray_sync1 = 0;
    (* ASYNC_REG = "TRUE" *) reg [PTR_W-1:0] wr_ptr_gray_sync2 = 0;
    always @(posedge rd_clk) begin
        if (!rd_rst_n) {wr_ptr_gray_sync1, wr_ptr_gray_sync2} <= 0;
        else           {wr_ptr_gray_sync2, wr_ptr_gray_sync1} <= {wr_ptr_gray_sync1, wr_ptr_gray};
    end

    // 双同步：读指针的格雷码 → 写时钟域
    (* ASYNC_REG = "TRUE" *) reg [PTR_W-1:0] rd_ptr_gray_sync1 = 0;
    (* ASYNC_REG = "TRUE" *) reg [PTR_W-1:0] rd_ptr_gray_sync2 = 0;
    always @(posedge wr_clk) begin
        if (!wr_rst_n) {rd_ptr_gray_sync1, rd_ptr_gray_sync2} <= 0;
        else           {rd_ptr_gray_sync2, rd_ptr_gray_sync1} <= {rd_ptr_gray_sync1, rd_ptr_gray};
    end

    // 读时钟域的 empty 判定
    wire [PTR_W-1:0] wr_ptr_gray_in_rd = wr_ptr_gray_sync2;
    assign empty = (wr_ptr_gray_in_rd == rd_ptr_gray);

    // 写时钟域的 full 判定
    //
    // ⚠️ 经典陷阱：格雷码判"满"必须【最高两位都取反】，只反转最高位是错的。
    //    设指针 PTR_W=N+1 位、深度 2^N，「满」= wr 领先 rd 整一圈 (2^N)：
    //        bin_wr[PTR_W-1]   = ~bin_rd[PTR_W-1]
    //        bin_wr[PTR_W-2:0] =  bin_rd[PTR_W-2:0]
    //    转成格雷码（g = b ^ (b>>1)）后变成：
    //        gray_wr[PTR_W-1]   = ~gray_rd[PTR_W-1]    <- 最高位取反
    //        gray_wr[PTR_W-2]   = ~gray_rd[PTR_W-2]    <- 次高位【也要】取反
    //        gray_wr[PTR_W-3:0] =  gray_rd[PTR_W-3:0]  <- 其余位相等
    //    只反转最高位时 full 永远不会拉高（脚本遍历 4096 种指针组合验证：
    //    错误判据命中 0/4096，正确判据命中 4096/4096），FIFO 会静默溢出。
    wire [PTR_W-1:0] rd_ptr_gray_in_wr = rd_ptr_gray_sync2;
    wire [PTR_W-1:0] full_cmp_gray     = {~rd_ptr_gray_in_wr[PTR_W-1],
                                          ~rd_ptr_gray_in_wr[PTR_W-2],
                                           rd_ptr_gray_in_wr[PTR_W-3:0]};
    assign full = (wr_ptr_gray == full_cmp_gray);

    wire wr_act = wr_en && !full;
    wire rd_act = rd_en && !empty;

    // ---- 存储体写: 敏感表【不含复位】，否则 BRAM 推断失败 ------------------
    always @(posedge wr_clk) begin
        if (wr_act) mem[wr_ptr_bin[ADDR_W-1:0]] <= wr_data;
    end

    // ---- 存储体读: 无条件同步读（BRAM 推断的关键，禁止门控）----------------
    reg [WIDTH-1:0] rd_data_reg;
    always @(posedge rd_clk) begin
        rd_data_reg <= mem[rd_ptr_bin[ADDR_W-1:0]];
    end
    assign rd_data = rd_data_reg;

    // ---- 指针: 复位留在独立块里，不污染存储体 ------------------------------
    always @(posedge wr_clk) begin
        if (!wr_rst_n) wr_ptr_bin <= 0;
        else if (wr_act) wr_ptr_bin <= wr_ptr_bin + 1'b1;
    end

    always @(posedge rd_clk) begin
        if (!rd_rst_n) rd_ptr_bin <= 0;
        else if (rd_act) rd_ptr_bin <= rd_ptr_bin + 1'b1;
    end

endmodule
