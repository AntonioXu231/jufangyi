# Configure four synchronous 1024 x 12-bit Block Memory Generator ROMs for
# the continuous MATLAB-derived PD template source.
#
# Prerequisites:
#   1. Open pd_feature_bd_2020_2.xpr in Vivado 2020.2.
#   2. Run sim/generate_pd_adc_waveform_physical_model.m in MATLAB once.
#   3. In Vivado Tcl Console, source this script.
#
# This script only adds/configures IP and RTL source dependencies. It does not
# run synthesis/implementation or reset existing runs.

set expected_project "pd_feature_bd_2020_2"
if {[current_project -quiet] eq "" || \
    [get_property NAME [current_project]] ne $expected_project} {
    error "Open the $expected_project project before sourcing this script."
}

set project_dir [get_property DIRECTORY [current_project]]
set rtl_dir [file normalize [file join $project_dir \
    pd_feature_bd_2020_2.srcs sources_1 imports rtl]]
set coe_dir [file normalize [file join $project_dir sim coe_templates]]
set ip_dir [file normalize [file join $project_dir \
    pd_feature_bd_2020_2.srcs sources_1 ip]]
set schedule_src [file join $rtl_dir pd_pd_event_schedule.v]

if {![file exists $schedule_src]} {
    error "MATLAB-generated RTL schedule is missing: $schedule_src"
}

file mkdir $ip_dir
set source_files [get_files -quiet $schedule_src]
if {[llength $source_files] == 0} {
    add_files -fileset sources_1 -norecurse $schedule_src
}

for {set ch 0} {$ch < 4} {incr ch} {
    set module_name "pd_pd_template_rom_ch${ch}"
    set coe_file [file normalize [file join $coe_dir "pd_pulse_ch${ch}.coe"]]
    if {![file exists $coe_file]} {
        error "COE template missing: $coe_file. Run the MATLAB generator first."
    }

    set ip [get_ips -quiet $module_name]
    if {[llength $ip] == 0} {
        create_ip -name blk_mem_gen -vendor xilinx.com -library ip \
            -version 8.4 -module_name $module_name -dir $ip_dir
        set ip [get_ips $module_name]
    }

    set_property -dict [list \
        CONFIG.Byte_Size {9} \
        CONFIG.Coe_File $coe_file \
        CONFIG.EN_SAFETY_CKT {false} \
        CONFIG.Enable_32bit_Address {false} \
        CONFIG.Enable_A {Always_Enabled} \
        CONFIG.Load_Init_File {true} \
        CONFIG.Memory_Type {Single_Port_ROM} \
        CONFIG.Port_A_Write_Rate {0} \
        CONFIG.Read_Width_A {12} \
        CONFIG.Register_PortA_Output_of_Memory_Primitives {false} \
        CONFIG.Use_Byte_Write_Enable {false} \
        CONFIG.Use_RSTA_Pin {false} \
        CONFIG.Write_Depth_A {1024} \
        CONFIG.Write_Width_A {12} \
        CONFIG.use_bram_block {Stand_Alone} \
    ] $ip
    generate_target all $ip
    # These ROM IPs are nested below the pd_dds_adc_source module reference.
    # Vivado 2020.2 rejects OOC checkpoints inside that reference during BD
    # refresh; source-level checks may still call synth_ip explicitly.
    set xci_path [file normalize [file join $ip_dir $module_name "$module_name.xci"]]
    set xci_file [get_files -quiet -all $xci_path]
    if {[llength $xci_file] != 1} {
        error "Expected one XCI for $module_name at $xci_path; found [llength $xci_file]"
    }
    set_property generate_synth_checkpoint 0 $xci_file
    puts "Configured $module_name from $coe_file"
}

export_ip_user_files -of_objects [get_ips pd_pd_template_rom_ch0] \
    -no_script -sync -force -quiet
export_ip_user_files -of_objects [get_ips pd_pd_template_rom_ch1] \
    -no_script -sync -force -quiet
export_ip_user_files -of_objects [get_ips pd_pd_template_rom_ch2] \
    -no_script -sync -force -quiet
export_ip_user_files -of_objects [get_ips pd_pd_template_rom_ch3] \
    -no_script -sync -force -quiet
update_compile_order -fileset sources_1

puts "PD_MATLAB_COE_ROM_SETUP_PASS channels=4 width=12 depth=1024 bits=49152"
