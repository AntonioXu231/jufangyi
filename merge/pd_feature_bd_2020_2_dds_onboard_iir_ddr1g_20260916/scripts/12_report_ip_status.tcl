# Read-only diagnostic for the current Vivado project IP lock state.
# Vivado 2020.2:
#   vivado -mode batch -source scripts/12_report_ip_status.tcl
# Expected: IP status table in the console and Vivado log; no project save.

set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set project_file [file join $project_dir pd_feature_bd_2020_2.xpr]
open_project $project_file
report_ip_status
set target_ips [get_ips -all -quiet *auto_pc*]
puts "AUTO_PC_IPS=$target_ips"
foreach ip $target_ips {
    report_property $ip
}
set bd_files [get_files -all -quiet */pd_feature_bd.bd]
if {[llength $bd_files] == 1} {
    open_bd_design [lindex $bd_files 0]
    set auto_pc_cells [get_bd_cells -hier -quiet -filter {VLNV =~ *axi_protocol_converter*}]
    foreach cell $auto_pc_cells {
        puts "AUTO_PC_CELL=$cell"
        puts "AUTO_PC_PROPERTIES=[list_property $cell]"
        report_property $cell
    }
}
close_project
