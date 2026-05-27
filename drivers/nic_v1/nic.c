#include "nic.h"
#include "nic_ethtool.h"

#include <linux/ip.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>


#define NAME_LEN 16
#define NAME_PREFIX "nic"

void nic_rx_complete(struct rx_request *rxr)
{
	struct nic *nic = rxr->cb_data;
	int data_len = rxr->data_len;

	// GFP_ATOMIC to avoid sleeping in softirq (DMA callback context)
	struct sk_buff *skb = netdev_alloc_skb_ip_align(nic->ndev, data_len - 2);
	if (!skb) {
		nic->ndev->stats.rx_dropped++;
		netdev_err(nic->ndev, "no mem for skb\n");
		return;
	}

	void *data = skb_put(skb, data_len-2);
	memcpy(data, rxr->buff+2,data_len-2);
	release_rx_request(nic,rxr);

	skb->protocol = eth_type_trans(skb, nic->ndev);

	/* 硬件只负责链路层完整性，上层IP/TCP校验和交由内核网络栈进行计算和验证 */
	skb->ip_summed = CHECKSUM_NONE;

	nic->ndev->stats.rx_packets++;
	nic->ndev->stats.rx_bytes += data_len;

	netif_rx(skb);
}

void nic_tx_complete(struct tx_request *txr)
{
	struct sk_buff *skb = txr->payload;
    struct nic* nic = txr->cb_data;


	// update counts
	if( txr->cookie == DMA_COMPLETE ){
		nic->ndev->stats.tx_packets++;
		nic->ndev->stats.tx_bytes += skb->len;
	}else{
		if( (enum dma_status)txr->cookie == DMA_ERROR ){
			// dma internal error, cause nic stop
			pr_err("dma internal error, net device stop!");
			netif_stop_queue(nic->ndev);
		}
		nic->ndev->stats.tx_errors++;
	}
    
    // unmap dma and release tx_request
    dma_unmap_sg(nic->device, txr->sg_table.sgl, 
                txr->sg_table.orig_nents, DMA_TO_DEVICE);

    sg_free_table(&txr->sg_table);

    // free skb
    dev_kfree_skb(skb);
    int avil = release_tx_request(nic,txr);
	wmb();
	// if arise from warn level, awake 
    if( avil == NIC_TX_QUEUE_WARN )
	    netif_wake_queue(nic->ndev);

	return;
}

static int nic_tx(struct sk_buff *skb, struct net_device *ndev)
{
	struct nic* nic = netdev_priv(ndev);
	struct tx_request* txr = NULL;
	int avil = get_tx_request(nic, &txr);
	wmb();

    if( avil == NIC_TX_QUEUE_STOP )
        netif_stop_queue(ndev);

    if( txr == NULL ){
        ndev->stats.tx_dropped++;
		pr_info("tx dma desciptor buffer error! nic shutdown!");
        netif_stop_queue(ndev); // may not appear
        return NETDEV_TX_BUSY;
    };

    txr->payload = skb;
    // issue dma pending
    nic_prepare_tx(nic, txr);

    return NETDEV_TX_OK;
}

static int nic_open(struct net_device *ndev)
{
	struct nic *nic = netdev_priv(ndev);
	
	pr_info("nic open : dev name = %s\n", ndev->name);

	nic_disable_phy(nic);
	nic_disable_mac(nic);

	int ret = tx_queue_init(nic);
    if ( ret < 0 ) {
        pr_err("tx queue init failed: ret %d\n", ret);
		return ret;
    }

	ret = rx_queue_init(nic);
	if ( ret < 0 ) {
		pr_err("rx queue init failed: ret %d\n", ret);
		tx_queue_exit(nic);
		return ret;
	}
	
	nic_enable_phy(nic);
	nic_enable_mac(nic);

	netif_start_queue(ndev);
	return 0;
}

