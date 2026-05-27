#ifndef __MAC__H__
#define __MAC__H__

#define MAC_NODE_ID         0x00
#define MAC_VERSION         0x04
#define MAC_RESET           0x08
#define MAC_ADDR_LOW        0x0C
#define MAC_ADDR_HIGH       0x10
#define MAC_RETRY_TIMES     0x14
#define MAC_CABS_THRESH     0x18
#define MAC_RSSI_THRESH     0x1C
#define MAC_RTS_THRESH      0x20
#define MAC_TX              0x24
#define MAC_DROP            0x28   
#define MAC_CRC_PASSED      0x30
#define MAC_FILTERD         0x34
#define MAC_ACK_RESPONSE    0x38

#define MAC_CACHED          0x40
#define MAC_PACKET_LEN      0x4C

#define MAC_CURRENT_MCS     0x50
#define MAC_OVERWRITE_MCS   0x54
#define MAC_REGS_SIZE       0x50

#define mac_status \
    "node_id","mac_version","mac_reset","mac_addr_low","mac_addr_high","mac_retry_times",                   \
    "mac_cabs_thresh","mac_rssi_thresh","mac_rts_thresh","mac_tx","mac_drop","mac_crc_passed",              \
    "mac_filtered","mac_ack_response","mac_cached","mac_packet_len","mac_current_mcs","mac_overwrite_mcs",

#define nic_enable_mac(pnic)  do{ writel(0x0,pnic->mac_ctrl_base+MAC_RESET);} while(0);
#define nic_disable_mac(pnic) do{ writel(0x1,pnic->mac_ctrl_base+MAC_RESET);} while(0);


#endif