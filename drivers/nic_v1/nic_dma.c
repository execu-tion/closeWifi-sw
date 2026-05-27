#include "nic.h"
#include <linux/dmaengine.h> 
#include <linux/dma-mapping.h>

static int dma_num_channels(struct platform_device *pdev)
{
    int num_dmas, num_dma_names;
    struct device_node *driver_node;

    // Get the device tree node for the driver
    driver_node = pdev->dev.of_node;

    // Check that the device tree node has the 'dmas' and 'dma-names' properties
    if (of_find_property(driver_node, "dma-names", NULL) == NULL) {
        pr_err("Property 'dma-names' is missing.\n");
        return -EINVAL;
    } else if (of_find_property(driver_node, "dmas", NULL) == NULL) {
        pr_err("Property 'dmas' is missing.\n");
        return -EINVAL;
    }

    // Get the length of the properties, and make sure they are not empty
    num_dma_names = of_property_count_strings(driver_node, "dma-names");
    if (num_dma_names < 0) {
        pr_err("Unable to get the 'dma-names' property "
                        "length.\n");
        return -EINVAL;
    } else if (num_dma_names == 0) {
        pr_err("'dma-names' property is empty.\n");
        return -EINVAL;
    }
    num_dmas = of_count_phandle_with_args(driver_node, "dmas", "#dma-cells");
    if (num_dmas < 0) {
        pr_err("Unable to get the 'dmas' property length.\n");
        return -EINVAL;
    } else if (num_dmas == 0) {
        pr_err("'dmas' property is empty.\n");
        return -EINVAL;
    }

    // Check that the number of entries in each property matches
    if (num_dma_names != num_dmas) {
        pr_err("Length of 'dma-names' and 'dmas' properties differ.\n");
        return -EINVAL;
    }

    return num_dma_names;
}

