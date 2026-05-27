obj-m += acquisition.o
KDIR := ~/linux
#~/adilinux/linux-adi/

all:
	make -C $(KDIR)  M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
	@rm -f  *.o   *.mod.o   *.mod.c  *.symvers *.markers *.unsigned *.order *~
clean:
	make -C $(KDIR)  M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules clean
	rm -f  *.o   *.mod.o   *.mod.c  *.symvers *.markers *.unsigned *.order *~
