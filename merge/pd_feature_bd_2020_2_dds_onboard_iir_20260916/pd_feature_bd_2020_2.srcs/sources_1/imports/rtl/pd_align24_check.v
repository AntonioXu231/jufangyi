`timescale 1ns/1ps

// 32-bit 值的 24 字节对齐检查器。
//
// 24 = 8 * 3。先检查低 3 位为零，再检查 value>>3 是否能被 3 整除。
// 因为 16 mod 3 = 1，可把 value>>3 拆成 8 个 4-bit 数，各自求模后用
// 平衡加法树求和。与直接写 `% 24` 相比，不会推导出长串常数除法器。
module pd_align24_check (
    input  wire [31:0] value,
    output wire        aligned
);
    function [1:0] nibble_mod3;
        input [3:0] nibble;
        begin
            case (nibble)
                4'd0, 4'd3, 4'd6, 4'd9, 4'd12, 4'd15: nibble_mod3 = 2'd0;
                4'd1, 4'd4, 4'd7, 4'd10, 4'd13:       nibble_mod3 = 2'd1;
                default:                              nibble_mod3 = 2'd2;
            endcase
        end
    endfunction

    wire [1:0] r0 = nibble_mod3(value[6:3]);
    wire [1:0] r1 = nibble_mod3(value[10:7]);
    wire [1:0] r2 = nibble_mod3(value[14:11]);
    wire [1:0] r3 = nibble_mod3(value[18:15]);
    wire [1:0] r4 = nibble_mod3(value[22:19]);
    wire [1:0] r5 = nibble_mod3(value[26:23]);
    wire [1:0] r6 = nibble_mod3(value[30:27]);
    wire [1:0] r7 = nibble_mod3({3'b000, value[31]});

    wire [2:0] s01 = r0 + r1;
    wire [2:0] s23 = r2 + r3;
    wire [2:0] s45 = r4 + r5;
    wire [2:0] s67 = r6 + r7;
    wire [3:0] s03 = s01 + s23;
    wire [3:0] s47 = s45 + s67;
    wire [4:0] residue_sum = s03 + s47;

    wire div3 = (residue_sum == 5'd0)  || (residue_sum == 5'd3)  ||
                (residue_sum == 5'd6)  || (residue_sum == 5'd9)  ||
                (residue_sum == 5'd12) || (residue_sum == 5'd15);

    assign aligned = (value[2:0] == 3'b000) && div3;
endmodule
