# Retarget the existing Zynq-7020 Block Design from 26 MSPS to 65 MSPS.
# Vivado 2020.2; run only after saving/closing any GUI project session:
#   vivado -mode batch -source scripts/11_retarget_65msps.tcl
# This updates the Clock Wizard and PL module-reference parameters, refreshes
# their boundary metadata, validates and saves the BD, then regenerates BD
# output products. It does not synthesize or
# implement the design and does not download anything to hardware.
# Expected console marker: PD_65MSPS_BD_PASS; expected source changes include
# pd_feature_bd.bd and clk_wiz / custom-IP XCI metadata.

set script_dir [file dirname [file normalize [info script]]]
set project_dir [file normalize [file join $script_dir ..]]
set project_file [file join $project_dir pd_feature_bd_2020_2.xpr]

if {![file exists $project_file]} {
    error "Vivado project not found: $project_file"
}

open_project $project_file

set bd_files [get_files -all -quiet */pd_feature_bd.bd]
if {[llength $bd_files] != 1} {
    error "Expected one pd_feature_bd.bd, found [llength $bd_files]"
}
set bd_file [lindex $bd_files 0]
open_bd_design $bd_file

foreach cell_name {clk_wiz_0 pd_ddr_0 pd_feature_0 pd_filter_0 pd_dds_0} {
    if {[llength [get_bd_cells -quiet $cell_name]] != 1} {
        error "Required Block Design cell is missing: $cell_name"
    }
}

# The checked-in BD carries the already-propagated 65 MHz module-ref port
# metadata. Clock FREQ_HZ/CLK_DOMAIN are read-only on those pins; validate their
# persisted values below instead of trying to write them through BD properties.

# Keep the feature/system clock at 130 MHz and raise only the ADC clock to
# 65 MHz (integer 2:1 crossing, generated from the same 50 MHz PL reference).
set_property -dict [list \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {130.000} \
    CONFIG.CLKOUT2_REQUESTED_OUT_FREQ {65.000} \
] [get_bd_cells clk_wiz_0]

set_property CONFIG.SAMPLE_HZ {65000000} [get_bd_cells pd_feature_0]
set_property CONFIG.SAMPLE_HZ {65000000} [get_bd_cells pd_filter_0]
set_property CONFIG.CLK_HZ {65000000} [get_bd_cells pd_dds_0]
set_property CONFIG.SAMPLE_HZ {65000000} [get_bd_cells pd_dds_0]

if {[get_property CONFIG.CLKOUT1_REQUESTED_OUT_FREQ [get_bd_cells clk_wiz_0]] != "130.000"} {
    error "Clock Wizard system clock did not remain at 130 MHz"
}
if {[get_property CONFIG.CLKOUT2_REQUESTED_OUT_FREQ [get_bd_cells clk_wiz_0]] != "65.000"} {
    error "Clock Wizard ADC clock is not 65 MHz"
}
foreach cell_name {pd_feature_0 pd_filter_0 pd_dds_0} {
    if {[get_property CONFIG.SAMPLE_HZ [get_bd_cells $cell_name]] != "65000000"} {
        error "$cell_name SAMPLE_HZ did not update to 65 MSPS"
    }
}
if {[get_property CONFIG.CLK_HZ [get_bd_cells pd_dds_0]] != "65000000"} {
    error "pd_dds_0 CLK_HZ did not update to 65 MHz"
}

validate_bd_design
foreach pin_name {pd_ddr_0/adc_clk pd_feature_0/adc_clk pd_dds_0/clk} {
    set pin_obj [get_bd_pins $pin_name]
    set pin_hz [get_property CONFIG.FREQ_HZ $pin_obj]
    set pin_domain [get_property CONFIG.CLK_DOMAIN $pin_obj]
    # CLK_DOMAIN records the Clock Wizard's master domain (clk_out1), while
    # the connected generated clock is clk_out2 at 65 MHz.
    if {$pin_hz != 65000000 || $pin_domain ne "/clk_wiz_0_clk_out1"} {
        error "$pin_name clock metadata mismatch: FREQ_HZ=$pin_hz CLK_DOMAIN=$pin_domain"
    }
}
save_bd_design
generate_target all [get_files $bd_file]
update_compile_order -fileset sources_1

puts "PD_65MSPS_BD_PASS project=$project_file bd=$bd_file adc_hz=65000000 system_hz=130000000"
close_project
