# ============================================================================
# Restore AXI-Lite address reachability for pd_feature v3
#
# Run from the Tcl Console of the already-open pd_feature_bd_2020_2 project:
#   source {F:/xinya/v5/merge/pd_feature_bd_2020_2/scripts/04_restore_v3_axi_address_map.tcl}
#
# Effects:
#   1. Assigns PS Data-space addresses to both custom AXI-Lite slaves.
#   2. Regenerates BD/IP output products so the stale mref implementation is
#      rebuilt from the current RTL.
#   3. Resets synth_1 and impl_1.  Existing run results are intentionally made
#      stale; they were produced before the address path was reachable.
#
# No RTL, XDC, board pin, or algorithm parameter is modified by this script.
# ============================================================================

set script_name [file tail [info script]]
set project_name [get_property NAME [current_project]]
if {$project_name ne "pd_feature_bd_2020_2"} {
    error "$script_name must run in pd_feature_bd_2020_2; current project is '$project_name'."
}

set bd_file [get_files -quiet */pd_feature_bd.bd]
if {[llength $bd_file] != 1} {
    error "Expected exactly one pd_feature_bd.bd, found: $bd_file"
}
open_bd_design $bd_file

proc require_one_addr_seg {intf_pin label} {
    set segs [get_bd_addr_segs -quiet -of_objects [get_bd_intf_pins $intf_pin]]
    if {[llength $segs] != 1} {
        error "$label: expected one AXI-Lite address segment below '$intf_pin', found: $segs"
    }
    return [lindex $segs 0]
}

# Both AXI-Lite ports expose 16 address bits, so their legal map range is 64 KB.
set feature_seg [require_one_addr_seg pd_feature_0/s_axi pd_feature_0]
set ddr_seg     [require_one_addr_seg pd_ddr_0/s_axi     pd_ddr_0]
set ps_data     [get_bd_addr_spaces -quiet processing_system7_0/Data]
if {[llength $ps_data] != 1} {
    error "Expected one PS Data address space, found: $ps_data"
}

# Assign against the *master* PS address space.  Setting OFFSET on a slave
# segment is not sufficient in Vivado 2020.2 and may cause auto-assignment.
# Keep the feature-register base defined by the project interface contract.
assign_bd_address -target_address_space $ps_data -offset 0x40000000 -range 64K $feature_seg

# Place the DDR control/status AXI-Lite aperture directly after feature's 64 KB.
assign_bd_address -target_address_space $ps_data -offset 0x40010000 -range 64K $ddr_seg

# pd_ddr_0 has independent read and write AXI masters.  Each needs a mapping
# through its connected PS HP port before it can access DDR.
set rd_space [get_bd_addr_spaces -quiet pd_ddr_0/m_axi_rd]
set wr_space [get_bd_addr_spaces -quiet pd_ddr_0/m_axi_wr]
set cw_space [get_bd_addr_spaces -quiet pd_ddr_0/m_axi_cw]
if {[llength $rd_space] != 1 || [llength $wr_space] != 1 || [llength $cw_space] != 1} {
    error "Expected one pd_ddr_0 read, write, and control-write address space; got rd='$rd_space', wr='$wr_space', cw='$cw_space'."
}
set hp1_ddr [get_bd_addr_segs -quiet processing_system7_0/S_AXI_HP1/HP1_DDR_LOWOCM]
set hp0_ddr [get_bd_addr_segs -quiet processing_system7_0/S_AXI_HP0/HP0_DDR_LOWOCM]
if {[llength $hp1_ddr] != 1 || [llength $hp0_ddr] != 1} {
    error "Expected one HP1 and one HP0 DDR segment; got hp1='$hp1_ddr', hp0='$hp0_ddr'."
}
assign_bd_address -target_address_space $rd_space -offset 0x00000000 -range 1G $hp1_ddr
assign_bd_address -target_address_space $wr_space -offset 0x00000000 -range 1G $hp0_ddr
assign_bd_address -target_address_space $cw_space -offset 0x00000000 -range 1G $hp1_ddr

validate_bd_design
save_bd_design

# Rebuild generated products of the module-reference IP and BD.  This clears
# the stale-mref condition without changing any IP configuration parameters.
generate_target all $bd_file
export_ip_user_files -of_objects $bd_file -no_script -sync -force -quiet
update_compile_order -fileset sources_1

set report_dir [file normalize [file join [get_property DIRECTORY [current_project]] reports iter02_v3_address_map]]
file mkdir $report_dir
set address_report [file join $report_dir bd_address_map.rpt]
set fh [open $address_report w]
puts $fh "pd_feature v3 AXI-Lite address map"
puts $fh ""
set feature_map [get_bd_addr_segs -quiet -of_objects $ps_data -filter {NAME =~ *pd_feature_0*}]
set ddr_map     [get_bd_addr_segs -quiet -of_objects $ps_data -filter {NAME =~ *pd_ddr_0*}]
foreach seg [concat $feature_map $ddr_map] {
    puts $fh "Segment: $seg"
    puts $fh "  OFFSET: [get_property OFFSET $seg]"
    puts $fh "  RANGE:  [get_property RANGE  $seg]"
}
puts $fh ""
puts $fh "pd_ddr_0 DDR master mappings"
foreach {label space} [list m_axi_rd $rd_space m_axi_wr $wr_space m_axi_cw $cw_space] {
    foreach seg [get_bd_addr_segs -quiet -of_objects $space] {
        puts $fh "$label: $seg"
        puts $fh "  OFFSET: [get_property OFFSET $seg]"
        puts $fh "  RANGE:  [get_property RANGE  $seg]"
    }
}
close $fh

# The former synth/impl results reflect an unreachable AXI path and cannot be
# used for timing or resource sign-off after this change.
reset_run synth_1
reset_run impl_1

puts ""
puts "RESTORE_V3_AXI_ADDRESS_MAP_PASS"
puts "  $feature_seg -> 0x40000000 / 64K"
puts "  $ddr_seg     -> 0x40010000 / 64K"
puts "  pd_ddr_0/m_axi_rd -> HP1 DDR @ 0x00000000 / 1G"
puts "  pd_ddr_0/m_axi_wr -> HP0 DDR @ 0x00000000 / 1G"
puts "  pd_ddr_0/m_axi_cw -> HP1 DDR @ 0x00000000 / 1G"
puts "  Address report: [file join $report_dir bd_address_map.rpt]"
puts "  synth_1 and impl_1 were reset. Run synthesis and implementation next."
