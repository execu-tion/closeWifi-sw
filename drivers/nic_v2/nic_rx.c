#include "nic.h"
#include <linux/skbuff.h>
#include <linux/dmaengine.h>

/*
*   nic_rx.c : 
*   Implementation of Key Functions and Resource Management on Recv Paths.
*
*   create: 25-12-05
*   author: xct
*/


#define _WRAP(base,idx,len) (((base) + (idx)) & ((len) - 1))
#define NIC_WRAP(base,idx)  _WRAP(base,idx,NIC_RX_QUEUE_LEN)

static inline void nic_clear_request(struct nic* nic, struct rx_request* rxr){
    rxr->skb = NULL;
    rxr->data = NULL;
    rxr->status = detached;
    rxr->cb_data = nic;
    rxr->data_len = NIC_MAX_MTU;

    rxr->cookie = 0;
    rxr->dma_addr = 0;
    rxr->submit_timestamp = 0;
    rxr->completion_timestamp = 0;
};

static inline int nic_fill_payload(struct nic* nic, struct rx_request* rxr){
    // new skb and map for dma
    rxr->skb = netdev_alloc_skb(nic->ndev, NIC_MAX_MTU);
    if ( !rxr->skb ) return -ENOMEM;
    rxr->status    = alloced;

    // map dma request
    rxr->data     = skb_put(rxr->skb, NIC_MAX_MTU);
    rxr->dma_addr = dma_map_single(nic->device, rxr->data, NIC_MAX_MTU, DMA_FROM_DEVICE);
    
    if( dma_mapping_error(nic->device,rxr->dma_addr) ) return -1;
    rxr->status   = mapped;
    return 0;
};

static void nic_release_request(struct nic* nic, struct rx_request* rxr){
    if( rxr->status > alloced ){ // mapped, submitted
        dma_unmap_single(nic->device, rxr->dma_addr, NIC_MAX_MTU, DMA_FROM_DEVICE);
    };  

    dev_kfree_skb_any(rxr->skb);
    nic_clear_request(nic,rxr);   // none
};

int rx_queue_init(struct nic* nic){
    BUILD_BUG_ON(NIC_RX_QUEUE_STOP>NIC_RX_QUEUE_WARN);
    BUILD_BUG_ON(NIC_RX_QUEUE_WARN>NIC_RX_QUEUE_LEN );

    struct rx_queue* rxq = &nic->rx_queue;
    // init rx items
    for(int i = 0; i < NIC_RX_QUEUE_LEN; i++ ){
        // init request
        nic_clear_request(nic,&rxq->items[i]);

        // fill with new payload
        if( nic_fill_payload(nic,&rxq->items[i]) != 0 ) 
            return -EPERM;
        
        // start dma transfer
        if( nic_dma_prep_rx(nic,&rxq->items[i]) != 0 ) 
            return -EPERM;
    };
    return 0;
};

/**
 * @brief 清理RX队列及相关资源
 * 
 * @param nic NIC结构体指针
**/
void rx_queue_exit(struct nic* nic){
    struct rx_queue *rxq = &nic->rx_queue;
    if ( !nic || !rxq ) {
        pr_err("Invalid NIC or RX queue pointer\n");
        return ;
    };

    for (int i = 0; i < NIC_RX_QUEUE_LEN; i++) {
        nic_release_request(nic,&rxq->items[i]);
    };

    pr_info("RX queue cleanup completed\n");
    return;
};


void* nic_detach_payload(struct nic* nic,struct rx_request* rxr){
    struct sk_buff* skb = rxr->skb;

    if( rxr->data_len <= 2 || !skb_pull(skb,2) ){
		dev_kfree_skb_any(skb);
        netdev_err(nic->ndev, "skb detach error!\n");
        skb = NULL;
	};

    nic_clear_request(nic,rxr);
    return skb;
};

// replace the complete one with new skb
int nic_replace_request(struct nic* nic, struct rx_request* rxr){
    // init request
    nic_clear_request(nic,rxr);
    
    // fill with new payload
    if( nic_fill_payload(nic,rxr) != 0 ) 
        return -EPERM;

    // start dma transfer
    if( nic_dma_prep_rx(nic,rxr) != 0 ) 
        return -EPERM;

    return 0;
};
