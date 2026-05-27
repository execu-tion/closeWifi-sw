/*
*   nic.h : configure nic default parameters
*   
*   create: 25-12-05
*   author: xct
*
*/
#ifndef __NIC_H__
#define __NIC_H__

#include <asm/io.h>
#include <asm/irq.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/build_bug.h>

#include <linux/netdevice.h>

#include <linux/dmaengine.h>
#include <linux/dma-direction.h>
#include <linux/scatterlist.h>
#define NIC_NAME_LEN 16

#include "mac.h"
#include "phy.h"

/*  TO DO:
*   This parameter is associated with the hardware and should 
*   be the default mcs. 
*/
#define DEFAULT_NIC_MCS 8
#define NIC_MAX_MCS 12

/*
*   This parameter is associated with the hardware and should 
*   be less than the hardware DMA buffer length.
*/
#define DEFAULT_NIC_MTU 1500
#define NIC_MAX_MTU 2048    

/*
*  This parameter specifies the size of the DMA pre-allocation 
*  pool on the sending path.
*/
#define NIC_TX_QUEUE_LEN    16
#define NIC_RX_QUEUE_LEN    8
/*
*   This parameter specifies the warning level when the buffer 
*   on the sending path is insufficient.
*   NIC will transmit the skb and return an NETDEV_TX_OK 
*/
#define NIC_TX_QUEUE_WARN   6
#define NIC_RX_QUEUE_WARN   4

/*
*   This parameter specifies the size when the buffer along 
*   the sending path is severely insufficient.
*   NIC will transmit the skb and return an NETDEV_TX_OK
*   but this will cause ximt queue stop, less then STOP will
*   return NETDEV_TX_BUSY (may not cause)
*/
#define NIC_TX_QUEUE_STOP   2
#define NIC_RX_QUEUE_STOP   1

/*
*   tx_request
*
*/
typedef struct tx_request{
    dma_cookie_t cookie;          // transcation cookie 
    struct sg_table sg_table;     // transcation sg table
    u32     sg_len;               // transcation sg len

    bool    used;
    void*   payload;              // transcation payload, always skb.
    void*   cb_data;              // call back data, binding the device
    u64     submit_timestamp;     // transcation submit timestamp
    u64     completion_timestamp; // transcation completion timestamp
} tx_request;

/*
*   rx_request
*
*/
typedef enum {none,alloced, mapped, submitted,detached} request_status;
typedef struct rx_request{
    // recv the mac frame
    void*       skb ;
    char*       data;
    dma_addr_t  dma_addr;
    u32         data_len;

    // recv the phy status
    dma_cookie_t cookie;          // transcation cookie

    request_status status;
    void*   cb_data;              // call back data, binding the device
    u64     submit_timestamp;     // transcation submit timestamp
    u64     completion_timestamp; // transcation completion timestamp
} rx_request;


/*
*   tx_queue will manage the prealloced tx dma descriptor and 
*   provide external interface
*
*/
typedef struct tx_queue {
    spinlock_t lock;
    tx_request items[NIC_TX_QUEUE_LEN];

    // 0 is set as normal, 1 as high water level warning, and 2 as emergency stop.
    int     get_ptr;
    int     free_nums;
} tx_queue;

/*
*   rx_queue will manage the prealloced tx dma buffers and 
*   provide external interface
*
*/
typedef struct rx_queue {
    rx_request items[NIC_RX_QUEUE_LEN];
} rx_queue;

struct nic{
    struct platform_device *pdev; // for dynamic binding
	struct device *device;
    struct net_device *ndev;

    struct tx_queue tx_queue;
    struct rx_queue rx_queue;

	struct { int num; const char *name;
	irqreturn_t (*handler)(int, void *); } irq_desc;

    struct {
        int channel_id;                 // The identifier for the device
        const char *name;               // Name of the channel (ignore)
        struct dma_chan *chan;          // The DMA channel (ignore)
    } dma_tx_chan,dma_rx_chan;

    void __iomem* phy_ctrl_base;
    void __iomem* mac_ctrl_base;
};

// nic.c
irqreturn_t nic_irq_handler(int, void *);
void nic_tx_complete(struct tx_request *);
void nic_rx_complete(struct rx_request *);

// nic_tx.c
int  tx_queue_init(struct nic*);
void tx_queue_exit(struct nic*);

/*
 * get_tx_request: returns remaining free count and sets *txr to the
 * allocated request on success. Caller must pass address of a
 * (struct tx_request *) variable.
 */
inline int get_tx_request(struct nic*, struct tx_request **);
inline int release_tx_request(struct nic*,struct tx_request*);
void nic_prepare_tx(struct nic*, struct tx_request*);

// nic_rx.c
int  rx_queue_init(struct nic*);
void rx_queue_exit(struct nic*);
int nic_replace_request(struct nic*,struct rx_request*);

/*
 * get_rx_request: returns remaining free count and sets *rxr to the
 * allocated request on success. Caller must pass address of a
 * (struct rx_request *) variable.
 */
inline int get_rx_request(struct nic*, struct rx_request **);
inline int release_rx_request(struct nic*,struct rx_request*);
void* nic_detach_payload(struct nic*, struct rx_request*);
void nic_prepare_rx(struct rx_request*);

// nic_dma.c
#include <linux/of.h>               // Device tree parsing functions
#include <linux/platform_device.h>  // Platform device definitions
int nic_dma_init(struct nic*);
void nic_dma_exit(struct nic*);
int nic_dma_prep_tx(struct nic*,struct tx_request*);
int nic_dma_prep_rx(struct nic*,struct rx_request*);


// nic_hardward.c
int nic_get_resources(struct nic*);
void nic_free_resources(struct nic*);
inline void nic_reset_hardware(struct nic*);
inline void nic_set_hardware_mac(struct nic*, void*);
inline void nic_get_hardware_mac(struct nic*, void*);
#endif