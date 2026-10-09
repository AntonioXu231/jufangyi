// Generated from sim/pd_event_truth_physical_65m.csv by the MATLAB stimulus
// generator. One 20 ms schedule repeats continuously. Events closer than the
// 1024-sample template length are shifted minimally to prevent ROM overlap.
module pd_pd_event_schedule #(
    parameter integer CHANNEL = 0,
    parameter integer TEMPLATE_LEN = 1024
)(
    input wire clk,
    input wire rst_n,
    input wire sample_ce,
    input wire [20:0] sample_index,
    output reg active,
    output reg [9:0] template_addr,
    output reg signed [11:0] amplitude
);
    function integer event_count;
        input integer channel;
        begin
            case (channel)
                0: event_count = 8;
                1: event_count = 9;
                2: event_count = 7;
                3: event_count = 10;
                default: event_count = 0;
            endcase
        end
    endfunction

    function integer event_sample;
        input integer channel;
        input integer index;
        begin
            event_sample = 0;
            case (channel)
                0: begin
                    case (index)
                        0: event_sample = 142749;
                        1: event_sample = 143773;
                        2: event_sample = 147233;
                        3: event_sample = 148257;
                        4: event_sample = 149281;
                        5: event_sample = 160345;
                        6: event_sample = 169367;
                        7: event_sample = 170889;
                        default: event_sample = 0;
                    endcase
                end
                1: begin
                    case (index)
                        0: event_sample = 467876;
                        1: event_sample = 475082;
                        2: event_sample = 480998;
                        3: event_sample = 490062;
                        4: event_sample = 491752;
                        5: event_sample = 500174;
                        6: event_sample = 501770;
                        7: event_sample = 503333;
                        8: event_sample = 519601;
                        default: event_sample = 0;
                    endcase
                end
                2: begin
                    case (index)
                        0: event_sample = 762681;
                        1: event_sample = 796560;
                        2: event_sample = 801114;
                        3: event_sample = 816952;
                        4: event_sample = 823239;
                        5: event_sample = 826762;
                        6: event_sample = 845280;
                        default: event_sample = 0;
                    endcase
                end
                3: begin
                    case (index)
                        0: event_sample = 1093013;
                        1: event_sample = 1107821;
                        2: event_sample = 1112117;
                        3: event_sample = 1121851;
                        4: event_sample = 1126686;
                        5: event_sample = 1128318;
                        6: event_sample = 1141955;
                        7: event_sample = 1159657;
                        8: event_sample = 1167305;
                        9: event_sample = 1185633;
                        default: event_sample = 0;
                    endcase
                end
                default: event_sample = 0;
            endcase
        end
    endfunction

    function integer event_amplitude;
        input integer channel;
        input integer index;
        begin
            event_amplitude = 0;
            case (channel)
                0: begin
                    case (index)
                        0: event_amplitude = 703;
                        1: event_amplitude = 647;
                        2: event_amplitude = 766;
                        3: event_amplitude = 799;
                        4: event_amplitude = 627;
                        5: event_amplitude = 825;
                        6: event_amplitude = 682;
                        7: event_amplitude = 831;
                        default: event_amplitude = 0;
                    endcase
                end
                1: begin
                    case (index)
                        0: event_amplitude = -478;
                        1: event_amplitude = -439;
                        2: event_amplitude = -582;
                        3: event_amplitude = -485;
                        4: event_amplitude = -619;
                        5: event_amplitude = -479;
                        6: event_amplitude = -615;
                        7: event_amplitude = -607;
                        8: event_amplitude = -520;
                        default: event_amplitude = 0;
                    endcase
                end
                2: begin
                    case (index)
                        0: event_amplitude = 445;
                        1: event_amplitude = 382;
                        2: event_amplitude = -444;
                        3: event_amplitude = 499;
                        4: event_amplitude = -498;
                        5: event_amplitude = -437;
                        6: event_amplitude = -486;
                        default: event_amplitude = 0;
                    endcase
                end
                3: begin
                    case (index)
                        0: event_amplitude = 494;
                        1: event_amplitude = 609;
                        2: event_amplitude = -525;
                        3: event_amplitude = 675;
                        4: event_amplitude = -695;
                        5: event_amplitude = 536;
                        6: event_amplitude = -480;
                        7: event_amplitude = 717;
                        8: event_amplitude = 477;
                        9: event_amplitude = -658;
                        default: event_amplitude = 0;
                    endcase
                end
                default: event_amplitude = 0;
            endcase
        end
    endfunction

    reg pulse_active;
    reg [3:0] event_index;
    reg [9:0] pulse_addr;
    reg signed [11:0] pulse_amplitude;
    wire event_start = !pulse_active &&
        (event_index < event_count(CHANNEL)) &&
        (sample_index == event_sample(CHANNEL, event_index));

    always @* begin
        active = pulse_active || event_start;
        template_addr = pulse_active ? pulse_addr : 10'd0;
        if (pulse_active)
            amplitude = pulse_amplitude;
        else if (event_start)
            amplitude = event_amplitude(CHANNEL, event_index);
        else
            amplitude = 12'sd0;
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pulse_active    <= 1'b0;
            event_index     <= 4'd0;
            pulse_addr      <= 10'd0;
            pulse_amplitude <= 12'sd0;
        end else if (sample_ce) begin
            if (sample_index == 21'd1299999) begin
                pulse_active    <= 1'b0;
                event_index     <= 4'd0;
                pulse_addr      <= 10'd0;
                pulse_amplitude <= 12'sd0;
            end else if (pulse_active) begin
                if (pulse_addr == TEMPLATE_LEN - 1) begin
                    pulse_active <= 1'b0;
                    event_index  <= event_index + 4'd1;
                    pulse_addr   <= 10'd0;
                end else begin
                    pulse_addr <= pulse_addr + 10'd1;
                end
            end else if (event_start) begin
                pulse_active    <= 1'b1;
                pulse_addr      <= 10'd1;
                pulse_amplitude <= event_amplitude(CHANNEL, event_index);
            end
        end
    end
endmodule
