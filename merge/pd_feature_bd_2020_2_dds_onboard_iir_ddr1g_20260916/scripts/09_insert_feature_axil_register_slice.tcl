# ============================================================================
# Add an official AXI Register Slice on the feature-control AXI4-Lite path.
# It breaks the remaining post-route timing path in axi_ic_ctrl -> pd_feature_0
# without changing the data/DMA path, register address map, or IIR algorithm.
#
# Run in the already-open Vivado 2020.2 project:
# source {F:/xinya/v5/merge/pd_feature_bd_2020_2_dds_onboard_20260914/scripts/09_insert_feature_axil_register_slice.tcl}
# ============================================================================
set expected_project "pd_feature_bd_2020_2"
if {[get_property NAME [current_project]] ne $expected_project} {
    error "Open the current pd_feature_bd_2020_2 DDS project before running this script."
}

set bd_file [get_files -quiet */pd_feature_bd.bd]
if {[llength $bd_file] != 1} { error "Expected exactly one pd_feature_bd.bd, found: $bd_file" }
open_bd_design $bd_file

set slice axi_rs_feature_ctrl_0
if {[llength [get_bd_cells -quiet $slice]] != 0} {
    error "$slice already exists. This script must not be run twice."
}

proc disconnect_axil_sink {intf_name} {
    set intf [get_bd_intf_pins -quiet $intf_name]
    if {[llength $intf] != 1} { error "Expected one AXI interface '$intf_name', found: $intf" }
    foreach net [get_bd_intf_nets -quiet -of_objects $intf] { disconnect_bd_intf_net $net $intf }
}
proc connect_axil_clock {source sink} {
    set sink_pin [get_bd_pins -quiet $sink]
    foreach net [get_bd_nets -quiet -of_objects $sink_pin] { disconnect_bd_net $net $sink_pin }
    connect_bd_net [get_bd_pins $source] $sink_pin
}

# M00 is the established 0x4000_0000 pd_feature_0 AXI4-Lite connection.
disconnect_axil_sink pd_feature_0/s_axi
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice:2.1 $slice

# AXI Register Slice v2.1 configures each channel independently; it does not
# have a CONFIG.REG_CONFIG property.  Its default registered channel settings
# are retained and the interface is explicitly made AXI4-Lite.
set_property -dict [list CONFIG.PROTOCOL {AXI4LITE} CONFIG.DATA_WIDTH {32} CONFIG.ADDR_WIDTH {16}] [get_bd_cells $slice]
connect_bd_intf_net [get_bd_intf_pins axi_ic_ctrl/M00_AXI] [get_bd_intf_pins $slice/S_AXI]
connect_bd_intf_net [get_bd_intf_pins $slice/M_AXI] [get_bd_intf_pins pd_feature_0/s_axi]
connect_axil_clock clk_wiz_0/clk_out1           $slice/aclk
connect_axil_clock rst_ps7_0/peripheral_aresetn $slice/aresetn

validate_bd_design
save_bd_design
generate_target all $bd_file
export_ip_user_files -of_objects $bd_file -no_script -sync -force -quiet
update_compile_order -fileset sources_1

puts ""
puts "FEATURE_AXIL_REGISTER_SLICE_INSERT_PASS"
puts "  Control path: axi_ic_ctrl/M00_AXI -> $slice -> pd_feature_0/s_axi"
puts "  Feature address remains 0x40000000; DDS, filter data path, DMA and ILA are unchanged."
puts "  Next: reset and rerun synth_1 / impl_1; require WNS >= 0 and TNS = 0."