static int dma_parse_channel(struct nic* nic) {
    struct of_phandle_args phandle_args = {0};
    struct device_node *driver_node, *dma_node, *dma_chan_node = NULL;
    int ret = 0;
    int i;
    int num_channels;
    int tx_found = 0, rx_found = 0;
    
    num_channels = dma_num_channels(nic->pdev);
    if (num_channels <= 0) {
        return num_channels;
    }
    
    driver_node = nic->device->of_node;
    
    for(i = 0; i < num_channels; i++) {
        ret = of_parse_phandle_with_args(driver_node, "dmas", "#dma-cells", i,
                                        &phandle_args);
        if (ret < 0) {
            dev_err(nic->device, "Unable to get phandle %d\n", i);
            ret = -EINVAL;
            goto cleanup_phandle;
        }
        dma_node = phandle_args.np;
        
        // parse channel args
        if (phandle_args.args_count < 1) {
            dev_err(nic->device, "Missing channel direction for phandle %d\n", i);
            ret = -EINVAL;
            goto cleanup_phandle;
        }
        
        int channel = phandle_args.args[0];
        if (channel != 0 && channel != 1) {
            dev_err(nic->device, "Invalid channel %d for phandle %d\n", channel, i);
            ret = -EINVAL;
            goto cleanup_phandle;
        }
        
        /*
         * Find the child node index 'channel' of the dma_node in a safe
         * manner. of_get_next_child() returns a node with an incremented
         * refcount, so keep track and only call of_node_put() once for the
         * node we actually use. This avoids double-put / use-after-free
         * situations observed when manipulating children directly.
         */
        {
            struct device_node *child = NULL;
            int idx = 0;

            dma_chan_node = NULL;
            for (child = of_get_next_child(dma_node, NULL); child;
                 child = of_get_next_child(dma_node, child)) {
                if (idx == channel) {
                    /* 'child' already has a reference from of_get_next_child */
                    dma_chan_node = child;
                    break;
                }
                idx++;
            }

            if (!dma_chan_node) {
                dev_err(nic->device, "Channel %d not found\n", channel);
                ret = -EINVAL;
                goto cleanup_phandle;
            }
        }
        
        // read channel id
        u32 channel_id;
        ret = of_property_read_u32(dma_chan_node, "xlnx,device-id", &channel_id);
        if (ret < 0) {
            dev_err(nic->device, "Failed to read device-id\n");
            goto cleanup_phandle;
        }
        
        // specific with channel type
        const char *dma_name;
        ret = of_property_read_string_index(driver_node, "dma-names", i, &dma_name);
        if (ret < 0) {
            dev_err(nic->device, "Failed to read DMA name %d\n", i);
            goto cleanup_phandle;
        }
        
        if (of_device_is_compatible(dma_chan_node, "xlnx,axi-dma-mm2s-channel")) {
            if (tx_found) {
                dev_err(nic->device, "Multiple TX channels found\n");
                ret = -EINVAL;
                goto cleanup_phandle;
            }
            nic->dma_tx_chan.name = dma_name;
            nic->dma_tx_chan.channel_id = channel_id;
            tx_found = 1;
        } else if (of_device_is_compatible(dma_chan_node, "xlnx,axi-dma-s2mm-channel")) {
            if (rx_found) {
                dev_err(nic->device, "Multiple RX channels found\n");
                ret = -EINVAL;
                goto cleanup_phandle;
            }
            nic->dma_rx_chan.name = dma_name;
            nic->dma_rx_chan.channel_id = channel_id;
            rx_found = 1;
        } else {
            dev_err(nic->device, "Unsupported channel type\n");
            ret = -EINVAL;
            goto cleanup_phandle;
        }
        
cleanup_phandle:
        of_node_put(dma_chan_node);
        of_node_put(phandle_args.np);
        dma_chan_node = NULL;
        memset(&phandle_args, 0, sizeof(phandle_args));
        
        if (ret < 0) {
            return ret;
        }
    }
    
    // verify channel
    if (!tx_found || !rx_found) {
        dev_err(nic->device, "Missing required DMA channels\n");
        return -EINVAL;
    }
    
    // dma request channel
    nic->dma_tx_chan.chan = dma_request_chan(nic->device, nic->dma_tx_chan.name);
    if (IS_ERR(nic->dma_tx_chan.chan)) {
        ret = PTR_ERR(nic->dma_tx_chan.chan);
        dev_err(nic->device, "Failed to request TX DMA channel\n");
        return ret;
    }
    
    nic->dma_rx_chan.chan = dma_request_chan(nic->device, nic->dma_rx_chan.name);
    if (IS_ERR(nic->dma_rx_chan.chan)) {
        ret = PTR_ERR(nic->dma_rx_chan.chan);
        dev_err(nic->device, "Failed to request RX DMA channel\n");
        dma_release_channel(nic->dma_tx_chan.chan);
        return ret;
    }
    
    return 0;
}

int nic_dma_init(struct nic* nic){
    int ret;
    u64 dma_mask = DMA_BIT_MASK(8 * sizeof(dma_addr_t));
    ret = dma_set_coherent_mask(nic->device, dma_mask);
    if (ret < 0) {
        dev_err(nic->device, "Unable to set DMA coherent mask: %d\n", ret);
        return ret;
    }
    
    ret = dma_parse_channel(nic);
    if (ret < 0) {
        dev_err(nic->device, "Failed to initialize DMA: %d\n", ret);
        return ret;
    }
    return 0;
}

void nic_dma_exit(struct nic* nic){
    dmaengine_terminate_all(nic->dma_tx_chan.chan);
    dma_release_channel(nic->dma_tx_chan.chan);

    dmaengine_terminate_all(nic->dma_rx_chan.chan);
    dma_release_channel(nic->dma_rx_chan.chan);

    nic->dma_tx_chan.name = NULL;
    nic->dma_tx_chan.chan = NULL;
    nic->dma_tx_chan.channel_id = -1;

    nic->dma_rx_chan.name = NULL;
    nic->dma_rx_chan.chan = NULL;
    nic->dma_rx_chan.channel_id = -1;
};


