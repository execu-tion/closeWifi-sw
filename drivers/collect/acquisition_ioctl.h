#ifndef __ACQUISITION_IOCTL_H__
#define __ACQUISITION_IOCTL_H__

// 注意：当前驱动不提供read阻塞读取，必须使用下列ioctl函数进行交互
//
// valid data type
enum acquisition_type {
    TYPE_DATA = 0,
    TYPE_CONST,
	// TYPE_SNR, current not avaliable
    TYPE_FREQ,
    TYPE_CHAN,      // new for channel information
    TYPE_MAX
};

//const char* types[] = {"DATA","CONST","SNR","FREQ","CHAN"};
const char* types[] = {"DATA","CONST","FREQ","CHAN"};
#define IOC_MAGIC 'k'

// no effect and do not use
#define IOCTL_RESET             _IO(IOC_MAGIC,0)

// set expected data type to driver or get current data type from driver when given -1
#define IOCTL_DATA_TYPE         _IOWR(IOC_MAGIC, 1, int)

// set buffer length for driver
#define IOCTL_SET_BUFFER        _IOW(IOC_MAGIC, 2, int)

// set transfer length for driver
#define IOCTL_SET_LENGTH        _IOW(IOC_MAGIC, 3, int)

// set user notifier hook
#define IOCTL_SET_SIGNO         _IOW(IOC_MAGIC, 4, int) 

// when a buffer is attached, driver will always use this buffer
// user just call an IOCTL_START_TRANSFER
#define IOCTL_START_TRANSFER    _IO(IOC_MAGIC, 5)

#define IOCTL_WAITON_TRANSFER   _IO(IOC_MAGIC, 7)
// read num bytes
#define IOCTL_GET_AVALIABLE     _IOR(IOC_MAGIC, 6, int*)


#endif
