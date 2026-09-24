# Remove the optional four-channel FFT branch from the BD project.
# This script intentionally does not run output generation, simulation,
# synthesis, implementation, or bitstream generation.

set project_dir [file normalize [file join [file dirname [info script]] ..]]
set project_file [file join $project_dir pd_feature_bd_2020_2.xpr]
if {![file exists $project_file]} {
    error "Project not found: $project_file"
}

open_project $project_file
set bd_file [get_files -quiet */pd_feature_bd.bd]
if {[llength $bd_file] != 1} {
    error "Expected exactly one pd_feature_bd.bd, found: $bd_file"
}
open_bd_design $bd_file

# These cells form the complete optional FFT branch.
set fft_cells {
    xfft_ch0
    xfft_ch1
    xfft_ch2
    xfft_ch3
    pd_fft_input_adapter_0
    pd_fft_bin_monitor_4_0
}

foreach cell_name $fft_cells {
    set cell [get_bd_cells -quiet $cell_name]
    if {[llength $cell] == 1} {
        puts "Removing FFT BD cell: $cell_name"
        delete_bd_objs $cell
    } elseif {[llength $cell] == 0} {
        puts "FFT BD cell already absent: $cell_name"
    } else {
        error "Expected one BD cell '$cell_name', found: $cell"
    }
}

# Remove only FFT-specific source and simulation files from project metadata.
set fft_files [list \
    [file join $project_dir pd_feature_bd_2020_2.srcs sources_1 imports rtl pd_fft_input_adapter_4ch.v] \
    [file join $project_dir pd_feature_bd_2020_2.srcs sources_1 imports rtl pd_fft_bin_monitor_4ch.v] \
    [file join $project_dir pd_feature_bd_2020_2.srcs sim_1 new tb_pd_fft_chain.v] \
    [file join $project_dir pd_feature_bd_2020_2.srcs sim_1 new tb_pd_fft_chain_wave.tcl]]

foreach f $fft_files {
    set project_file_obj [get_files -quiet $f]
    if {[llength $project_file_obj] > 0} {
        puts "Removing project file reference: $f"
        remove_files $project_file_obj
    }
}

validate_bd_design
save_bd_design
# Vivado 2020.2 persists project-file-list changes through save_project_as.
# Use the same project name and directory, replacing only the XPR metadata.
save_project_as -force pd_feature_bd_2020_2 $project_dir
close_project

puts "FFT_BD_CLEANUP_PASS"
