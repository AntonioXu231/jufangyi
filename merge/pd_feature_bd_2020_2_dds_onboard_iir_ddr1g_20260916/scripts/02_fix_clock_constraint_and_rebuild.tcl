# Iteration 1: remove the duplicate Clock Wizard input primary-clock constraint.
# Run inside the open Vivado 2020.2 project Tcl console:
#   source F:/xinya/v5/merge/pd_feature_bd_2020_2/scripts/02_fix_clock_constraint_and_rebuild.tcl
#
# Scope: constraint-file enablement only.  No RTL/BD interface changes and no bitstream.

set script_dir [file normalize [file dirname [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set report_dir [file join $project_dir reports iter01_clock_constraint]
set clk_wiz_xdc [file join $project_dir pd_feature_bd_2020_2.gen sources_1 bd pd_feature_bd ip pd_feature_bd_clk_wiz_0_0 pd_feature_bd_clk_wiz_0_0.xdc]

if {[current_project] eq ""} {
    open_project [file join $project_dir pd_feature_bd_2020_2.xpr]
}
if {![file exists $clk_wiz_xdc]} {
    error "Clock Wizard XDC not found: $clk_wiz_xdc"
}

# The PS7 IP owns the true 150 MHz primary FCLK.  This generated Clock Wizard
# XDC incorrectly creates another primary clock downstream on clk_in1.  Vivado
# will automatically derive clk_out1 from the PS clock through the MMCM.
set clk_wiz_xdc_obj [get_files -quiet $clk_wiz_xdc]
if {[llength $clk_wiz_xdc_obj] != 1} {
    error "Expected exactly one Clock Wizard XDC file object, got [llength $clk_wiz_xdc_obj]"
}
set_property IS_ENABLED false $clk_wiz_xdc_obj

file mkdir $report_dir
report_compile_order -constraints -file [file join $report_dir constraint_compile_order.rpt]

# Constraint changes must be validated from a fresh timing-driven synthesis.
reset_run synth_1
reset_run impl_1
launch_runs synth_1 -jobs 6
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] ne "synth_design Complete!"} {
    error "Synthesis failed after clock-XDC change; no implementation was started."
}

open_run synth_1
report_clocks -file [file join $report_dir clocks_post_synth.rpt]
report_clock_interaction -delay_type max -file [file join $report_dir clock_interaction_post_synth.rpt]
check_timing -verbose -file [file join $report_dir check_timing_post_synth.rpt]

# Guard: the PL clock must remain the intended 130 MHz generated clock.
set pl_clk [get_clocks -quiet -of_objects [get_pins -hierarchical *clk_wiz_0*/clk_out1]]
if {[llength $pl_clk] != 1} {
    error "Expected one Clock Wizard clk_out1 clock, got [llength $pl_clk]."
}
set pl_period [get_property PERIOD $pl_clk]
if {[expr {abs($pl_period - 7.692307)}] > 0.010} {
    error "Unexpected PL clock period $pl_period ns; expected about 7.692 ns (130 MHz)."
}

# Keep the baseline implementation directive; this iteration isolates the
# clock-constraint correction from any physical-optimization strategy change.
launch_runs impl_1 -to_step route_design -jobs 6
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] ne "route_design Complete!"} {
    error "Implementation did not reach route_design Complete."
}

open_run impl_1
report_timing_summary -delay_type min_max -report_unconstrained -max_paths 20 \
    -file [file join $report_dir timing_summary_post_route.rpt]
report_timing -delay_type max -max_paths 50 -sort_by slack \
    -file [file join $report_dir timing_max_paths_post_route.rpt]
report_clock_interaction -delay_type max \
    -file [file join $report_dir clock_interaction_post_route.rpt]
report_drc -file [file join $report_dir drc_post_route.rpt]
report_methodology -file [file join $report_dir methodology_post_route.rpt]
report_utilization -hierarchical -file [file join $report_dir utilization_post_route.rpt]

puts "ITER01_CLOCK_CONSTRAINT_OK: reports written to $report_dir"
