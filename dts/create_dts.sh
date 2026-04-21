#!/usr/bin/bash
source log.sh
LOG_I $0 "executing!"
EXEC_DIR=$PWD

if [ $# -ne 1 ];then
	LOG_E "error parameters!"
	LOG_W "Usage:"
	LOG_W "\t./create_dts.sh [path_to_xsa]"
	exit 255
else
	XSA_FILE=$1
	if [ -e $XSA_FILE ];then
		cp $XSA_FILE $EXEC_DIR/system_top.xsa
	else
		echo "$XSA_FILE not exists!"
		exit 255
	fi
fi
XSA_FILE=$EXEC_DIR/system_top.xsa
XLNX_DT_DIR=$EXEC_DIR/device-tree-xlnx
XSCT_DT_SHL=$EXEC_DIR/create_dts.tcl
WORK_DIR=$EXEC_DIR/build
PACK_DTC=dcc.sh
# check file exists
check `[ -f $XSA_FILE ]   ` $? "$XSA_FILE not exists!" "$XSA_FILE exists!" || exit 83
check `[ -d $XLNX_DT_DIR ]` $? "$XLNX_DT_DIR not exists!" "$XLNX_DT_DIR exists!" || exit 83
check `[ -f $XSCT_DT_SHL ]` $? "$XSCT_DT_SHL not exists!" "$XSCT_DT_SHL exists!" || exit 83
check `[ -d $WORK_DIR ]   ` $? "$WORK_DIR not exists and will be created!" "$WORK_DIR exists!" || mkdir $WORK_DIR
# gen dts
LOG_I $XSCT_DT_SHL "executing!"
xsct $XSCT_DT_SHL
#loader -exec rdi_xsct $XSCT_DT_SHL
check $? "GEN DTS fail for early errors!" "GEN DTS success"

cd $WORK_DIR && ../patch.sh "$WORK_DIR/pcw.dtsi" "$WORK_DIR/system-top.dts" "$WORK_DIR/pl.dtsi" "$WORK_DIR/zynq-7000.dtsi"
cd $WORK_DIR && ../$PACK_DTC $WORK_DIR/system-top.dts $EXEC_DIR/system.dtb && cd $EXEC_DIR
check $? "GEN DTB fail!" "GEN DTB success!"


#check `./pa`


