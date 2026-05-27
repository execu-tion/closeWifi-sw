#include "nic.h"
#include <linux/skbuff.h>

/*
*   nic_tx.c : 
*   Implementation of Key Functions and Resource Management on Sending Paths.
*
*   create: 25-12-05
*   author: xct
*/


#define _WRAP(base,idx,len) (((base) + (idx)) & ((len) - 1))
#define NIC_WRAP(base,idx)  _WRAP(base,idx,NIC_TX_QUEUE_LEN)

int tx_queue_init(struct nic* nic){
    BUILD_BUG_ON(NIC_TX_QUEUE_STOP>NIC_TX_QUEUE_WARN);
    BUILD_BUG_ON(NIC_TX_QUEUE_WARN>NIC_TX_QUEUE_LEN );

    struct tx_queue* txq = &nic->tx_queue;
    // init tx queue 
    spin_lock_init(&txq->lock);

    txq->free_nums  = NIC_TX_QUEUE_LEN;
    txq->get_ptr    = 0;

    // init tx items
    for(int i = 0; i < NIC_TX_QUEUE_LEN; i++ ){
        txq->items[i].used = false;
        txq->items[i].cb_data = nic;
        txq->items[i].payload = NULL;
        txq->items[i].cookie = 0;
        txq->items[i].submit_timestamp = 0;
        txq->items[i].completion_timestamp = 0;
    }

    return 0;
};

/**
 * @brief 清理TX队列及相关资源
 * 
 * @param nic NIC结构体指针
 * 
 * 这个函数会安全地清理整个TX队列，包括：
 * 1. 终止所有活动的DMA传输
 * 2. 释放DMA缓冲区
 * 3. 取消映射SKB
 * 4. 释放未完成的SKB
 * 5. 重置队列状态
 * 
 * 注意：调用前应先停止网络接口
 */

void tx_queue_exit(struct nic* nic){
    struct tx_queue *txq = &nic->tx_queue;
    int i;
    unsigned long flags;
    int pending_count = 0;
    
    if (!nic || !txq) {
        pr_err("Invalid NIC or TX queue pointer\n");
        return ;
    }
    
    pr_info("Cleaning up TX queue for device %s\n", 
           nic->ndev ? nic->ndev->name : "unknown");
        

    spin_lock_irqsave(&txq->lock, flags);
    for (i = 0; i < NIC_TX_QUEUE_LEN; i++) {
        if (txq->items[i].used) {
            pending_count++;
        }
    }
    
    if (pending_count > 0) {
        pr_warn("Cleaning TX queue with %d pending transmissions\n", 
               pending_count);
    }
    
    for (i = 0; i < NIC_TX_QUEUE_LEN; i++) {
        struct tx_request *txr = &txq->items[i];
        
        if (!txr->used) {
            continue;
        }
        
        pr_debug("Cleaning pending TX item %d:\n", i);
        pr_debug("  payload=%p, submit_time=%llu\n",
                txr->payload, txr->submit_timestamp);
        
        if (txr->payload && txr->sg_table.sgl) {
            dma_unmap_sg(nic->device, txr->sg_table.sgl, txr->sg_table.orig_nents, DMA_TO_DEVICE);
        }
        
        if (txr->payload) {
            dev_kfree_skb_any((struct sk_buff *)txr->payload);
            pr_debug("  Freed SKB %p\n", txr->payload);
        }
        
        memset(txr, 0, sizeof(*txr));
        txr->cb_data = NULL;
    }

    txq->get_ptr = 0;
    txq->free_nums = NIC_TX_QUEUE_LEN;
    
    spin_unlock_irqrestore(&txq->lock, flags);

    spin_lock_irqsave(&txq->lock, flags);
    for (i = 0; i < NIC_TX_QUEUE_LEN; i++) {
        if (txq->items[i].used /*|| txq->items[i].payload*/) {
            pr_err("TX item %d not properly cleaned!\n", i);
        }
    }
    spin_unlock_irqrestore(&txq->lock, flags);
    
    pr_info("TX queue cleanup completed\n");
    return;
};