static int nic_stop(struct net_device *ndev)
{
	struct nic *nic = netdev_priv(ndev);

	// stop nic queue
	netif_stop_queue(ndev);
	pr_info("opnet_stop: dev name = %s\n", ndev->name);
	
	/*
	 * Before releasing queues and freeing DMA buffers, make sure to terminate
	 * outstanding DMA transfers.
	 */
	if (nic->dma_tx_chan.chan)
		dmaengine_terminate_sync(nic->dma_tx_chan.chan);
	if (nic->dma_rx_chan.chan)
		dmaengine_terminate_sync(nic->dma_rx_chan.chan);

	/*
	 * Wait up to 200ms for both rx and tx queues to report they are free.
	 */
	int wait_ms = 200;
	while (wait_ms > 0) {
		if (nic->rx_queue.free_nums == NIC_RX_QUEUE_LEN &&
			nic->tx_queue.free_nums == NIC_TX_QUEUE_LEN)
			break;
		msleep(20);
		wait_ms -= 20;
	}

	if (nic->rx_queue.free_nums != NIC_RX_QUEUE_LEN ||
		nic->tx_queue.free_nums != NIC_TX_QUEUE_LEN) {
		pr_warn("nic_stop: queues not fully drained: tx_free=%d rx_free=%d\n",
				nic->tx_queue.free_nums, nic->rx_queue.free_nums);
	}

	nic_disable_phy(nic);
	nic_disable_mac(nic);
	
	// release tx/rx queues (will free DMA buffers)
	tx_queue_exit(nic);
	rx_queue_exit(nic);

	return 0;
}

static int nic_change_mtu(struct net_device *ndev, int new_mtu)
{
	struct nic __attribute__((unused)) *nic = netdev_priv(ndev);
    int ret;
    
    // need validation avoid mtu exceed buffer length
    if ( new_mtu > NIC_MAX_MTU ) {
        netdev_err(ndev, "invalid MTU %d, must smaller than [%d]!\n",
                  new_mtu, NIC_MAX_MTU);
        return -EINVAL;
    }
    
    if ( ndev->mtu == new_mtu )
        return 0;
    
    // stop xmit
    netif_tx_lock_bh(ndev);
    netif_device_detach(ndev);
    
    // set up for hardware, may write a register-> not implemented yet.
    ret = 0; //my_driver_hw_set_mtu(priv->hw_regs, new_mtu);
    if (ret) {
        netdev_err(ndev, "MTU change failed: %d\n", ret);
        netif_device_attach(ndev);
        netif_tx_unlock_bh(ndev);
        return ret;
    }
    
    // update nic status
    ndev->mtu = new_mtu;
    
    // start xmit
    netif_device_attach(ndev);
    netif_tx_unlock_bh(ndev);
    
    netdev_info(ndev, "MTU changed to %d!\n", ndev->mtu);
    return 0;
}

static const struct net_device_ops nic_ops = {
	.ndo_open 		= nic_open, // start nic
	.ndo_stop 		= nic_stop, // stop nic
	.ndo_start_xmit = nic_tx,   // tx path
	//.ndo_do_ioctl 	= nic_ioctl,      // io control
	//.ndo_set_mac_address = nic_set_mac, // 
	.ndo_change_mtu = nic_change_mtu,   // change mtu
};


// setup ethernet net_device
static void nic_setup(struct net_device *netdev){
	ether_setup(netdev);

	netdev->netdev_ops		 = &nic_ops;
	netdev->mtu              = DEFAULT_NIC_MTU;
	netdev->tx_queue_len     = 1;

	// 允许 Scatter/Gather
	netdev->features |= NETIF_F_SG;
	
	// 支持高位地址内存
	netdev->features |= NETIF_F_HIGHDMA;

	netdev->features |= NETIF_F_HW_CSUM;  // 发送时硬件计算 Checksum
	netdev->features |= NETIF_F_RXCSUM;   // 接收时硬件检查 Checksum
};

irqreturn_t nic_irq_handler(int irq, void *dev_id)
{
	struct nic *nic = (struct nic *) dev_id;
	struct rx_request* rxr = NULL;
	// avoid reset pulse
	if( nic->dma_rx_chan.channel_id == -1 )
		return IRQ_HANDLED;
	
	// read the packets received
	u32 cached_packets  = readl(nic->mac_ctrl_base+MAC_CACHED);
	u32 cached_curr_len = readl(nic->mac_ctrl_base+MAC_PACKET_LEN);

	if( cached_packets == 0 ){	// ignored for non-cached cases
		return IRQ_HANDLED;
	}

	/* cap to a sane maximum to avoid huge allocations */
	// when rx_len == 0, it will overflow to a large value
	if ( cached_curr_len -1 > NIC_MAX_MTU -1 ){
		nic->ndev->stats.rx_length_errors++;
		nic->ndev->stats.rx_errors++;
		return IRQ_HANDLED;
	}

	// while (cached_packets > 0) {
		get_rx_request(nic, &rxr);
		wmb();

		if( rxr == NULL ){
			pr_err("NIC: FATAL ERROR DMA RX ERROR!");
        	nic->ndev->stats.rx_dropped++;
			netif_stop_queue(nic->ndev);
			return IRQ_HANDLED;
    	}

		rxr->data_len = cached_curr_len;
		// issue dma pending
    	nic_prepare_rx(nic, rxr);
		
		// cached_packets  = readl(nic->mac_ctrl_base+MAC_CACHED);
		// cached_curr_len = readl(nic->mac_ctrl_base+MAC_PACKET_LEN);
	// }
	
	return IRQ_HANDLED;
};


