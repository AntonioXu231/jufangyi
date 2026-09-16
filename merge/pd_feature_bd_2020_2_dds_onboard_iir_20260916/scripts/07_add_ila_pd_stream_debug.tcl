# ============================================================================
# Insert the first on-board DDS debug ILA into pd_feature_bd_2020_2.
#
# Run after opening this project's XPR in Vivado 2020.2:
#   source {F:/xinya/v5/merge/pd_feature_bd_2020_2_dds_onboard_20260914/scripts/07_add_ila_pd_stream_debug.tcl}
#
# This script creates one 1024-sample, 130 MHz ILA.  It observes the feature
# input-valid vector and the AXI-Stream event output without changing the
# algorithm, AXI address map, DDR data path, or DDS source.
# ============================================================================

set script_dir [file dirname [file normalize [info script]]]
set project_xpr [file normalize [file join $script_dir .. pd_feature_bd_2020_2.xpr]]
if {[llength [get_projects -quiet]] == 0} {
    open_project $project_xpr
}

set expected_project "pd_feature_bd_2020_2"
if {[get_property NAME [current_project]] ne $expected_project} {
    error "Open the pd_feature_bd_2020_2 project in this DDS version directory first."
}

set bd_file [get_files -quiet */pd_feature_bd.bd]
if {[llength $bd_file] != 1} {
    error "Expected exactly one pd_feature_bd.bd, found: $bd_file"
}
open_bd_design $bd_file

set ila_name ila_pd_stream_0
if {[llength [get_bd_cells -quiet $ila_name]] != 0} {
    error "$ila_name already exists. Do not run this insertion script twice."
}

proc attach_ila_probe {probe_pin observed_pin} {
    set observed [get_bd_pins -quiet $observed_pin]
    if {[llength $observed] != 1} {
        error "Expected one observed pin '$observed_pin', found: $observed"
    }
    set nets [get_bd_nets -quiet -of_objects $observed]
    if {[llength $nets] != 1} {
        error "Expected one net on '$observed_pin', found: $nets"
    }
    connect_bd_net $nets [get_bd_pins $probe_pin]
}

create_bd_cell -type ip -vlnv xilinx.com:ip:ila:6.2 $ila_name
set_property -dict [list \
    CONFIG.C_DATA_DEPTH    {1024} \
    CONFIG.C_NUM_OF_PROBES {6} \
    CONFIG.C_PROBE0_WIDTH  {4} \
    CONFIG.C_PROBE1_WIDTH  {1} \
    CONFIG.C_PROBE2_WIDTH  {1} \
    CONFIG.C_PROBE3_WIDTH  {1} \
    CONFIG.C_PROBE4_WIDTH  {64} \
    CONFIG.C_PROBE5_WIDTH  {1} \
] [get_bd_cells $ila_name]

# ILA clock is the 130 MHz AXI/feature-output domain.
attach_ila_probe $ila_name/clk                  clk_wiz_0/clk_out1
attach_ila_probe $ila_name/probe0               pd_ddr_0/o_feat_dv
attach_ila_probe $ila_name/probe1               pd_feature_0/m_axis_tvalid
attach_ila_probe $ila_name/probe2               pd_feature_0/m_axis_tready
attach_ila_probe $ila_name/probe3               pd_feature_0/m_axis_tlast
attach_ila_probe $ila_name/probe4               pd_feature_0/m_axis_tdata
attach_ila_probe $ila_name/probe5               pd_feature_0/irq

validate_bd_design
save_bd_design
generate_target all $bd_file
export_ip_user_files -of_objects $bd_file -no_script -sync -force -quiet
update_compile_order -fileset sources_1

reset_run synth_1
reset_run impl_1

puts ""
puts "ILA_PD_STREAM_INSERT_PASS"
puts "  ILA: $ila_name, clock: clk_out1 (130 MHz), depth: 1024"
puts "  probe0=o_feat_dv[3:0]"
puts "  probe1=tvalid, probe2=tready, probe3=tlast"
puts "  probe4=tdata[63:0], probe5=irq"
puts "  synth_1 and impl_1 reset; rebuild to write matching .bit and .ltx."
