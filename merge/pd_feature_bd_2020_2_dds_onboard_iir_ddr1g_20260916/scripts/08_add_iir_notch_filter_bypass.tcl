# ============================================================================
# Add the verified four-channel IIR band-pass/notch boundary to the current
# onboard-DDS project.  Default configuration remains bit-exact bypass.
# Run in the already-open Vivado 2020.2 project:
#   source {F:/xinya/v5/merge/pd_feature_bd_2020_2_dds_onboard_20260914/scripts/08_add_iir_notch_filter_bypass.tcl}
# ============================================================================
set expected_project "pd_feature_bd_2020_2"
if {[get_property NAME [current_project]] ne $expected_project} {
    error "Open the current pd_feature_bd_2020_2 DDS project before running this script."
}

set project_dir [get_property DIRECTORY [current_project]]
set rtl_dir [file join $project_dir pd_feature_bd_2020_2.srcs sources_1 imports rtl]
foreach src {pd_iir_biquad.v pd_filter_chain.v} {
    set file [file normalize [file join $rtl_dir $src]]
    if {![file exists $file]} { error "Required filter RTL is missing: $file" }
    if {[llength [get_files -quiet $file]] == 0} { add_files -fileset sources_1 -norecurse $file }
}
set filter_tb [file normalize [file join $project_dir pd_feature_bd_2020_2.srcs sim_1 new tb_pd_filter_chain.v]]
if {![file exists $filter_tb]} { error "Required filter testbench is missing: $filter_tb" }
if {[llength [get_files -quiet $filter_tb]] == 0} { add_files -fileset sim_1 -norecurse $filter_tb }
update_compile_order -fileset sources_1
update_compile_order -fileset sim_1

set bd_file [get_files -quiet */pd_feature_bd.bd]
if {[llength $bd_file] != 1} { error "Expected exactly one pd_feature_bd.bd, found: $bd_file" }
open_bd_design $bd_file

proc disconnect_filter_pin {pin_name} {
    set pin [get_bd_pins -quiet $pin_name]
    if {[llength $pin] != 1} { error "Expected one pin '$pin_name', found: $pin" }
    foreach net [get_bd_nets -quiet -of_objects $pin] { disconnect_bd_net $net $pin }
}
proc connect_filter_net {source_pin sink_pin} {
    disconnect_filter_pin $sink_pin
    connect_bd_net [get_bd_pins $source_pin] [get_bd_pins $sink_pin]
}

# The control interconnect currently has M00..M02.  Add M03 exclusively for
# the filter, then give it the same 130 MHz clock and interconnect reset.
set_property -dict [list CONFIG.NUM_MI {4}] [get_bd_cells axi_ic_ctrl]
connect_filter_net clk_wiz_0/clk_out1 axi_ic_ctrl/M03_ACLK
connect_filter_net rst_ps7_0/interconnect_aresetn axi_ic_ctrl/M03_ARESETN

if {[llength [get_bd_cells -quiet pd_filter_0]] == 0} {
    create_bd_cell -type module -reference pd_filter_chain pd_filter_0
}
foreach {key value} {
    CONFIG.NUM_CH 4 CONFIG.ADC_W 12 CONFIG.DW 16 CONFIG.CW 18 CONFIG.FW 16
    CONFIG.N_BP 2 CONFIG.N_NT 6 CONFIG.CLK_HZ 130000000 CONFIG.SAMPLE_HZ 26000000
    CONFIG.C_S_AXI_DATA_WIDTH 32 CONFIG.C_S_AXI_ADDR_WIDTH 16
} {
    catch {set_property $key $value [get_bd_cells pd_filter_0]}
}

# The filter operates after pd_ddr_0's CDC, in the 130 MHz feature domain.
# Only feature ADC data/valid are rerouted; sync and all DMA connections stay.
connect_filter_net clk_wiz_0/clk_out1            pd_filter_0/clk
connect_filter_net rst_ps7_0/peripheral_aresetn  pd_filter_0/rst_n
connect_filter_net pd_ddr_0/o_feat_data          pd_filter_0/adc_data
connect_filter_net pd_ddr_0/o_feat_dv            pd_filter_0/adc_dv
connect_filter_net pd_filter_0/filt_data         pd_feature_0/adc_data
connect_filter_net pd_filter_0/filt_dv           pd_feature_0/adc_dv

set filter_axi [get_bd_intf_pins -quiet pd_filter_0/S_AXI]
if {[llength $filter_axi] != 1} { error "pd_filter_0/S_AXI was not inferred; check AXI interface attributes in pd_filter_chain.v" }
foreach net [get_bd_intf_nets -quiet -of_objects $filter_axi] { disconnect_bd_intf_net $net $filter_axi }
connect_bd_intf_net [get_bd_intf_pins axi_ic_ctrl/M03_AXI] $filter_axi

# Preserve the established feature and DDR map; filter occupies the next 64K.
set ps_data [get_bd_addr_spaces -quiet processing_system7_0/Data]
if {[llength $ps_data] != 1} { error "Cannot resolve processing_system7_0/Data address space." }
set filter_seg [get_bd_addr_segs -quiet -of_objects $filter_axi]
if {[llength $filter_seg] != 1} { error "Expected one pd_filter_0 register segment, found: $filter_seg" }
assign_bd_address -target_address_space $ps_data -offset 0x40020000 -range 64K $filter_seg

validate_bd_design
save_bd_design
generate_target all $bd_file
export_ip_user_files -of_objects $bd_file -no_script -sync -force -quiet
update_compile_order -fileset sources_1

puts ""
puts "IIR_NOTCH_FILTER_BYPASS_INSERT_PASS"
puts "  Data: pd_ddr_0/o_feat_* -> pd_filter_0 -> pd_feature_0/adc_*"
puts "  AXI4-Lite: axi_ic_ctrl/M03_AXI -> pd_filter_0/S_AXI @ 0x40020000"
puts "  Default: global bypass=1, channel bypass mask=0xF (existing DDS behavior preserved)."
puts "  Next: run Synthesis and Implementation, then the existing PS DMA regression."