inline int get_tx_request(struct nic* nic, struct tx_request **txr_out){
    struct tx_queue* txq = &nic->tx_queue;

    int base = txq->get_ptr;
    for (int i = 0; i < NIC_TX_QUEUE_LEN; i++) {
        int idx = NIC_WRAP(base, i);
        /* find a free tx request */
        if (txq->items[idx].used)
            continue;

        unsigned long flags;
        spin_lock_irqsave(&txq->lock, flags);
        /* double-check under lock */
        if (txq->items[idx].used) {
            spin_unlock_irqrestore(&txq->lock, flags);
            continue;
        }
        txq->items[idx].used = true;
        txq->free_nums--;
        txq->get_ptr = idx;
        spin_unlock_irqrestore(&txq->lock, flags);

        *txr_out = &txq->items[idx];
        return txq->free_nums;
    }
    /* no free entries */
    *txr_out = NULL;
    return NIC_TX_QUEUE_STOP;
}

inline int release_tx_request(struct nic* nic,struct tx_request* txr){
    struct tx_queue* txq = &nic->tx_queue;

    unsigned long flags;
    spin_lock_irqsave(&txq->lock,flags);
    txr->used = false;
    txr->sg_table.sgl = NULL;
    txq->free_nums++;
    spin_unlock_irqrestore(&txq->lock,flags);
    return txq->free_nums;
};

// map single dma or map dma_sg
void nic_prepare_tx(struct nic* nic, struct tx_request* txr){
    struct sk_buff *skb = txr->payload;
    int ret = -1;
    // start with IP header
	skb->protocol = htons(ETH_P_IP);
	if( skb->len > NIC_MAX_MTU ) // invalid skb
		goto clean_up_skb;
	

    // transmit main skb
    struct skb_shared_info *shinfo = skb_shinfo(skb);
    int linear = skb->len - skb->data_len;
    int sglen = shinfo->nr_frags + ( linear > 0 );

    ret = sg_alloc_table(&txr->sg_table, sglen, GFP_KERNEL);
    if ( ret != 0 )
        goto clean_up_skb;
    
    struct scatterlist *sg = txr->sg_table.sgl;
    // map linear skb
    if( linear > 0 ){
        struct page *linear_page = virt_to_page(skb->data);
        unsigned int linear_offset = offset_in_page(skb->data);
        sg_set_page(sg, linear_page, linear, linear_offset);
        sg = sg_next(sg);
    }

    // map nonlinear skb
    if( skb->data_len > 0 ){
        for (int i = 0; i < shinfo->nr_frags; i++) {
            skb_frag_t *frag = &shinfo->frags[i];
            // got frags info
            struct page *page = skb_frag_page(frag);
            unsigned int offset = skb_frag_off(frag);
            unsigned int size = skb_frag_size(frag);
            sg_set_page(sg,page,size,offset);
            sg = sg_next(sg);
        }
    }

    // map dma addr
    txr->sg_len = dma_map_sg(nic->device, txr->sg_table.sgl, txr->sg_table.orig_nents, DMA_TO_DEVICE);
    if ( txr->sg_len == 0 ) // mapped as 0
        goto free_sg;

    ret = nic_dma_prep_tx(nic,txr);
    if ( ret != 0 ){
        goto clean_up_sg;
    }
        
	// update counts in cbas
	//nic->ndev->stats.tx_packets++;
	//nic->ndev->stats.tx_bytes += skb->len;
	return ;

clean_up_sg:
    dma_unmap_sg(nic->device, txr->sg_table.sgl, txr->sg_table.orig_nents, DMA_TO_DEVICE);
free_sg:
    sg_free_table(&txr->sg_table);
clean_up_skb:
    nic->ndev->stats.tx_dropped ++;
    dev_kfree_skb(skb);
    release_tx_request(nic,txr);
}
