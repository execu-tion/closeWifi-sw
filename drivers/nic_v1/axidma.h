/**
 * @file axidma.h
 * @date Saturday, November 14, 2015 at 01:41:02 PM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This file contains the internal definitions and structures for AXI DMA module
 *
 * @bug No known bugs.
 **/

#ifndef AXIDMA_H_
#define AXIDMA_H_

// Kernel dependencies
#include <linux/list.h>         // Linked list definitions and functions
#include <linux/kernel.h>           // Contains the definition for printk
#include <linux/device.h>           // Definitions for class and device structs
#include <linux/cdev.h>             // Definitions for character device structs
#include <linux/signal.h>           // Definition of signal numbers
#include <linux/dmaengine.h>        // Definitions for DMA structures and types
#include <linux/platform_device.h>  // Defintions for a platform device
#include <linux/of.h>
#include <linux/of_device.h>

#include <linux/dma-mapping.h>
#include <linux/spinlock.h>

/*----------------------------------------------------------------------------
 * Module Definitions
 *----------------------------------------------------------------------------*/

#define MODULE_NAME                 "opnet_dma"
// dma buffer pool definitions
#define OPNET_DMA_BUF_NUM  6
#define OPNET_DMA_BUF_SIZE 2048

// Truncates the full __FILE__ path, only displaying the basename
#define __FILENAME__ \
    (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

// Convenient macros for printing out messages to the kernel log buffer
#define axidma_err(fmt, ...) \
    printk(KERN_ERR MODULE_NAME ": %s: %s: %d: " fmt, __FILENAME__, __func__, \
           __LINE__, ## __VA_ARGS__)
#define axidma_info(fmt, ...) \
    printk(KERN_INFO MODULE_NAME ": %s: %s: %d: " fmt, __FILENAME__, __func__, \
            __LINE__, ## __VA_ARGS__)

// Forward declaration of the callback data structure for DMA
// The data to pass to the DMA transfer completion callback function
struct axidma_cb_data {
    int channel_id;                 // The id of the channel used
    int notify_signal;              // For async, signal to send
    struct task_struct *process;    // The process to send the signal to
    struct completion *comp;        // For sync, the notification to kernel
};

struct axidma_buffer{
    int     channel_id;                 // The id of the DMA channel to use
    void*   buf;                        // The buffer used for the transaction
    size_t  buf_len;                    // The length of the buffer
    bool    wait;                       // Indicates if the call is blocking
    
    void *user_addr;
    dma_addr_t dma_addr;
    struct list_head list;
};

struct axidma_dma_allocation {
    size_t size;                // Size of the buffer
    void *user_addr;            // User virtual address of the buffer
    void *kern_addr;            // Kernel virtual address of the buffer
    dma_addr_t dma_addr;        // DMA bus address of the buffer
    struct list_head list;      // List node pointers for allocation list
};

// Direction from the persepctive of the processor
/**
 * Enumeration for direction in a DMA transfer.
 *
 * The enumeration has two directions: write is from the processor to the
 * FPGA, and read is from the FPGA to the processor.
 **/
enum axidma_dir {
    AXIDMA_WRITE,                   ///< Transmits from memory to a device.
    AXIDMA_READ                     ///< Transmits from a device to memory.
};

/**
 * Enumeration for the type of a DMA channel.
 *
 * There are two types of channels, the standard DMA channel, and the special
 * video DMA (VDMA) channel. The VDMA channel is for transferring frame buffers
 * and other display related data.
 **/
enum axidma_type {
    AXIDMA_DMA,                     ///< Standard AXI DMA engine
    AXIDMA_NONE                     ///< Specialized AXI video DMA enginge
};

// TODO: Channel really should not be here
struct axidma_chan {
    enum axidma_dir dir;            // The DMA direction of the channel
    enum axidma_type type;          // The DMA type of the channel
    int channel_id;                 // The identifier for the device
    const char *name;               // Name of the channel (ignore)
    struct dma_chan *chan;          // The DMA channel (ignore)
};

struct axidma_num_channels {
    int num_channels;               // Total DMA channels in the system
    int num_dma_tx_channels;        // DMA transmit channels available
    int num_dma_rx_channels;        // DMA receive channels available
};

struct axidma_channel_info {
    struct axidma_chan *channels;   // Metadata about all available channels
};

struct axidma_register_buffer {
    int fd;                         // Anonymous file descriptor for DMA buffer
    size_t size;                    // The size of the external DMA buffer
    void *user_addr;                // User virtual address of the buffer
};

struct axidma_transaction {
    bool wait;                      // Indicates if the call is blocking
    int channel_id;                 // The id of the DMA channel to use
    void *buf;                      // The buffer used for the transaction
    size_t buf_len;                 // The length of the buffer
};


// All of the meta-data needed for an axidma device
struct axidma_device {
    int num_devices;                // The number of devices
    dev_t dev_num;                  // The device number of the device
    struct device *device;          // Device structure for the char device

    int num_dma_tx_chans;           // The number of transmit DMA channels
    int num_dma_rx_chans;           // The number of receive DMA channels
    int num_chans;                  // The total number of DMA channels

    int notify_signal;              // Signal used to notify transfer completion
    struct platform_device *pdev;   // The platofrm device from the device tree
    struct axidma_cb_data *cb_data; // The callback data for each channel
    struct axidma_chan *channels;   // All available channels
    struct list_head dmabuf_list;   // List of allocated DMA buffers

    // add
    // dma pool related members
    spinlock_t pool_lock;        // Spinlock for protecting pool access
    int free_cnt;                // Number of free TX buffers
};

/*----------------------------------------------------------------------------
 * DMA Device Definitions
 *----------------------------------------------------------------------------*/

// Checks that the given integer is a valid notification signal for DMA
#define VALID_NOTIFY_SIGNAL(signal) \
    (SIGRTMIN <= (signal) && (signal) <= SIGRTMAX)

// Function Prototypes
int axidma_dma_init(struct platform_device *pdev, struct axidma_device *dev);
void axidma_dma_exit(struct axidma_device *dev);
void axidma_get_num_channels(struct axidma_device *dev,
                             struct axidma_num_channels *num_chans);
void axidma_get_channel_info(struct axidma_device *dev,
                             struct axidma_channel_info *chan_info);
int axidma_set_signal(struct axidma_device *dev, int signal);
int axidma_read_transfer(struct axidma_device *dev,
                          struct axidma_buffer *trans);
int axidma_write_transfer(struct axidma_device *dev,
                          struct axidma_buffer *trans);
int axidma_write_transfer_sg(struct axidma_device *dev,
                          int channel_id,void *buf);
int axidma_stop_channel(struct axidma_device *dev, struct axidma_chan *chan);

/*----------------------------------------------------------------------------
 * Device Tree Definitions
 *----------------------------------------------------------------------------*/

// Macro for printing out an error message related to a device tree node
#define axidma_node_err(node, fmt, ...) \
    axidma_err("Device tree node %s: " fmt, node->name, ##__VA_ARGS__)

// Function Prototypes
int axidma_of_num_channels(struct platform_device *pdev);
int axidma_of_parse_dma_nodes(struct platform_device *pdev,
                              struct axidma_device *dev);

// dma pool operations
int opnet_dma_pool_init(struct axidma_device *dev);
struct axidma_buffer *axidma_pool_get_buf(struct axidma_device *dev);
void axidma_pool_put_buf(struct axidma_device *dev,
                                  struct axidma_buffer *buf);
inline bool axidma_buf_available(struct axidma_device *dev);
void opnet_dma_pool_exit(struct axidma_device *dev);

#endif /* AXIDMA_H_ */
