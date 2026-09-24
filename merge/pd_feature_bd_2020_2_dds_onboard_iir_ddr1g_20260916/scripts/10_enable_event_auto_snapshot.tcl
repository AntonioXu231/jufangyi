# =============================================================================
# 10_enable_event_auto_snapshot.tcl
#
# Purpose:
#   Refresh the two module references after their RTL port additions, then
#   connect pd_feature_0/o_event_accept[3:0] to
#   pd_ddr_0/i_event_accept[3:0].  Existing AXI, DMA and IRQ connections are
#   untouched.
#
# Run this from the Vivado Tcl Console while this project is open:
#   source {F:/xinya/v5/merge/pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916/scripts/10_enable_event_auto_snapshot.tcl}
# =============================================================================

set project_dir [get_property DIRECTORY [current_project]]
set src_file [file normalize "$project_dir/pd_feature_bd_2020_2.srcs/sources_1/new/pd_snapshot_trigger.v"]
set tb_file  [file normalize "$project_dir/pd_feature_bd_2020_2.srcs/sim_1/new/tb_pd_snapshot_trigger.v"]

if {![file exists $src_file]} {
    error "Missing RTL source: $src_file"
}
if {![file exists $tb_file]} {
    error "Missing testbench: $tb_file"
}

# The new leaf module must be in sources_1 before pd_ddr_wr_top is refreshed.
if {[llength [get_files -quiet $src_file]] == 0} {
    add_files -fileset sources_1 -norecurse $src_file
}
if {[llength [get_files -quiet $tb_file]] == 0} {
    add_files -fileset sim_1 -norecurse $tb_file
}
update_compile_order -fileset sources_1

set bd_file [get_files -quiet */pd_feature_bd.bd]
if {[llength $bd_file] != 1} {
    error "Expected exactly one pd_feature_bd.bd, found: $bd_file"
}
open_bd_design $bd_file

set feature_event_pin [get_bd_pins -quiet pd_feature_0/o_event_accept]
set ddr_event_pin     [get_bd_pins -quiet pd_ddr_0/i_event_accept]
# Refresh custom module ports only when absent.  update_module_reference takes
# the generated module-reference IP name, not the BD cell object.  Skipping it
# for already upgraded cells also makes this script safe to run a second time.
if {[llength $feature_event_pin] != 1} {
    set feature_ip [get_ips -quiet pd_feature_bd_pd_feature_0_0]
    if {[llength $feature_ip] != 1} {
        error "Cannot find module-reference IP pd_feature_bd_pd_feature_0_0"
    }
    update_module_reference $feature_ip
}
if {[llength $ddr_event_pin] != 1} {
    set ddr_ip [get_ips -quiet pd_feature_bd_pd_ddr_0_0]
    if {[llength $ddr_ip] != 1} {
        error "Cannot find module-reference IP pd_feature_bd_pd_ddr_0_0"
    }
    update_module_reference $ddr_ip
}

# Re-query after the optional IP upgrades.
set feature_event_pin [get_bd_pins -quiet pd_feature_0/o_event_accept]
set ddr_event_pin     [get_bd_pins -quiet pd_ddr_0/i_event_accept]
if {[llength $feature_event_pin] != 1 || [llength $ddr_event_pin] != 1} {
    error "Module-reference refresh did not expose event ports; check sources_1 compile order."
}

set ddr_event_net [get_bd_nets -quiet -of_objects $ddr_event_pin]
if {[llength $ddr_event_net] == 0} {
    connect_bd_net $feature_event_pin $ddr_event_pin
    puts "Connected: pd_feature_0/o_event_accept -> pd_ddr_0/i_event_accept"
} elseif {[llength $ddr_event_net] == 1 &&
          [lsearch -exact [get_bd_pins -quiet -of_objects $ddr_event_net] $feature_event_pin] >= 0} {
    puts "Event auto-snapshot connection already exists; no change made."
} else {
    error "pd_ddr_0/i_event_accept already belongs to unexpected net: $ddr_event_net"
}

validate_bd_design
save_bd_design
generate_target all $bd_file
export_ip_user_files -of_objects $bd_file -no_script -sync -force -quiet
make_wrapper -files $bd_file -top

puts "AUTO_SNAPSHOT_BD_UPDATE_PASS"
puts "Next: run tb_pd_snapshot_trigger, then regenerate the wrapper if Vivado marks it stale."
