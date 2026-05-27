#include "nic.h"
#include "phy.h"
#include "mac.h"

inline void nic_set_hardware_mac(struct nic* nic, void* addr){
	u8* mac_new = (u8*) addr;
	u32 mac_high_reg = 0;
	u32 mac_low_reg  = 0;
	mac_high_reg = ( mac_new[0] << 8 ) | ( mac_new[1] << 0 );
	mac_low_reg  = ( mac_new[2] << 24 )| ( mac_new[3] << 16) |
					( mac_new[4] << 8 )| ( mac_new[5] << 0);
	writel(mac_high_reg, nic->mac_ctrl_base + MAC_ADDR_HIGH);
	writel(mac_low_reg, nic->mac_ctrl_base + MAC_ADDR_LOW);
};

inline void nic_get_hardware_mac(struct nic* nic, void* addr){
	u8* hw_mac = (u8*)addr;

	u32 mac_lo = readl(nic->mac_ctrl_base + MAC_ADDR_LOW );
	u32 mac_hi = readl(nic->mac_ctrl_base + MAC_ADDR_HIGH);

	hw_mac[5] = 0xFF & ( mac_lo >>  0);
	hw_mac[4] = 0xFF & ( mac_lo >>  8);
	hw_mac[3] = 0xFF & ( mac_lo >> 16);
	hw_mac[2] = 0xFF & ( mac_lo >> 24);
	hw_mac[1] = 0xFF & ( mac_hi >>  0);
	hw_mac[0] = 0xFF & ( mac_hi >>  8);
};

inline void nic_reset_phy(struct nic* nic){
    /* reset nic register */
	writel(0x1,nic->phy_ctrl_base+PHY_RESET);
    mdelay(10);  //
	writel(0x0,nic->phy_ctrl_base+PHY_RESET);
    wmb();
};

inline void nic_reset_mac(struct nic* nic){
    /* reset nic register */
	writel(0x1,nic->mac_ctrl_base+MAC_RESET);
    mdelay(10);  //
	writel(0x0,nic->mac_ctrl_base+MAC_RESET);
    wmb();
};

inline void nic_reset_hardware(struct nic* nic){
    nic_reset_mac(nic);
	nic_reset_phy(nic);
};

int nic_get_resources(struct nic* nic){
    struct resource *res = NULL;
    int ret = -1;
    
    // get phy ctrl regs
    res = platform_get_resource(nic->pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_err("Failed to get memory resource 0\n");
		return -ENODEV;
	};

    // nic->phy_ctrl_base = devm_ioremap_resource(nic->device, res);
	nic->phy_ctrl_base = devm_ioremap(nic->device, res->start, resource_size(res));

	if( IS_ERR(nic->phy_ctrl_base) ){
		pr_err("Failed to map rx ctrl regs\n");
		return PTR_ERR(nic->phy_ctrl_base);
	};

    // get mac ctrl regs
	res = platform_get_resource(nic->pdev, IORESOURCE_MEM, 1);
	if (!res) {
		pr_err("Failed to get memory resource 1\n");
		return -ENODEV;
	};

	nic->mac_ctrl_base = devm_ioremap_resource(nic->device, res);
	if( IS_ERR(nic->mac_ctrl_base) ){
		pr_err("Failed to map ctrl regs\n");
		return PTR_ERR(nic->mac_ctrl_base);
	};

    // get irq number from device tree
    nic->irq_desc.num = -1;
	nic->irq_desc.num = platform_get_irq(nic->pdev, 0);
	if (nic->irq_desc.num < 0) {
		pr_err("failed to get irq from device tree\n");
		return -ENODEV;
	}

	pr_info("nic use irq num [%d] !\n", nic->irq_desc.num);

	nic->irq_desc.name = devm_kmalloc(nic->device, NIC_NAME_LEN, GFP_KERNEL);
	if (!nic->irq_desc.name) {
		pr_err("failed to malloc memory for irq name\n");
		return -ENOMEM;
	}

	snprintf((char *)nic->irq_desc.name, NIC_NAME_LEN, "nic_irq_%d", nic->irq_desc.num);
	nic->irq_desc.handler = nic_irq_handler;

	// request irq in probe so devm manages lifetime.
	ret = devm_request_irq(nic->device, nic->irq_desc.num, nic->irq_desc.handler,
						  0, nic->irq_desc.name, nic);
	if (ret) {
		pr_err("request_irq failed in probe: ret %d\n", ret);
		return ret;
	}

    return 0;
};

void nic_free_resources(struct nic* nic){
    return;
};

