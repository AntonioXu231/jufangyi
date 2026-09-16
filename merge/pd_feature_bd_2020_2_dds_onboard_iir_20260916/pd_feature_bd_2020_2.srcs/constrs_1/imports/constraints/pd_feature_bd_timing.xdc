# clk_130 is generated and constrained by clk_wiz_0 from the PS 150 MHz FCLK.
# adc_clk_0 is the external 26 MSPS sampling clock for all four ADC channels.

create_clock -name adc_clk -period 38.461538 [get_ports adc_clk_0]

# adc_clk_0 and clk_130 are asynchronous at the BD boundary.  Their intended
# crossings are the two independent async FIFOs in pd_ddr_wr_top.
# XDC accepts constraint commands only. Do not wrap this in Tcl conditionals:
# `if` is rejected by the XDC parser and causes the asynchronous FIFO crossings
# to be analyzed as unrelated synchronous paths.
set_clock_groups -asynchronous \
    -group [get_clocks adc_clk] \
    -group [get_clocks -of_objects [get_pins -hierarchical *clk_wiz_0*/clk_out1]]
