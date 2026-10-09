# Out-of-context synthesis of the COE-backed DDS module and its four BMG ROMs.
# This checks RTL/IP elaboration, resource inference and a 65 MHz source clock;
# it is not a substitute for full Block Design synthesis or implementation.
# It does not reset synth_1 or impl_1.
#
# Vivado 2020.2 batch:
#   vivado -mode batch -source scripts/15_check_pd_coe_source_synthesis.tcl

set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set xpr_file [file join $project_dir pd_feature_bd_2020_2.xpr]
set report_dir [file normalize [file join $project_dir .. .. .codex_tmp pd_coe_source_synth_reports]]
file mkdir $report_dir

open_project $xpr_file
source [file join $script_dir 13_configure_matlab_coe_roms.tcl]

# Vivado 2020.2 emits a warning for synth_ip in project mode, but executes
# the OOC flow and writes the DCP consumed by the source-level synthesis below.
foreach ip_name {
    pd_pd_template_rom_ch0
    pd_pd_template_rom_ch1
    pd_pd_template_rom_ch2
    pd_pd_template_rom_ch3
} {
    puts "Synthesizing $ip_name out of context..."
    synth_ip [get_ips $ip_name] -force
}

set source_fileset [get_filesets sources_1]
set original_top [get_property top $source_fileset]
set project_xdcs [get_files -all -quiet -filter {FILE_TYPE == XDC}]
set original_xdc_states {}
foreach xdc_file $project_xdcs {
    lappend original_xdc_states [list $xdc_file [get_property IS_ENABLED $xdc_file]]
    set_property IS_ENABLED false $xdc_file
}

set_property top pd_dds_adc_source $source_fileset
set synth_error [catch {
    synth_design -top pd_dds_adc_source -part xc7z020clg400-2 -flatten_hierarchy none
    create_clock -name dds65 -period 15.384 [get_ports clk]
    report_utilization -hierarchical -file [file join $report_dir utilization_synth.rpt]
    report_timing_summary -delay_type max -file [file join $report_dir timing_summary_synth.rpt]
} synth_message]

# Do not leave the project with this temporary source-only top or constraint edit.
set_property top $original_top $source_fileset
foreach xdc_state $original_xdc_states {
    lassign $xdc_state xdc_file was_enabled
    set_property IS_ENABLED $was_enabled $xdc_file
}
close_project
if {$synth_error} {
    error "OOC DDS synthesis failed: $synth_message"
}

puts "PD_COE_SOURCE_SYNTH_PASS"
puts "Reports: $report_dir"
