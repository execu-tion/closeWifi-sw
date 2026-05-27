#include "axidma.h"

int opnet_dma_pool_init(struct axidma_device *dev)
{
    int i;
    struct axidma_buffer *buf;
    spin_lock_init(&dev->pool_lock);

    INIT_LIST_HEAD(&dev->dmabuf_list);

    for (i = 0; i < OPNET_DMA_BUF_NUM; i++) {
        buf = devm_kmalloc(&dev->pdev->dev, sizeof(*buf), GFP_KERNEL);

        if (!buf)
            return -ENOMEM;

        buf->buf_len = OPNET_DMA_BUF_SIZE;
        buf->user_addr = dma_alloc_coherent(dev->device,
                            OPNET_DMA_BUF_SIZE, &buf->dma_addr, GFP_KERNEL);
        

        if ( !buf->user_addr ) {
            kfree(buf);
            return -ENOMEM;
        }
        INIT_LIST_HEAD(&buf->list);
        list_add_tail(&buf->list, &dev->dmabuf_list);
    }
    
    dev->free_cnt = OPNET_DMA_BUF_NUM;
  
    return 0;
}


struct axidma_buffer *axidma_pool_get_buf(struct axidma_device *dev)
{
    struct axidma_buffer *buf;
    unsigned long flags;

    spin_lock_irqsave(&dev->pool_lock,flags);
    if (list_empty(&dev->dmabuf_list) || dev->free_cnt <= 0)
        return NULL;

    buf = list_first_entry(&dev->dmabuf_list,
                           struct axidma_buffer, list);

    list_del(&buf->list);
    dev->free_cnt--;
    spin_unlock_irqrestore(&dev->pool_lock,flags);

    return buf;
}

void axidma_pool_put_buf(struct axidma_device *dev,
                                  struct axidma_buffer *buf)
{
    if( buf == NULL )
        return;
    unsigned long flags;
    spin_lock_irqsave(&dev->pool_lock, flags);
    list_add_tail(&buf->list, &dev->dmabuf_list);
    dev->free_cnt++;
    spin_unlock_irqrestore(&dev->pool_lock, flags);
}

inline bool axidma_buf_available(struct axidma_device *dev)
{
    return dev->free_cnt > 0;
}

void opnet_dma_pool_exit(struct axidma_device *dev)
{
    struct axidma_buffer *buf, *tmp;

    unsigned long flags;
    spin_lock_irqsave(&dev->pool_lock, flags);
    dev->free_cnt = 0; // bug: may not free all buffers, prevent get_buf after exit
    spin_unlock_irqrestore(&dev->pool_lock, flags);

    list_for_each_entry_safe(buf, tmp, &dev->dmabuf_list, list) {

        // remove list node
        list_del(&buf->list);

        // free dma buffer
        if (buf->user_addr)
            dma_free_coherent(dev->device, OPNET_DMA_BUF_SIZE,
                              buf->user_addr, buf->dma_addr);

        devm_kfree(&dev->pdev->dev, buf);
    }

    INIT_LIST_HEAD(&dev->dmabuf_list);
    dev->free_cnt = 0; 
}

