#!/bin/bash
echo -e "\033[32m ENTER PATCH MODE!\033[0m"
## ENABLE DEBUG
# enable FSBL_DEBUG_INFO in the fsbl_debug.h
DEBUG_FS=`find $(pwd) -name "fsbl_debug.h"`
for i in $DEBUG_FS;do 
	#echo 'exec sed -i "36i #define FSBL_DEBUG_INFO" '$i >> $BUILD_DIR/create_fsbl_project.tcl
	sed -i "36i #define FSBL_DEBUG_INFO" $i
done

# set up cpu frequency
CHANGE_FS=`find $(pwd) -type f | xargs grep "0XF8000100, 0x0007F000U ,0x00028000U" | sort -u | awk -v FS=":" '{print $1}'`
for i in $CHANGE_FS;do
	SUB_LOC=`grep -n  "0XF8000100, 0x0007F000U ,0x00028000U" $i | awk -v FS=":" '{print $1}'`
	for k in $SUB_LOC;do
		sed -i "$(( $k + 5 ))"'s/0x00000010U/0x0003000U/g' $i
	done	
done
echo -e "\033[32m EXIT PATCH MODE!\033[0m"
