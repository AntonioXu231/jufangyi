# ============================================================================
# Enable synthesizable on-board DDS test mode for pd_feature_bd_2020_2.
#
# Run in the already-open Vivado project:
#   source {F:/xinya/v5/merge/pd_feature_bd_2020_2/scripts/06_enable_onboard_dds_test_mode.tcl}
#
# Board-test topology:
#   clk_wiz_0/clk_out2 (26 MHz) -> pd_dds_0 -> pd_ddr_0
#   pd_ddr_0 (CDC to clk_out1 = 130 MHz) -> pd_feature_0 -> AXI event output
#
# External ADC ports and their external-clock XDC are disabled in this test
# build.  Real ADC hardware validation needs a separate restore + board XDC
# step and is intentionally not implied by this mode.
# ============================================================================

set expected_project "pd_feature_bd_2020_2"
if {[get_property NAME [current_project]] ne $expected_project} {
    error "Run this script in $expected_project."
}

set project_dir [get_property DIRECTORY [current_project]]
set dds_src [file normalize [file join $project_dir pd_feature_bd_2020_2.srcs sources_1 imports rtl pd_dds_adc_source.v]]
if {![file exists $dds_src]} {
    error "DDS RTL not found: $dds_src"
}

if {[llength [get_files -quiet $dds_src]] == 0} {
    add_files -fileset sources_1 -norecurse $dds_src
}
update_compile_order -fileset sources_1

set bd_file [get_files -quiet */pd_feature_bd.bd]
if {[llength $bd_file] != 1} {
    error "Expected exactly one pd_feature_bd.bd, found: $bd_file"
}
open_bd_design $bd_file

if {[llength [get_bd_cells -quiet pd_dds_0]] == 0} {
    create_bd_cell -type module -reference pd_dds_adc_source pd_dds_0
}

proc disconnect_pin_from_all_nets {pin_name} {
    set pin [get_bd_pins -quiet $pin_name]
    if {[llength $pin] != 1} {
        error "Expected one BD pin '$pin_name', found: $pin"
    }
    foreach net [get_bd_nets -quiet -of_objects $pin] {
        disconnect_bd_net $net $pin
    }
}

# Remove only the external ADC source connections. AXI, DDR and feature IP
# interfaces remain untouched.
foreach pin_name {
    pd_dds_0/clk
    pd_dds_0/rst_n
    pd_feature_0/adc_clk
    pd_ddr_0/adc_clk
    pd_ddr_0/adc_data
    pd_ddr_0/adc_dv
    pd_feature_0/sync_in
    sync_pulse_0/sync_in
} {
    disconnect_pin_from_all_nets $pin_name
}
foreach port_name {adc_clk_0 adc_data_0 adc_dv_0 sync_in_0} {
    set port [get_bd_ports -quiet $port_name]
    if {[llength $port] == 1} {
        delete_bd_objs $port
    }
}

# Connect through source pins, not a named BD net.  This avoids Vivado 2020.2
# collection issues and makes the script repeatable after a partially applied
# GUI edit. clk_out2 must already be enabled at 26 MHz in the Clock Wizard;
# it clocks the DDS and ADC-side capture logic. clk_out1 remains the 130 MHz
# system clock.
connect_bd_net [get_bd_pins clk_wiz_0/clk_out2] [get_bd_pins pd_dds_0/clk]
connect_bd_net [get_bd_pins rst_ps7_0/peripheral_aresetn] [get_bd_pins pd_dds_0/rst_n]
connect_bd_net [get_bd_pins clk_wiz_0/clk_out2] [get_bd_pins pd_ddr_0/adc_clk]
connect_bd_net [get_bd_pins clk_wiz_0/clk_out2] [get_bd_pins pd_feature_0/adc_clk]
connect_bd_net [get_bd_pins pd_dds_0/adc_data] [get_bd_pins pd_ddr_0/adc_data]
connect_bd_net [get_bd_pins pd_dds_0/adc_dv]   [get_bd_pins pd_ddr_0/adc_dv]
connect_bd_net [get_bd_pins pd_dds_0/sync_in] \
               [get_bd_pins pd_feature_0/sync_in] \
               [get_bd_pins sync_pulse_0/sync_in]

# The original XDC describes an external 26 MHz port that no longer exists in
# this build. Disable it rather than weakening or waiving any DRC.
set external_adc_xdc [get_files -quiet */pd_feature_bd_timing.xdc]
if {[llength $external_adc_xdc] != 1} {
    error "Expected one external ADC timing XDC, found: $external_adc_xdc"
}
set_property IS_ENABLED false $external_adc_xdc

validate_bd_design
save_bd_design
generate_target all $bd_file
export_ip_user_files -of_objects $bd_file -no_script -sync -force -quiet
update_compile_order -fileset sources_1

reset_run synth_1
reset_run impl_1

puts ""
puts "ONBOARD_DDS_TEST_MODE_PASS"
puts "  External ADC ports removed from this test build."
puts "  pd_dds_0 supplies four channels at effective 26 MSPS and 50 Hz sync."
puts "  External ADC XDC disabled: $external_adc_xdc"
puts "  synth_1 and impl_1 reset; rebuild before adding ILA."
