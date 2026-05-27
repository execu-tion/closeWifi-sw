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

	struct sk_buff* skb = (struct sk_buff*)nic_detach_payload(nic,rxr);
	if( skb ){
		nic->ndev->stats.rx_packets++;
		nic->ndev->stats.rx_bytes += skb->len;

		skb->protocol  = eth_type_trans(skb, nic->ndev);
		skb->ip_summed = CHECKSUM_NONE;
		netif_rx(skb);
	};

	if( nic_replace_request(nic,rxr) != 0){
		pr_warn("dma replaced error!");
	};
};

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
		goto free_tx;
    };

	ret = rx_queue_init(nic);
	if ( ret < 0 ) {
		pr_err("rx queue init failed: ret %d\n", ret);
		goto free_rx;
	};

	// start the nic receive 
	nic_enable_phy(nic);
	nic_enable_mac(nic);

	netif_start_queue(ndev);
	return 0;

free_rx:
	rx_queue_exit(nic);

free_tx:
	dmaengine_terminate_sync(nic->dma_rx_chan.chan);
	tx_queue_exit(nic);
	return ret;
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

	// wait driver sync
	msleep(200);
	
	// release tx/rx queues (will free DMA buffers)
	tx_queue_exit(nic);
	rx_queue_exit(nic);

	// disable hardware
	nic_disable_phy(nic);
	nic_disable_mac(nic);

	return 0;
}

static int nic_change_mtu(struct net_device *ndev, int new_mtu)
{    
    // only an announcement
    if ( new_mtu > NIC_MAX_MTU || new_mtu < 64 ) {
        netdev_err(ndev, "invalid MTU %d, must smaller than [%d]!\n",
                  new_mtu, NIC_MAX_MTU);
        return -EINVAL;
    }
    
    if ( ndev->mtu == new_mtu )
        return 0;
    
    // update nic status
    ndev->mtu = new_mtu;
    netdev_info(ndev, "MTU changed to %d!\n", ndev->mtu);
    return 0;
}

static int nic_set_mac(struct net_device* ndev, void* addr)
{
	struct nic *nic = netdev_priv(ndev);
	struct sockaddr *sa = addr;

	if (!sa)
		return -EINVAL;

	/* sa->sa_data contains the 6-byte MAC for SIOCSIFHWADDR */
	if (!is_valid_ether_addr((u8 *)sa->sa_data)) {
		netdev_err(ndev, "invalid mac address\n");
		return -EADDRNOTAVAIL;
	};

	/* set new MAC to netdev */
	u8 hw_addr[ETH_ALEN];
	nic_set_hardware_mac(nic,sa->sa_data);
	nic_get_hardware_mac(nic,hw_addr);
	eth_hw_addr_set(ndev, hw_addr);
	netdev_info(ndev, "MAC address set to %pM\n", hw_addr);

	return 0;
}

static const struct net_device_ops nic_ops = {
	.ndo_open 		= nic_open, // start nic
	.ndo_stop 		= nic_stop, // stop nic
	.ndo_start_xmit = nic_tx,   // tx path
	//.ndo_do_ioctl 	= nic_ioctl,      // io control
	.ndo_set_mac_address = nic_set_mac, //
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
	
	// read the packets received
	u32 cached_packets  = readl(nic->mac_ctrl_base+MAC_CACHED);
	u32 cached_curr_len = readl(nic->mac_ctrl_base+MAC_PACKET_LEN);

	/* enable via /sys/kernel/debug/dynamic_debug/control
	 * to avoid flooding dmesg under high packet rates.
	 */
	netdev_dbg(nic->ndev, "irq: cached=%u len=%u\n",
			   cached_packets, cached_curr_len);
	
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
	u8 hw_mac[ETH_ALEN];
	nic_get_hardware_mac(nic,&hw_mac);

	if (is_valid_ether_addr(hw_mac)) {
		eth_hw_addr_set(ndev, hw_mac);
		dev_info(&pdev->dev, "Using HW MAC address: %pM\n", hw_mac);
	} else {
		dev_warn(&pdev->dev, "Invalid HW MAC address %pM, falling back to random\n", hw_mac);
		eth_hw_addr_random(ndev);
		ether_addr_copy(hw_mac,nic->ndev->dev_addr);
		nic_set_hardware_mac(nic,hw_mac);
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


/* platform driver remove */
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

/* driver init */
static int __init nic_init(void)
{
	return platform_driver_register(&nic_driver);
}

/* driver remove */
static void __exit nic_exit(void)
{
	platform_driver_unregister(&nic_driver);
}

module_init(nic_init);
module_exit(nic_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("xcution");