static void nic_dma_callback_tx(void *data)
{
    enum dma_status status;
    struct tx_request* txr = data;
    txr->completion_timestamp = get_cycles();

    // raise soft irq
    struct nic* nic = txr->cb_data;

    status = dma_async_is_tx_complete(nic->dma_tx_chan.chan, txr->cookie, NULL, NULL);
    txr->cookie = (dma_cookie_t)status;
    
    // 直接在当前的 DMA Context (通常已经是 Tasklet/SoftIRQ) 调用清理逻辑
    nic_tx_complete(txr);
}

static void nic_dma_callback_rx(void *data)
{
    enum dma_status status;
    struct rx_request* rxr = data;
    rxr->completion_timestamp = get_cycles();

    // raise soft irq
    struct nic* nic = rxr->cb_data;

    status = dma_async_is_tx_complete(nic->dma_rx_chan.chan, rxr->cookie, NULL, NULL);
    rxr->cookie = (dma_cookie_t)status;

    // 直接在当前的 DMA Context 处理收包，避免引发 Workqueue 的高昂调度延迟
    nic_rx_complete(rxr);
}

int nic_dma_prep_tx(struct nic* nic,struct tx_request* txr){
    int sg_len;
    struct dma_chan *chan;
    dma_cookie_t dma_cookie;
    struct scatterlist *sg_list;
    enum dma_ctrl_flags dma_flags;

    // got txr 
    chan = nic->dma_tx_chan.chan;
    sg_list = &txr->sg_table.sgl[0];
    sg_len = txr->sg_len;
    
    // we prepare a slave scatter-gather transfer.
    struct dma_async_tx_descriptor *dma_txnd;
    dma_flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    dma_txnd = dmaengine_prep_slave_sg(chan, sg_list, sg_len, DMA_MEM_TO_DEV, dma_flags);

    if ( dma_txnd == NULL ) {
        pr_err("Unable to prepare the dma engine for the tx buffer.\n");
        goto stop_dma;
    }

    dma_txnd->callback_param = txr;
    dma_txnd->callback = nic_dma_callback_tx;
    dma_cookie = dmaengine_submit(dma_txnd);
    if (dma_submit_error(dma_cookie)) {
        pr_err("Unable to submit the tx request to the engine.\n");
        goto stop_dma;
    }

    // Return the DMA cookie for the transaction
    txr->cookie = dma_cookie;
    txr->submit_timestamp = get_cycles();
    dma_async_issue_pending(chan);
    return 0;

stop_dma:
    dmaengine_terminate_all(chan);
    return -EBUSY;
}

int nic_dma_prep_rx(struct nic* nic,struct rx_request* rxr){
    int data_len;
    struct dma_chan *chan;
    dma_addr_t dma_addr;
    dma_cookie_t dma_cookie;
    enum dma_ctrl_flags dma_flags;
    struct dma_async_tx_descriptor *dma_rxnd;

    // got rxr 
    chan = nic->dma_rx_chan.chan;
    dma_addr = rxr->dma_addr;
    data_len = rxr->data_len;

    // prep dma     
    dma_flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    dma_rxnd = dmaengine_prep_slave_single(chan, dma_addr, data_len,
	                DMA_DEV_TO_MEM, dma_flags);

    if ( dma_rxnd == NULL ) {
        pr_err("Unable to prepare the dma engine for the rx buffer.\n");
        goto stop_dma;
    }

    dma_rxnd->callback_param = rxr;
    dma_rxnd->callback = nic_dma_callback_rx;
    dma_cookie = dmaengine_submit(dma_rxnd);
    if (dma_submit_error(dma_cookie)) {
        pr_err("Unable to submit the rx request to the engine.\n");
        goto stop_dma;
    }

    // Return the DMA cookie for the transaction
    rxr->cookie = dma_cookie;
    rxr->submit_timestamp = get_cycles();
    dma_async_issue_pending(chan);
    return 0;

stop_dma:
    dmaengine_terminate_all(chan);
    return -EBUSY;
}