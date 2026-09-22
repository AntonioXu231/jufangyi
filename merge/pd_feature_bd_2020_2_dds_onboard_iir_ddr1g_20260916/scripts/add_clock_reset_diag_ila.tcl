# Add a non-intrusive clock/reset diagnostic ILA to the open pd_feature_bd.
#
# Sampling clock: PS FCLK_CLK0.  This deliberately remains independent of
# clk_wiz_0/clk_out1, so the ILA can report whether the 130 MHz capture clock
# domain is alive or being held in reset.
#
# Probes:
#   probe0 = clk_wiz_0/locked
#   probe1 = rst_ps7_0/peripheral_aresetn
#   probe2 = processing_system7_0/FCLK_RESET0_N
#
# Run from an open Vivado 2020.2 project:
#   source <project-root>/scripts/add_clock_reset_diag_ila.tcl

set bd_name pd_feature_bd
set bd_file [get_files -quiet */${bd_name}.bd]
if {[llength $bd_file] != 1} {
    error "Expected exactly one ${bd_name}.bd in the open project; found: $bd_file"
}
open_bd_design $bd_file
current_bd_design $bd_name

set diag_name ila_clk_reset_diag_0
if {[llength [get_bd_cells -quiet $diag_name]] != 0} {
    error "$diag_name already exists.  No change was made."
}

foreach {name object} [list \
    processing_system7_0 [get_bd_cells -quiet processing_system7_0] \
    clk_wiz_0           [get_bd_cells -quiet clk_wiz_0] \
    rst_ps7_0           [get_bd_cells -quiet rst_ps7_0]] {
    if {[llength $object] != 1} {
        error "Required BD cell '$name' was not found."
    }
}

set diag [create_bd_cell -type ip -vlnv xilinx.com:ip:ila:6.2 $diag_name]
set_property -dict [list \
    CONFIG.C_DATA_DEPTH {1024} \
    CONFIG.C_NUM_OF_PROBES {3} \
    CONFIG.C_PROBE0_WIDTH {1} \
    CONFIG.C_PROBE1_WIDTH {1} \
    CONFIG.C_PROBE2_WIDTH {1} \
] $diag

# The diagnostic ILA must be clocked from the PS FCLK, not clk_out1.
connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] \
               [get_bd_pins ${diag_name}/clk]
connect_bd_net [get_bd_pins clk_wiz_0/locked] \
               [get_bd_pins ${diag_name}/probe0]
connect_bd_net [get_bd_pins rst_ps7_0/peripheral_aresetn] \
               [get_bd_pins ${diag_name}/probe1]
connect_bd_net [get_bd_pins processing_system7_0/FCLK_RESET0_N] \
               [get_bd_pins ${diag_name}/probe2]

validate_bd_design
save_bd_design
generate_target all $bd_file
export_ip_user_files -of_objects $bd_file -no_script -sync -force -quiet

puts "CLOCK_RESET_DIAG_ILA_ADDED"
puts "probe0=clk_wiz_0/locked probe1=rst_ps7_0/peripheral_aresetn probe2=processing_system7_0/FCLK_RESET0_N"
puts "Next: reset_run synth_1; reset_run impl_1; launch_runs impl_1 -to_step write_bitstream -jobs 7"
