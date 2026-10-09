
# adc_clk is clk_wiz_0/clk_out2, generated at 65 MSPS from the PL 50 MHz input.
# Clock Wizard supplies the generated-clock constraint; do not create a clock
# on the nonexistent adc_clk_0 top-level port.
# Core-board PL 50 MHz oscillator, net name PL_GCLK.
set_property PACKAGE_PIN U18 [get_ports pl_gclk_50]
set_property IOSTANDARD LVCMOS33 [get_ports pl_gclk_50]
create_clock -name pl_gclk_50 -period 20.000 [get_ports pl_gclk_50]

# adc_clk (65 MHz) and clk_130 (130 MHz) feed explicit asynchronous-FIFO
# boundaries in the acquisition/feature paths.
# XDC accepts constraint commands only. Do not wrap this in Tcl conditionals:
# `if` is rejected by the XDC parser and causes the asynchronous FIFO crossings
# to be analyzed as unrelated synchronous paths.
set_clock_groups -asynchronous \
-group [get_clocks -of_objects [get_pins -hierarchical *clk_wiz_0*/clk_out2]] \
-group [get_clocks -of_objects [get_pins -hierarchical *clk_wiz_0*/clk_out1]]

# PS FCLK0 and clk_wiz_0/clk_out1 now originate from independent sources:
# PS PLL and the external PL_GCLK 50 MHz oscillator, respectively.  The only
# FCLK0 destination is the diagnostic ILA; its probes sample lock/reset
# levels for observation and create no functional CDC data transfer.
set_clock_groups -asynchronous \
-group [get_clocks clk_fpga_0] \
-group [get_clocks -of_objects [get_pins -hierarchical *clk_wiz_0*/clk_out1]]
