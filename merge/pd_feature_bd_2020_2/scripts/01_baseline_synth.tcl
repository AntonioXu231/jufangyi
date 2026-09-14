# Vivado 2020.2 baseline synthesis and timing evidence collection.
# Run from any directory:
#   vivado.bat -mode batch -source 01_baseline_synth.tcl -notrace
# This script intentionally does not run implementation or write a bitstream.

set script_dir [file normalize [file dirname [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set project_file [file join $project_dir pd_feature_bd_2020_2.xpr]
set report_dir [file join $project_dir reports baseline_synth]

if {![file exists $project_file]} {
    error "Project file not found: $project_file"
}

file mkdir $report_dir
open_project $project_file

# Refresh generated HDL/compile order from the current BD and module references.
update_compile_order -fileset sources_1
report_ip_status -file [file join $report_dir ip_status_pre_synth.rpt]

# A synthesis result interrupted before completion is not reusable as a baseline.
set synth_run [get_runs synth_1]
if {[get_property STATUS $synth_run] ne "synth_design Complete!"} {
    reset_run synth_1
}
launch_runs synth_1 -jobs 6
wait_on_run synth_1

if {[get_property STATUS $synth_run] ne "synth_design Complete!"} {
    error "Synthesis did not complete. See $project_dir/pd_feature_bd_2020_2.runs/synth_1/runme.log"
}

open_run synth_1
report_timing_summary -delay_type min_max -report_unconstrained -max_paths 20 \
    -file [file join $report_dir timing_summary.rpt]
report_timing -delay_type max -max_paths 50 -sort_by slack \
    -file [file join $report_dir timing_max_paths.rpt]
report_timing -delay_type min -max_paths 50 -sort_by slack \
    -file [file join $report_dir timing_min_paths.rpt]
report_clock_interaction -delay_type max \
    -file [file join $report_dir clock_interaction.rpt]
report_clocks -file [file join $report_dir clocks.rpt]
check_timing -verbose -file [file join $report_dir check_timing.rpt]
report_utilization -hierarchical -file [file join $report_dir utilization.rpt]
report_drc -file [file join $report_dir drc_post_synth.rpt]
close_project
puts "BASELINE_SYNTH_OK: reports written to $report_dir"
