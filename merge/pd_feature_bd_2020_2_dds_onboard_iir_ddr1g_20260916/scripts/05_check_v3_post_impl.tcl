# ============================================================================
# Post-implementation guard for the restored pd_feature v3 address map
#
# Run only after impl_1 finishes successfully:
#   source {F:/xinya/v5/merge/pd_feature_bd_2020_2/scripts/05_check_v3_post_impl.tcl}
#
# This script does not modify the design. It writes the reports required to
# prove that the complete v3 feature IP, rather than an optimized-away subset,
# reached implementation.
# ============================================================================

set project_name [get_property NAME [current_project]]
if {$project_name ne "pd_feature_bd_2020_2"} {
    error "Run this script in pd_feature_bd_2020_2; current project is '$project_name'."
}

set impl_status [get_property STATUS [get_runs impl_1]]
if {[string first "Complete!" $impl_status] < 0} {
    error "impl_1 is not complete. Current status: $impl_status"
}

open_run impl_1
set report_dir [file normalize [file join [get_property DIRECTORY [current_project]] reports iter02_v3_address_map]]
file mkdir $report_dir

report_utilization -hierarchical -file [file join $report_dir utilization_post_route.rpt]
report_timing_summary -delay_type min_max -report_unconstrained \
    -max_paths 20 -file [file join $report_dir timing_summary_post_route.rpt]
report_drc -file [file join $report_dir drc_post_route.rpt]

set feature_cells [get_cells -hier -quiet -filter {NAME =~ *pd_feature_0*}]
set prpd_cells    [get_cells -hier -quiet -filter {NAME =~ *u_prpd*}]
set regs_cells    [get_cells -hier -quiet -filter {NAME =~ *u_regs*}]

set fh [open [file join $report_dir v3_presence_check.txt] w]
puts $fh "pd_feature_0 descendant cells: [llength $feature_cells]"
puts $fh "u_prpd descendant cells:       [llength $prpd_cells]"
puts $fh "u_regs descendant cells:       [llength $regs_cells]"
puts $fh ""
puts $fh "The v3 address-map repair is accepted only when hierarchical utilization"
puts $fh "shows the expected register/PRPD implementation and timing plus DRC are clean."
close $fh

puts "V3_POST_IMPL_REPORTS_WRITTEN: $report_dir"
puts "Review utilization_post_route.rpt, timing_summary_post_route.rpt, drc_post_route.rpt, and v3_presence_check.txt."
