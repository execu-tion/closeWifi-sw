#!/usr/bin/bash
#############################################
#	GCC PreProcessor for dts include
#
#	create: 25-04-17 dfy
#	rev0: file created
#############################################
SHL_DIR=`echo $0 | awk -F '/' '{for(i=1; i<NF; i++) printf "%s/", $i} END {print ""}'`
source $SHL_DIR/log.sh || exit 83

LOG_I $0 "executing"
if [ $# -eq 2 ];then
	if [ -f $1 ];then
		DTSFILE=`echo $2 | awk '{split($0,token,".");print token[1]}'`
		DTSFILE=$DTSFILE.dts
		gcc -I $PWD -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp -o $DTSFILE $1
		check $? "dts processor error!" "DTS file $DTSFILE created!"
		#if [ $? -ne 0 ];then
		#	echo -e "dts processor error!"
		#	exit 83
		#else
		#	echo -e "file $DTSFILE created!"
		#fi
		source $SHL_DIR/patch.sh $DTSFILE
		dtc -I dts -O dtb -o $2 $DTSFILE
		check $? "DTB complier error!" "DTB file $2 created!"
		#if [ $? -ne 0 ];then
		#	echo -e "DTB complier error!"
		#else
		#	echo -e "file $2 created!"
		#fi
		exit 0
	else
		LOG_E "Input file $1 not exits!"
		#echo -e "Input file $1 not exits!"
		exit 83
	fi
else
	LOG_E "Need two parameters, input file and output file!"
	#echo -e "Need two parameters, input file and output file!"
	exit 83
fi
