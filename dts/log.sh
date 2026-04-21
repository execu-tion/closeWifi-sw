#!/usr/bin/bash
#########################################
#       process log tools
#	create : 25-04-17 dfy
#  rev0: file created
#########################################
function LOG_E(){
	HEAD="[ \033[1;31mERROR\033[0m ]"
	echo -e $HEAD $@
}
export -f LOG_E
function LOG_S(){
	HEAD="[ \033[1;32mSUCS  \033[0m ]"
	echo -e $HEAD $@
}
export -f LOG_S
function LOG_W(){
	HEAD="[ \033[1;33mWARN \033[0m ]"	
	echo -e $HEAD $@
}
export -f LOG_W
function LOG_I(){
	HEAD="[ \033[1;34mINFO \033[0m ]"
	echo -e $HEAD $@
}
export -f LOG_I
function check(){
	if [ $# -ne 0 ];then
		if [ $1 -ne 0 ];then
			shift
			LOG_E $1
			return 83
		else
			shift
			shift
			LOG_S $1
			return 0
		fi
	fi
}
export -f check
function check_exists(){
	test $1 $2
	if [ $? -ne 0 ];then
		LOG_S "$2 found!"
		return 0
	else
		LOG_E "$2 not exists!"
		return 83
	fi
}
export -f check_exists