/*
 * @description    : platform driver probe
 * 		binding dma channel, irq number and mapping essential reg base
 */
static int nic_probe(struct platform_device *pdev)
{
    int ret = 0;
    // alloc an net device
    struct net_device* ndev = alloc_netdev(sizeof(struct nic), "nic%d",NET_NAME_UNKNOWN, nic_setup);
    if( ndev == NULL )
        return -ENOMEM;

    // alloc nic driver
    struct nic* nic = netdev_priv(ndev);
    nic->pdev = pdev;
    nic->device = &pdev->dev;
    platform_set_drvdata(pdev, ndev);

    ret = nic_get_resources(nic);
    if( ret )
        goto error_resources;

    ret = nic_dma_init(nic);
    if( ret )
        goto error_resources;

    nic_reset_hardware(nic);

	/* 从硬件寄存器中读取 MAC 地址 */
	{
		u32 mac_lo = readl(nic->mac_ctrl_base + MAC_ADDR_LOW);
		u32 mac_hi = readl(nic->mac_ctrl_base + MAC_ADDR_HIGH);
		u8 hw_mac[ETH_ALEN];

		// 按照硬件逆序模式调整字节映射
		hw_mac[5] = mac_lo & 0xFF;
		hw_mac[4] = (mac_lo >> 8) & 0xFF;
		hw_mac[3] = (mac_lo >> 16) & 0xFF;
		hw_mac[2] = (mac_lo >> 24) & 0xFF;
		hw_mac[1] = mac_hi & 0xFF;
		hw_mac[0] = (mac_hi >> 8) & 0xFF;

		/* 校验读出的 MAC 地址是否为合法的单播设备地址 */
		if (is_valid_ether_addr(hw_mac)) {
			eth_hw_addr_set(ndev, hw_mac);
			dev_info(&pdev->dev, "Using HW MAC address: %pM\n", hw_mac);
		} else {
			dev_warn(&pdev->dev, "Invalid HW MAC address %pM, falling back to random\n", hw_mac);
			eth_hw_addr_random(ndev);
		}
	}

	nic_set_ethtool_ops(ndev);

    nic->ndev = ndev;
	ret = register_netdev(ndev);
	if (ret) {
		pr_err("register_netdev failed: ret %d\n", ret);
		nic->ndev = NULL;
		goto error_dma;
	}
	return 0;

error_dma:
    nic_dma_exit(nic);
error_resources:
   
//error_netdev:
    free_netdev(ndev);
    platform_set_drvdata(pdev, NULL);
    return ret;
}


/*
 * @description    : platform driver remove
 */
static void nic_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
    struct nic* nic = netdev_priv(ndev);

    unregister_netdev(ndev);
    nic_dma_exit(nic);

    free_netdev(ndev);
    platform_set_drvdata(pdev, NULL);
	return;
}


/* device of match table */
static const struct of_device_id nic_of_match[] = {
    { .compatible = "none,nic"}, //
	{ /* Sentinel */ }
};

/* platform driver struct */
static struct platform_driver nic_driver = {
	.driver     = {
		.name   = "nic_driver",         
		.of_match_table = nic_of_match,
	},
	.probe      = nic_probe,
	.remove     = nic_remove,
};


/*
 * @description: driver init
 */
static int __init nic_init(void)
{
	return platform_driver_register(&nic_driver);
}

/*
 * @description: driver remove
 */
static void __exit nic_exit(void)
{
	platform_driver_unregister(&nic_driver);
}

module_init(nic_init);
module_exit(nic_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("xcution");