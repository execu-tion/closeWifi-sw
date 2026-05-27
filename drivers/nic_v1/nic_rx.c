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

int rx_queue_init(struct nic* nic){
    BUILD_BUG_ON(NIC_RX_QUEUE_STOP>NIC_RX_QUEUE_WARN);
    BUILD_BUG_ON(NIC_RX_QUEUE_WARN>NIC_RX_QUEUE_LEN );

    struct rx_queue* rxq = &nic->rx_queue;
    // init tx queue 
    spin_lock_init(&rxq->lock);

    rxq->free_nums  = NIC_RX_QUEUE_LEN;
    rxq->get_ptr    = 0;

    // init rx items
    for(int i = 0; i < NIC_RX_QUEUE_LEN; i++ ){
        rxq->items[i].used = false;
        rxq->items[i].cookie = 0;
        rxq->items[i].cb_data = nic;
        rxq->items[i].data_len = NIC_MAX_MTU;

        rxq->items[i].submit_timestamp = 0;
        rxq->items[i].completion_timestamp = 0;

        rxq->items[i].buff = dma_alloc_coherent(nic->device,
            rxq->items[i].data_len, &rxq->items[i].dma_addr, GFP_KERNEL);

        if ( !rxq->items[i].buff ) {
            // when alloc error
            for(i = i - 1; i >= 0; i-- ){
                if (rxq->items[i].buff) {
                    dma_free_coherent(nic->device, rxq->items[i].data_len,
                                      rxq->items[i].buff, rxq->items[i].dma_addr);
                    rxq->items[i].buff = NULL;
                    rxq->items[i].dma_addr = 0;
                }
            }
            return -ENOMEM;
        }
    }

    return 0;
};

/**
 * @brief 清理RX队列及相关资源
 * 
 * @param nic NIC结构体指针
 * 
 * 这个函数会安全地清理整个RX队列，包括：
 * 1. 终止所有活动的DMA传输
 * 2. 释放DMA缓冲区
 * 3. 取消映射SKB
 * 4. 释放未完成的SKB
 * 5. 重置队列状态
 * 
 * 注意：调用前应先停止网络接口, 停止dma引擎,然后这里将会
 *      清除所有挂起的RX请求
 */

void rx_queue_exit(struct nic* nic){
    struct rx_queue *rxq = &nic->rx_queue;
    int i;
    unsigned long flags;
    int pending_count = 0;
    
    if (!nic || !rxq) {
        pr_err("Invalid NIC or RX queue pointer\n");
        return ;
    }
    
    pr_info("Cleaning up RX queue for device %s\n", 
           nic->ndev ? nic->ndev->name : "unknown");
        

    spin_lock_irqsave(&rxq->lock, flags);
    for (i = 0; i < NIC_RX_QUEUE_LEN; i++) {
        if (rxq->items[i].used) {
            pending_count++;
        }
        rxq->items[i].used = true;  // mark as true, avoid counting again
    }
    rxq->get_ptr = 0;
    rxq->free_nums = 0; // note all buffers will be freed below
    spin_unlock_irqrestore(&rxq->lock, flags);

    if (pending_count > 0) {
        pr_warn("Cleaning RX queue with %d pending transmissions\n", 
               pending_count);
    }
    
    // free dma buffers, the dma engine is already stopped before calling this function.
    for (i = 0; i < NIC_RX_QUEUE_LEN; i++) {
        struct rx_request *rxr = &rxq->items[i];
        pr_debug("rx_queue_exit: idx=%d used=%d buff=%p dma_addr=%p cookie=%u, used=%d\n",
                 i, rxr->used, rxr->buff, &rxr->dma_addr, (unsigned)rxr->cookie,rxr->used);

        if (rxr->buff) {
            dma_free_coherent(nic->device, NIC_MAX_MTU,rxr->buff, rxr->dma_addr);
            rxr->buff = NULL;
            rxr->dma_addr = 0;
        }

        /* reset request state now that we've snapshot it */
        rxr->used = false;
        rxr->cookie = 0;
        rxr->cb_data = NULL;
        rxr->submit_timestamp = 0;
        rxr->completion_timestamp = 0;
    }
    rxq->get_ptr = 0;
    rxq->free_nums = NIC_RX_QUEUE_LEN;
    
    pr_info("RX queue cleanup completed\n");
    return;
};

inline int get_rx_request(struct nic* nic, struct rx_request **rxr_out){
    struct rx_queue* rxq = &nic->rx_queue;

    int base = rxq->get_ptr;
    for (int i = 0; i < NIC_RX_QUEUE_LEN; i++) {
        int idx = NIC_WRAP(base, i);
        if (rxq->items[idx].used)
            continue;

        unsigned long flags;
        spin_lock_irqsave(&rxq->lock, flags);
        if (rxq->items[idx].used) {
            spin_unlock_irqrestore(&rxq->lock, flags);
            continue;
        }
        rxq->items[idx].used = true;
        rxq->free_nums--;
        rxq->get_ptr = idx;
        spin_unlock_irqrestore(&rxq->lock, flags);

        *rxr_out = &rxq->items[idx];
        return rxq->free_nums;
    }

    *rxr_out = NULL;
    return NIC_RX_QUEUE_STOP;
}

inline int release_rx_request(struct nic* nic,struct rx_request* rxr){
    struct rx_queue* rxq = &nic->rx_queue;

    unsigned long flags;
    spin_lock_irqsave(&rxq->lock,flags);
    rxr->used = false;
    rxq->free_nums++;
    spin_unlock_irqrestore(&rxq->lock,flags);
    return rxq->free_nums;
};

// map single dma or map dma_sg
void nic_prepare_rx(struct nic* nic, struct rx_request* rxr){
    int ret = -1;

    ret = nic_dma_prep_rx(nic,rxr);
    if ( ret != 0 ){
        goto clean_dma_prep;
    }
        
	return ;

clean_dma_prep:
    nic->ndev->stats.rx_dropped++;
    release_rx_request(nic,rxr);
}