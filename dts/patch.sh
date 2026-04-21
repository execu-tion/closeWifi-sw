#!/usr/bin/bash

if [[ -f $1 && -e $1 ]];then
	# patch for gem0
	line=`grep -n "&gem0 {" $1 | awk -F: '{print $1}'`
	if [ $? -ne 0 ];then
		LOG_E "gem node not found!"
	else
		sed -i "$(( $line + 1 ))"'s/.*//'  $1
	fi
	sed -i 's/phy1/pl_phy/g' $1
fi

if [[ -f $2 && -e $2 ]];then
	#rev for system-top.dts append
	sed -i '12i #include "fmcommos3.dtsi"' $2
	sed -i '12i #include "user.dtsi"' $2
fi

if [[ -f $3 && -e $3 ]];then
	sed -i 's/xlnx,axi-dmac-1.0/adi,axi-dmac-1.00.a/g' $3
	sed -i 's/interrupts = <0 31 4>;/interrupts = <0 57 4>;/g' $3
	sed -i 's/interrupts = <0 32 4>;/interrupts = <0 56 4>;/g' $3
	sed -i 's/interrupts = <0 33 4>;/interrupts = <0 55 4>;/g' $3
	sed -i 's/interrupts = <0 34 4>;/interrupts = <0 54 4>;/g' $3
	# compensation for channel id
	line=`grep -n "xlnx,axi-dma-mm2s-channel" $3 | awk -F: '{print $1}'`
	if [ $? -ne 0 ];then
		LOG_E "xlnx,axi-dma-mm2s-channel not found!"
	else
		sed -i "$(( $line + 4 ))"'c xlnx,device-id = <0x1>;' $3
	fi
	
	line=`grep -n "xlnx,axi-dma-s2mm-channel" $3 | awk -F: '{print $1}'`
	if [ $? -ne 0 ];then
		LOG_E "xlnx,axi-dma-s2mm-channel not found!"
	else
		sed -i "$(( $line + 4 ))"'c xlnx,device-id = <0x2>;' $3
	fi
	# compensation for add chrdev
	line=`grep -n "axi_dma_msg: dma@40400000 {" $3 | awk -F: '{print $1}'`
	if [ $? -ne 0 ];then
		LOG_E "xlnx dma not found!"
	else
		sed -i "$(( $line - 1 ))"'i '"};\n"'axidma_chrdev: axidma_chrdev@0{' $3
		sed -i "$(( $line + 1 ))"'i compatible = "xlnx,axidma-chrdev";' $3
		sed -i "$(( $line + 2 ))"'i dmas = <&axi_dma_msg 0 &axi_dma_msg 1>;' $3
		sed -i "$(( $line + 3 ))"'i dma-names = "tx_channel","rx_channel";' $3
	fi
fi

if [[ -f $4 && -e $4 ]];then
	# patch cpu frequency
	echo -e "\n\n\n"
	echo $4 $line
	line=`grep -n "kHz    uV" $4 | awk -F: '{print $1}'`
	if [ $? -ne 0 ];then
		LOG_E "cpu not found!"
	else
		sed -i "$line"'i 800000 10000000' $4
	fi

fi


