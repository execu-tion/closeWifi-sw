#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/string.h>
#include <asm/io.h>
#include "nic.h"
#include "nic_ethtool.h"

#define DRV_NAME    "nw_nic"
#define DRV_VERSION "1.0"

static void nic_get_drvinfo(struct net_device *ndev,
                            struct ethtool_drvinfo *info)
{
    struct nic *nic = netdev_priv(ndev);
    // mac version
    char fw_version_str[ETHTOOL_FWVERS_LEN]={'\0'};
    u32 fw_version = readl(nic->mac_ctrl_base + MAC_VERSION);
    sprintf(fw_version_str,"%x",fw_version);
    // phy version
    char hw_version_str[ETHTOOL_EROMVERS_LEN]={'\0'};
    u32 hw_version = readl(nic->phy_ctrl_base + PHY_VERSION);
    sprintf(hw_version_str,"%x",hw_version);
    // dump status
    strscpy(info->driver, DRV_NAME, sizeof(info->driver));
    strscpy(info->version, DRV_VERSION, sizeof(info->version));
    strscpy(info->fw_version,fw_version_str,strlen(fw_version_str));
    strscpy(info->erom_version,hw_version_str,strlen(hw_version_str));

    if (nic->pdev)
        strscpy(info->bus_info, nic->pdev->name, sizeof(info->bus_info));
}

static int nic_get_link_ksettings(struct net_device *ndev,
                                  struct ethtool_link_ksettings *cmd)
{
    struct nic *nic = netdev_priv(ndev);
    u32 current_mcs = readl(nic->mac_ctrl_base+MAC_CURRENT_MCS);

    cmd->base.speed = (current_mcs==0x0)?20:
                      (current_mcs==0xC)?15:
                      (current_mcs==0x4)?5:SPEED_10; 
            
    cmd->base.duplex = DUPLEX_FULL;       
    cmd->base.port = PORT_OTHER;          
    cmd->base.autoneg = AUTONEG_DISABLE;  

    // clear capable bits
    ethtool_link_ksettings_zero_link_mode(cmd, supported);
    ethtool_link_ksettings_zero_link_mode(cmd, advertising);
    return 0;
}

static void nic_get_ringparam(struct net_device *ndev,
                              struct ethtool_ringparam *ring,
                              struct kernel_ethtool_ringparam *kernel_ring,
                              struct netlink_ext_ack *extack)
{
    struct nic *nic = netdev_priv(ndev);
    ring->tx_max_pending = NIC_TX_QUEUE_LEN;
    ring->rx_max_pending = NIC_RX_QUEUE_LEN;
    ring->tx_pending = NIC_TX_QUEUE_LEN - nic->tx_queue.free_nums;
    ring->rx_pending = NIC_RX_QUEUE_LEN - nic->rx_queue.free_nums;
}


static int nic_get_regs_len(struct net_device *ndev)
{
    return MAC_REGS_SIZE;
}

static void nic_get_regs(struct net_device *ndev,
                         struct ethtool_regs *regs, void *p)
{
    struct nic *nic = netdev_priv(ndev);
    u32 *regs_buff = p;
    int i;

    regs->version = 1; 

    if (nic->mac_ctrl_base) {
        for (i = 0; i < (MAC_REGS_SIZE / 4); i++) {
            regs_buff[i] = readl(nic->mac_ctrl_base + i * 4);
        }
    } else {
        memset(p, 0, MAC_REGS_SIZE);
    }
}

static const char nic_gstrings_stats[][ETH_GSTRING_LEN] = {
    "rx_packets",
    "tx_packets",
    "rx_errors",
    "tx_errors",
};

#define NIC_STATS_NUM (sizeof(nic_gstrings_stats) / ETH_GSTRING_LEN)

static int nic_get_sset_count(struct net_device *ndev, int sset)
{
    switch (sset) {
    case ETH_SS_STATS:
        return NIC_STATS_NUM;
    default:
        return -EOPNOTSUPP;
    }
}

static void nic_get_strings(struct net_device *ndev, u32 stringset, u8 *data)
{
    if (stringset == ETH_SS_STATS) {
        memcpy(data, nic_gstrings_stats, sizeof(nic_gstrings_stats));
    }
}

static void nic_get_ethtool_stats(struct net_device *ndev,
                                  struct ethtool_stats *stats, u64 *data)
{
    int i = 0;
    
    data[i++] = ndev->stats.rx_packets;
    data[i++] = ndev->stats.tx_packets;
    data[i++] = ndev->stats.rx_errors;
    data[i++] = ndev->stats.tx_errors;
}

static const struct ethtool_ops nic_ethtool_ops = {
    .get_drvinfo          = nic_get_drvinfo,
    .get_link             = ethtool_op_get_link,
    .get_link_ksettings   = nic_get_link_ksettings,
    .get_ringparam        = nic_get_ringparam,
    .get_regs_len         = nic_get_regs_len,
    .get_regs             = nic_get_regs,
    .get_sset_count       = nic_get_sset_count,
    .get_strings          = nic_get_strings,
    .get_ethtool_stats    = nic_get_ethtool_stats,
};

void nic_set_ethtool_ops(struct net_device *ndev)
{
    ndev->ethtool_ops = &nic_ethtool_ops;
}
