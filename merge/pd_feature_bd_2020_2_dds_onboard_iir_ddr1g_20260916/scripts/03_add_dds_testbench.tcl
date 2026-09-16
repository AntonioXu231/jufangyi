# Add the DDS behavioral regression to sim_1. Run in the open Vivado project.
set root {F:/xinya/v5/merge/pd_feature_bd_2020_2}
set tb [file join $root pd_feature_bd_2020_2.srcs sim_1 new tb_pd_feature_dds.v]
if {![file exists $tb]} { error "Missing testbench: $tb" }
if {[llength [get_files -quiet $tb]] == 0} { add_files -fileset sim_1 -norecurse $tb }
set_property top tb_pd_feature_dds [get_filesets sim_1]
update_compile_order -fileset sim_1
puts "DDS_TB_READY: launch_simulation -mode behavioral"
