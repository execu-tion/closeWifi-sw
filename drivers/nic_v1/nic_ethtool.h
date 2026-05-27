#ifndef _NIC_ETHTOOL_H_
#define _NIC_ETHTOOL_H_

#include <linux/netdevice.h>

void nic_set_ethtool_ops(struct net_device *ndev);

#endif // _NIC_ETHTOOL_H_
