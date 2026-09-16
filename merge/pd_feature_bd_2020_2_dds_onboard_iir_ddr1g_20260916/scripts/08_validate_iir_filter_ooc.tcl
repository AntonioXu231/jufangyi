# Standalone pre-build check for the IIR filter RTL.
# Run from a command prompt or Vivado Tcl shell:
#   F:/vivado20/Vivado/2020.2/bin/vivado.bat -mode batch -source 08_validate_iir_filter_ooc.tcl
# Outputs: reports/filter_ooc_timing.rpt and reports/filter_ooc_utilization.rpt
set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set rtl_dir [file join $project_dir pd_feature_bd_2020_2.srcs sources_1 imports rtl]
set report_dir [file join $project_dir reports]
file mkdir $report_dir

read_verilog [file join $rtl_dir pd_iir_biquad.v]
read_verilog [file join $rtl_dir pd_filter_chain.v]
synth_design -top pd_filter_chain -part xc7z020clg400-2
create_clock -name filter_clk_130m -period 7.693 [get_ports clk]
report_timing_summary -delay_type max -max_paths 20 -file [file join $report_dir filter_ooc_timing.rpt]
report_utilization -hierarchical -file [file join $report_dir filter_ooc_utilization.rpt]

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1]]
puts "FILTER_OOC_WNS=$wns ns"
if {$wns < 0.0} {
    error "FILTER_OOC_FAIL: internal filter timing does not meet 130 MHz."
}
puts "FILTER_OOC_PASS"
