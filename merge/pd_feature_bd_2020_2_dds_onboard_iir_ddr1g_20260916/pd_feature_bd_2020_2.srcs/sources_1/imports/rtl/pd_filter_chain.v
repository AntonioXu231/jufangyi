`timescale 1ns / 1ps
// Four-channel IIR band-pass/notch chain.  The AXI4-Lite slave accepts AW and
// W independently; reset defaults to a bit-exact external bypass.
module pd_filter_chain #(
    parameter integer NUM_CH=4, ADC_W=12, DW=16, CW=18, FW=16,
    parameter integer N_BP=2, N_NT=6, CLK_HZ=130000000, SAMPLE_HZ=26000000,
    parameter integer C_S_AXI_DATA_WIDTH=32, C_S_AXI_ADDR_WIDTH=16
)(
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME CLK, FREQ_HZ 130000000, ASSOCIATED_BUSIF S_AXI" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *) input wire clk,
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME RST, POLARITY ACTIVE_LOW" *)
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst_n RST" *) input wire rst_n,
    input wire [NUM_CH*ADC_W-1:0] adc_data,
    input wire [NUM_CH-1:0] adc_dv, output wire [NUM_CH*ADC_W-1:0] filt_data,
    output wire [NUM_CH-1:0] filt_dv,
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME S_AXI, PROTOCOL AXI4LITE, ADDR_WIDTH 16, DATA_WIDTH 32, FREQ_HZ 130000000" *)
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWADDR" *) input wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_AWADDR,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWVALID" *) input wire S_AXI_AWVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI AWREADY" *) output wire S_AXI_AWREADY,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WDATA" *) input wire [31:0] S_AXI_WDATA,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WSTRB" *) input wire [3:0] S_AXI_WSTRB,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WVALID" *) input wire S_AXI_WVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI WREADY" *) output wire S_AXI_WREADY,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BRESP" *) output wire [1:0] S_AXI_BRESP,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BVALID" *) output wire S_AXI_BVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI BREADY" *) input wire S_AXI_BREADY,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARADDR" *) input wire [C_S_AXI_ADDR_WIDTH-1:0] S_AXI_ARADDR,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARVALID" *) input wire S_AXI_ARVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI ARREADY" *) output wire S_AXI_ARREADY,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RDATA" *) output wire [31:0] S_AXI_RDATA,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RRESP" *) output wire [1:0] S_AXI_RRESP,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RVALID" *) output wire S_AXI_RVALID,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S_AXI RREADY" *) input wire S_AXI_RREADY
);
    localparam integer N_STAGE=N_BP+N_NT, N_CELL=NUM_CH*N_STAGE, N_COEF=N_CELL*5;
    localparam signed [CW-1:0] ONE_C=(1 <<< FW), ZERO_C={CW{1'b0}};
    reg aw_hold, w_hold, bvalid, rvalid, cfg_valid;
    reg [C_S_AXI_ADDR_WIDTH-1:0] awaddr_hold;
    reg [31:0] wdata_hold, rdata, cfg_data;
    reg [3:0] wstrb_hold, cfg_strb;
    reg [11:0] cfg_addr;
    wire aw_hs=S_AXI_AWVALID && S_AXI_AWREADY;
    wire w_hs=S_AXI_WVALID && S_AXI_WREADY;
    wire write_fire=!bvalid && (aw_hold || aw_hs) && (w_hold || w_hs);
    wire [C_S_AXI_ADDR_WIDTH-1:0] waddr=aw_hold ? awaddr_hold : S_AXI_AWADDR;
    wire [31:0] wdata=w_hold ? wdata_hold : S_AXI_WDATA;
    wire [3:0] wstrb=w_hold ? wstrb_hold : S_AXI_WSTRB;
    // AXI data are captured into cfg_* first.  This register boundary keeps
    // the crossbar's wide address/data fanout out of the 160-entry coefficient
    // write decoder, which is essential at the 130 MHz control clock.
    assign S_AXI_AWREADY=!aw_hold && !bvalid && !cfg_valid;
    assign S_AXI_WREADY =!w_hold  && !bvalid && !cfg_valid;
    assign S_AXI_BVALID=bvalid; assign S_AXI_BRESP=2'b00;
    assign S_AXI_ARREADY=!rvalid; assign S_AXI_RVALID=rvalid;
    assign S_AXI_RRESP=2'b00; assign S_AXI_RDATA=rdata;

    function [31:0] merge32;
        input [31:0] oldv;
        input [31:0] newv;
        input [3:0] strb;
        begin merge32=oldv; if(strb[0])merge32[7:0]=newv[7:0]; if(strb[1])merge32[15:8]=newv[15:8]; if(strb[2])merge32[23:16]=newv[23:16]; if(strb[3])merge32[31:24]=newv[31:24]; end
    endfunction
    function [CW-1:0] mergecoef;
        input [CW-1:0] oldv; input [31:0] newv; input [3:0] strb; reg [31:0] t;
        begin t={{(32-CW){1'b0}},oldv}; t=merge32(t,newv,strb); mergecoef=t[CW-1:0]; end
    endfunction
    reg fctrl_bypass, coef_dirty, apply_p, clear_p;
    reg [31:0] bypass_mask;
    reg sh_en[0:N_CELL-1], ac_en[0:N_CELL-1];
    reg [CW-1:0] sh_coef[0:N_COEF-1], ac_coef[0:N_COEF-1];
    wire [11:0] wa=cfg_addr; wire [11:0] ca=wa-12'h010;
    wire [1:0] c_ch=ca[10:9]; wire [2:0] c_stage=ca[7:5], c_reg=ca[4:2];
    wire c_valid=(c_ch<NUM_CH)&&(c_stage<N_STAGE)&&(c_reg<=5);
    wire [4:0] c_idx=c_ch*N_STAGE+c_stage; wire [7:0] c_base=(c_ch*N_STAGE+c_stage)*5;
    integer i;
    always @(posedge clk or negedge rst_n) begin
        if(!rst_n) begin aw_hold<=0; w_hold<=0; bvalid<=0; cfg_valid<=0; awaddr_hold<={C_S_AXI_ADDR_WIDTH{1'b0}}; wdata_hold<=32'd0; wstrb_hold<=4'd0; cfg_addr<=12'd0; cfg_data<=32'd0; cfg_strb<=4'd0; end
        else begin
            if(cfg_valid) begin cfg_valid<=0; bvalid<=1; end
            if(write_fire) begin aw_hold<=0; w_hold<=0; cfg_valid<=1; cfg_addr<=waddr[11:0]; cfg_data<=wdata; cfg_strb<=wstrb; end
            else begin if(aw_hs) begin aw_hold<=1; awaddr_hold<=S_AXI_AWADDR; end if(w_hs) begin w_hold<=1; wdata_hold<=S_AXI_WDATA; wstrb_hold<=S_AXI_WSTRB; end end
            if(bvalid && S_AXI_BREADY && !cfg_valid) bvalid<=0;
        end
    end
    always @(posedge clk or negedge rst_n) begin
        if(!rst_n) begin fctrl_bypass<=1; bypass_mask<={NUM_CH{1'b1}}; coef_dirty<=0; apply_p<=0; clear_p<=0; for(i=0;i<N_CELL;i=i+1) begin sh_en[i]<=0; ac_en[i]<=0; end for(i=0;i<N_COEF;i=i+1) begin sh_coef[i]<={CW{1'b0}}; ac_coef[i]<={CW{1'b0}}; end end
        else begin
            apply_p<=0; clear_p<=0;
            if(cfg_valid && wa<12'h010) case(wa[3:2])
                0: if(cfg_strb[0]) begin fctrl_bypass<=cfg_data[0]; if(cfg_data[1]) begin apply_p<=1; coef_dirty<=0; end if(cfg_data[2]) clear_p<=1; end
                2: bypass_mask<=merge32(bypass_mask,cfg_data,cfg_strb);
                default: begin end
            endcase
            if(cfg_valid && wa>=12'h010 && c_valid) begin if(c_reg==0) begin if(cfg_strb[0]) sh_en[c_idx]<=cfg_data[0]; end else sh_coef[c_base+c_reg-1]<=mergecoef(sh_coef[c_base+c_reg-1],cfg_data,cfg_strb); coef_dirty<=1; end
            if(apply_p) begin for(i=0;i<N_CELL;i=i+1) ac_en[i]<=sh_en[i]; for(i=0;i<N_COEF;i=i+1) ac_coef[i]<=sh_coef[i]; end
        end
    end
    wire cfg_clr=apply_p|clear_p;
    reg [31:0] read_mux; wire [11:0] ra=S_AXI_ARADDR[11:0], rca=ra-12'h010;
    wire [1:0] rc_ch=rca[10:9]; wire [2:0] rc_stage=rca[7:5], rc_reg=rca[4:2];
    wire rc_valid=(rc_ch<NUM_CH)&&(rc_stage<N_STAGE)&&(rc_reg<=5); wire [4:0] rc_idx=rc_ch*N_STAGE+rc_stage; wire [7:0] rc_base=(rc_ch*N_STAGE+rc_stage)*5;
    always @* begin read_mux=0; if(ra<12'h010) case(ra[3:2]) 0:read_mux={31'd0,fctrl_bypass}; 1:read_mux={16'd0,8'h04,6'd0,coef_dirty,(apply_p|clear_p)}; 2:read_mux=bypass_mask; 3:read_mux=SAMPLE_HZ; endcase else if(rc_valid) begin if(rc_reg==0) read_mux={31'd0,sh_en[rc_idx]}; else read_mux={{(32-CW){1'b0}},sh_coef[rc_base+rc_reg-1]}; end end
    always @(posedge clk or negedge rst_n) begin if(!rst_n) begin rvalid<=0; rdata<=0; end else if(S_AXI_ARVALID && S_AXI_ARREADY) begin rvalid<=1; rdata<=read_mux; end else if(rvalid && S_AXI_RREADY) rvalid<=0; end
    genvar ch,st;
    generate for(ch=0;ch<NUM_CH;ch=ch+1) begin: GCH
        wire [ADC_W-1:0] raw=adc_data[ch*ADC_W+:ADC_W];
        wire signed [DW-1:0] in_s=$signed({1'b0,raw})-(1<<(ADC_W-1));
        wire signed [DW-1:0] sd[0:N_STAGE]; wire sv[0:N_STAGE]; assign sd[0]=in_s; assign sv[0]=adc_dv[ch];
        for(st=0;st<N_STAGE;st=st+1) begin: GST
            localparam integer CI=ch*N_STAGE+st, CB=CI*5;
            pd_iir_biquad #(.DW(DW),.CW(CW),.FW(FW),.PH(5)) u_iir(.clk(clk),.rst_n(rst_n),.clr(cfg_clr),.dv(sv[st]),.din(sd[st]),.c_b0(ac_en[CI]?ac_coef[CB]:ONE_C),.c_b1(ac_en[CI]?ac_coef[CB+1]:ZERO_C),.c_b2(ac_en[CI]?ac_coef[CB+2]:ZERO_C),.c_a1(ac_en[CI]?ac_coef[CB+3]:ZERO_C),.c_a2(ac_en[CI]?ac_coef[CB+4]:ZERO_C),.dout(sd[st+1]),.dout_dv(sv[st+1]));
        end
        wire signed [DW-1:0] y=sd[N_STAGE]; wire signed [ADC_W-1:0] yc=(y>2047)?2047:(y< -2048)?-2048:y[ADC_W-1:0]; wire bypass=fctrl_bypass|bypass_mask[ch];
        assign filt_data[ch*ADC_W+:ADC_W]=bypass?raw:(yc+(1<<(ADC_W-1)));
        assign filt_dv[ch]=bypass?adc_dv[ch]:sv[N_STAGE];
    end endgenerate
endmodule
