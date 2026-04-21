#!/usr/bin/rdi_xsct
#
connect
target

ta 2
rst
puts "dow fsbl.elf"
dow output_boot_bin/fsbl.elf

puts "dow bitstream"
ta 4
fpga -f output_boot_bin/system_top.bit

ta 2
con
exec sleep 1
stop

puts "dow u-boot.elf"
dow ../u-boot-xlnx/u-boot.elf
con
exec sleep 2
stop

#puts "dow device blob"
#dow -data ../dts/system.dtb 0x03000000

#puts "dow kernel"
#dow -data output_boot_bin/uImage  0x01000000

#puts "dow rootfs"
#dow -data output_boot_bin/rootfs.uImage  0x02000000
con

