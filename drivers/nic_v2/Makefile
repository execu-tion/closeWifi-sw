obj-m += nw.o
nw-objs := nic.o nic_tx.o nic_rx.o nic_dma.o nic_hardware.o nic_ethtool.o
KDIR := ~/linux

all:
	make -C $(KDIR)  M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
	@rm -f  *.o   *.mod.o   *.mod.c  *.symvers *.markers *.unsigned *.order *~
clean:
	make -C $(KDIR)  M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules clean
	rm -f  *.o   *.mod.o   *.mod.c  *.symvers *.markers *.unsigned *.order *~
