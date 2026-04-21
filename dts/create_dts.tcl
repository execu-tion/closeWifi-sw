#/usr/bin/tclsh
# must interpreter by rdi_xsct
#
set exec_dir [ exec pwd ]
set work_dir $exec_dir/build

exec cp $exec_dir/system_top.xsa $work_dir/system_top.xsa
cd $work_dir
set exec_dir [ exec pwd ]

##
hsi open_hw_design $work_dir/system_top.xsa
hsi set_repo_path $work_dir/../device-tree-xlnx

set procs [hsi get_cells -hier -filter {IP_TYPE==PROCESSOR}]
puts "List of processors found in XSA is $procs"
hsi create_sw_design device-tree -os device_tree -proc ps7_cortexa9_0

hsi generate_target -dir $work_dir
hsi close_hw_design [hsi current_hw_design]
##
#exec $exec_dir/patch.sh $work_dir/pcw.dtsi
##exec $exec_dir/dcc $work_dir/system-top.dts $exec_dir/system.dtb
##exec cp $work_dir/system.dt? $exec_dir
cd ..

exit
