#ifndef __PHY__H__
#define __PHY__H__

// OFFSET of the phy base reg
#define PHY_VERSION             0x00
#define PHY_RESET               0x04
#define PHY_MIN_PLATEAU         0x08
#define PHY_FINE_THRESH         0x0C

#define PHY_SYNC_SHORT          0x10
#define PHY_SYNC_LONG           0x14
#define PHY_SYNC_FAIL           0x18
#define PHY_CRC_FAIL            0x1C
#define PHY_CRC_OK              0x20

#define PHY_DEBUG_ON            0x30
#define PHY_DEBUG_MCS           0x34
#define PHY_DEBUG_LEN           0x38
#define PHY_DEBUG_PACKETS       0x3C
#define PHY_TRANSMITTED         0x40
#define PHY_RECEIVED            0x44
#define PHY_BUF_OVF             0x48


#define nic_enable_phy(pnic)  do{ writel(0x0,pnic->phy_ctrl_base+PHY_RESET);} while(0);
#define nic_disable_phy(pnic) do{ writel(0x1,pnic->phy_ctrl_base+PHY_RESET);} while(0);

#endif