`timescale 1ns / 1ps
// Standalone regression for the added IIR filter boundary.
// It proves reset/default bypass and AXI4-Lite independent AW/W transactions.
module tb_pd_filter_chain;
    reg clk=0, rst_n=0;
    reg [47:0] adc_data=0; reg [3:0] adc_dv=0;
    wire [47:0] filt_data; wire [3:0] filt_dv;
    reg [15:0] awaddr=0; reg awvalid=0; wire awready;
    reg [31:0] wdata=0; reg [3:0] wstrb=0; reg wvalid=0; wire wready;
    wire [1:0] bresp; wire bvalid; reg bready=1;
    reg [15:0] araddr=0; reg arvalid=0; wire arready;
    wire [31:0] rdata; wire [1:0] rresp; wire rvalid; reg rready=1;
    integer errors=0;

    always #3.846 clk=~clk; // 130 MHz

    pd_filter_chain dut (
        .clk(clk),.rst_n(rst_n),.adc_data(adc_data),.adc_dv(adc_dv),
        .filt_data(filt_data),.filt_dv(filt_dv),
        .S_AXI_AWADDR(awaddr),.S_AXI_AWVALID(awvalid),.S_AXI_AWREADY(awready),
        .S_AXI_WDATA(wdata),.S_AXI_WSTRB(wstrb),.S_AXI_WVALID(wvalid),.S_AXI_WREADY(wready),
        .S_AXI_BRESP(bresp),.S_AXI_BVALID(bvalid),.S_AXI_BREADY(bready),
        .S_AXI_ARADDR(araddr),.S_AXI_ARVALID(arvalid),.S_AXI_ARREADY(arready),
        .S_AXI_RDATA(rdata),.S_AXI_RRESP(rresp),.S_AXI_RVALID(rvalid),.S_AXI_RREADY(rready)
    );

    task axil_write_aw_then_w;
        input [15:0] addr; input [31:0] data; input [3:0] strb;
        begin
            @(negedge clk); awaddr=addr; awvalid=1;
            while(!awready) @(negedge clk);
            @(negedge clk); awvalid=0;
            @(negedge clk); wdata=data; wstrb=strb; wvalid=1;
            while(!wready) @(negedge clk);
            @(negedge clk); wvalid=0;
            while(!bvalid) @(negedge clk);
            @(negedge clk);
        end
    endtask

    task axil_write_w_then_aw;
        input [15:0] addr; input [31:0] data; input [3:0] strb;
        begin
            @(negedge clk); wdata=data; wstrb=strb; wvalid=1;
            while(!wready) @(negedge clk);
            @(negedge clk); wvalid=0;
            @(negedge clk); awaddr=addr; awvalid=1;
            while(!awready) @(negedge clk);
            @(negedge clk); awvalid=0;
            while(!bvalid) @(negedge clk);
            @(negedge clk);
        end
    endtask

    task axil_read;
        input [15:0] addr; output [31:0] data;
        begin
            @(negedge clk); araddr=addr; arvalid=1;
            while(!arready) @(negedge clk);
            @(negedge clk); arvalid=0;
            while(!rvalid) @(negedge clk);
            data=rdata;
            @(negedge clk);
        end
    endtask

    task check_bypass_sample;
        input [47:0] sample; input [3:0] valid;
        begin
            @(negedge clk); adc_data=sample; adc_dv=valid;
            @(posedge clk); #1;
            if(filt_data !== sample || filt_dv !== valid) begin
                $display("FAIL bypass sample data=%h/%h dv=%h/%h",filt_data,sample,filt_dv,valid);
                errors=errors+1;
            end
            @(negedge clk); adc_dv=0;
        end
    endtask

    reg [31:0] rd;
    initial begin
        repeat(4) @(negedge clk); rst_n=1;
        check_bypass_sample(48'h123_456_789_abc,4'b1111);
        check_bypass_sample(48'hfff_800_001_555,4'b0101);

        axil_read(16'h0004,rd);
        if(rd !== 32'h0000_0400) begin $display("FAIL FSTATUS reset=%h",rd); errors=errors+1; end
        axil_read(16'h0008,rd);
        if(rd !== 32'h0000_000f) begin $display("FAIL bypass mask reset=%h",rd); errors=errors+1; end

        axil_write_aw_then_w(16'h0008,32'h0000_0005,4'b0001);
        axil_read(16'h0008,rd);
        if(rd !== 32'h0000_0005) begin $display("FAIL AW-first bypass mask=%h",rd); errors=errors+1; end
        axil_write_w_then_aw(16'h0014,32'h0001_2345,4'b1111); // ch0/stage0/b0
        axil_read(16'h0014,rd);
        if(rd !== 32'h0001_2345) begin $display("FAIL W-first coefficient=%h",rd); errors=errors+1; end
        axil_write_aw_then_w(16'h0014,32'h0000_ab00,4'b0010);
        axil_read(16'h0014,rd);
        if(rd !== 32'h0001_ab45) begin $display("FAIL WSTRB coefficient=%h",rd); errors=errors+1; end
        axil_read(16'h0004,rd);
        if(rd[1] !== 1'b1) begin $display("FAIL coefficient dirty=%h",rd); errors=errors+1; end

        if(errors==0) $display("TB_PD_FILTER_CHAIN_PASS");
        else $fatal(1,"TB_PD_FILTER_CHAIN_FAIL errors=%0d",errors);
        $finish;
    end
endmodule
