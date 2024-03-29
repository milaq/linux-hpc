/*
 * FSL SoC setup code
 *
 * Maintained by Kumar Gala (see MAINTAINERS for contact information)
 *
 * 2006 (c) MontaVista Software, Inc.
 * Vitaly Bordug <vbordug@ru.mvista.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/fsl_devices.h>
#include <linux/fs_enet_pd.h>
#include <linux/fs_uart_pd.h>

#include <asm/system.h>
#include <asm/atomic.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/time.h>
#include <asm/prom.h>
#include <sysdev/fsl_soc.h>
#include <mm/mmu_decl.h>
#include <asm/cpm2.h>

extern void init_fcc_ioports(struct fs_platform_info*);
extern void init_scc_ioports(struct fs_uart_platform_info*);
static phys_addr_t immrbase = -1;

phys_addr_t get_immrbase(void)
{
	struct device_node *soc;

	if (immrbase != -1)
		return immrbase;

	soc = of_find_node_by_type(NULL, "soc");
	if (soc) {
		unsigned int size;
		const void *prop = get_property(soc, "reg", &size);

		if (prop)
			immrbase = of_translate_address(soc, prop);
		of_node_put(soc);
	};

	return immrbase;
}

EXPORT_SYMBOL(get_immrbase);

#ifdef CONFIG_CPM2

static u32 brgfreq = -1;

u32 get_brgfreq(void)
{
	struct device_node *node;

	if (brgfreq != -1)
		return brgfreq;

	node = of_find_node_by_type(NULL, "cpm");
	if (node) {
		unsigned int size;
		const unsigned int *prop = get_property(node, "brg-frequency",
					&size);

		if (prop)
			brgfreq = *prop;
		of_node_put(node);
	};

	return brgfreq;
}

EXPORT_SYMBOL(get_brgfreq);

static u32 fs_baudrate = -1;

u32 get_baudrate(void)
{
	struct device_node *node;

	if (fs_baudrate != -1)
		return fs_baudrate;

	node = of_find_node_by_type(NULL, "serial");
	if (node) {
		unsigned int size;
		const unsigned int *prop = get_property(node, "current-speed",
				&size);

		if (prop)
			fs_baudrate = *prop;
		of_node_put(node);
	};

	return fs_baudrate;
}

EXPORT_SYMBOL(get_baudrate);
#endif /* CONFIG_CPM2 */

static int __init gfar_mdio_of_init(void)
{
	struct device_node *np;
	unsigned int i;
	struct platform_device *mdio_dev;
	struct resource res;
	int ret;

	for (np = NULL, i = 0;
	     (np = of_find_compatible_node(np, "mdio", "gianfar")) != NULL;
	     i++) {
		int k;
		struct device_node *child = NULL;
		struct gianfar_mdio_data mdio_data;

		memset(&res, 0, sizeof(res));
		memset(&mdio_data, 0, sizeof(mdio_data));

		ret = of_address_to_resource(np, 0, &res);
		if (ret)
			goto err;

		mdio_dev =
		    platform_device_register_simple("fsl-gianfar_mdio",
						    res.start, &res, 1);
		if (IS_ERR(mdio_dev)) {
			ret = PTR_ERR(mdio_dev);
			goto err;
		}

		for (k = 0; k < 32; k++)
			mdio_data.irq[k] = -1;

		while ((child = of_get_next_child(np, child)) != NULL) {
			int irq = irq_of_parse_and_map(child, 0);
			if (irq != NO_IRQ) {
				const u32 *id = get_property(child, "reg", NULL);
				mdio_data.irq[*id] = irq;
			}
		}

		ret =
		    platform_device_add_data(mdio_dev, &mdio_data,
					     sizeof(struct gianfar_mdio_data));
		if (ret)
			goto unreg;
	}

	return 0;

unreg:
	platform_device_unregister(mdio_dev);
err:
	return ret;
}

arch_initcall(gfar_mdio_of_init);

static const char *gfar_tx_intr = "tx";
static const char *gfar_rx_intr = "rx";
static const char *gfar_err_intr = "error";

static int __init gfar_of_init(void)
{
	struct device_node *np;
	unsigned int i;
	struct platform_device *gfar_dev;
	struct resource res;
	int ret;

	for (np = NULL, i = 0;
	     (np = of_find_compatible_node(np, "network", "gianfar")) != NULL;
	     i++) {
		struct resource r[4];
		struct device_node *phy, *mdio;
		struct gianfar_platform_data gfar_data;
		const unsigned int *id;
		const char *model;
		const void *mac_addr;
		const phandle *ph;
		int n_res = 2;

		memset(r, 0, sizeof(r));
		memset(&gfar_data, 0, sizeof(gfar_data));

		ret = of_address_to_resource(np, 0, &r[0]);
		if (ret)
			goto err;

		r[1].start = r[1].end = irq_of_parse_and_map(np, 0);
		r[1].flags = IORESOURCE_IRQ;

		model = get_property(np, "model", NULL);

		/* If we aren't the FEC we have multiple interrupts */
		if (model && strcasecmp(model, "FEC")) {
			r[1].name = gfar_tx_intr;

			r[2].name = gfar_rx_intr;
			r[2].start = r[2].end = irq_of_parse_and_map(np, 1);
			r[2].flags = IORESOURCE_IRQ;

			r[3].name = gfar_err_intr;
			r[3].start = r[3].end = irq_of_parse_and_map(np, 2);
			r[3].flags = IORESOURCE_IRQ;

			n_res += 2;
		}

		gfar_dev =
		    platform_device_register_simple("fsl-gianfar", i, &r[0],
						    n_res);

		if (IS_ERR(gfar_dev)) {
			ret = PTR_ERR(gfar_dev);
			goto err;
		}

		mac_addr = get_property(np, "local-mac-address", NULL);
		if (mac_addr == NULL)
			mac_addr = get_property(np, "mac-address", NULL);
		if (mac_addr == NULL) {
			/* Obsolete */
			mac_addr = get_property(np, "address", NULL);
		}

		if (mac_addr)
			memcpy(gfar_data.mac_addr, mac_addr, 6);

		if (model && !strcasecmp(model, "TSEC"))
			gfar_data.device_flags =
			    FSL_GIANFAR_DEV_HAS_GIGABIT |
			    FSL_GIANFAR_DEV_HAS_COALESCE |
			    FSL_GIANFAR_DEV_HAS_RMON |
			    FSL_GIANFAR_DEV_HAS_MULTI_INTR;
		if (model && !strcasecmp(model, "eTSEC"))
			gfar_data.device_flags =
			    FSL_GIANFAR_DEV_HAS_GIGABIT |
			    FSL_GIANFAR_DEV_HAS_COALESCE |
			    FSL_GIANFAR_DEV_HAS_RMON |
			    FSL_GIANFAR_DEV_HAS_MULTI_INTR |
			    FSL_GIANFAR_DEV_HAS_CSUM |
			    FSL_GIANFAR_DEV_HAS_VLAN |
			    FSL_GIANFAR_DEV_HAS_EXTENDED_HASH;

		ph = get_property(np, "phy-handle", NULL);
		phy = of_find_node_by_phandle(*ph);

		if (phy == NULL) {
			ret = -ENODEV;
			goto unreg;
		}

		mdio = of_get_parent(phy);

		id = get_property(phy, "reg", NULL);
		ret = of_address_to_resource(mdio, 0, &res);
		if (ret) {
			of_node_put(phy);
			of_node_put(mdio);
			goto unreg;
		}

		gfar_data.phy_id = *id;
		gfar_data.bus_id = res.start;

		of_node_put(phy);
		of_node_put(mdio);

		ret =
		    platform_device_add_data(gfar_dev, &gfar_data,
					     sizeof(struct
						    gianfar_platform_data));
		if (ret)
			goto unreg;
	}

	return 0;

unreg:
	platform_device_unregister(gfar_dev);
err:
	return ret;
}

arch_initcall(gfar_of_init);

static int __init fsl_i2c_of_init(void)
{
	struct device_node *np;
	unsigned int i;
	struct platform_device *i2c_dev;
	int ret;

	for (np = NULL, i = 0;
	     (np = of_find_compatible_node(np, "i2c", "fsl-i2c")) != NULL;
	     i++) {
		struct resource r[2];
		struct fsl_i2c_platform_data i2c_data;
		const unsigned char *flags = NULL;

		memset(&r, 0, sizeof(r));
		memset(&i2c_data, 0, sizeof(i2c_data));

		ret = of_address_to_resource(np, 0, &r[0]);
		if (ret)
			goto err;

		r[1].start = r[1].end = irq_of_parse_and_map(np, 0);
		r[1].flags = IORESOURCE_IRQ;

		i2c_dev = platform_device_register_simple("fsl-i2c", i, r, 2);
		if (IS_ERR(i2c_dev)) {
			ret = PTR_ERR(i2c_dev);
			goto err;
		}

		i2c_data.device_flags = 0;
		flags = get_property(np, "dfsrr", NULL);
		if (flags)
			i2c_data.device_flags |= FSL_I2C_DEV_SEPARATE_DFSRR;

		flags = get_property(np, "fsl5200-clocking", NULL);
		if (flags)
			i2c_data.device_flags |= FSL_I2C_DEV_CLOCK_5200;

		ret =
		    platform_device_add_data(i2c_dev, &i2c_data,
					     sizeof(struct
						    fsl_i2c_platform_data));
		if (ret)
			goto unreg;
	}

	return 0;

unreg:
	platform_device_unregister(i2c_dev);
err:
	return ret;
}

arch_initcall(fsl_i2c_of_init);

#ifdef CONFIG_PPC_83xx
static int __init mpc83xx_wdt_init(void)
{
	struct resource r;
	struct device_node *soc, *np;
	struct platform_device *dev;
	const unsigned int *freq;
	int ret;

	np = of_find_compatible_node(NULL, "watchdog", "mpc83xx_wdt");

	if (!np) {
		ret = -ENODEV;
		goto nodev;
	}

	soc = of_find_node_by_type(NULL, "soc");

	if (!soc) {
		ret = -ENODEV;
		goto nosoc;
	}

	freq = get_property(soc, "bus-frequency", NULL);
	if (!freq) {
		ret = -ENODEV;
		goto err;
	}

	memset(&r, 0, sizeof(r));

	ret = of_address_to_resource(np, 0, &r);
	if (ret)
		goto err;

	dev = platform_device_register_simple("mpc83xx_wdt", 0, &r, 1);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err;
	}

	ret = platform_device_add_data(dev, freq, sizeof(int));
	if (ret)
		goto unreg;

	of_node_put(soc);
	of_node_put(np);

	return 0;

unreg:
	platform_device_unregister(dev);
err:
	of_node_put(soc);
nosoc:
	of_node_put(np);
nodev:
	return ret;
}

arch_initcall(mpc83xx_wdt_init);
#endif

static enum fsl_usb2_phy_modes determine_usb_phy(const char *phy_type)
{
	if (!phy_type)
		return FSL_USB2_PHY_NONE;
	if (!strcasecmp(phy_type, "ulpi"))
		return FSL_USB2_PHY_ULPI;
	if (!strcasecmp(phy_type, "utmi"))
		return FSL_USB2_PHY_UTMI;
	if (!strcasecmp(phy_type, "utmi_wide"))
		return FSL_USB2_PHY_UTMI_WIDE;
	if (!strcasecmp(phy_type, "serial"))
		return FSL_USB2_PHY_SERIAL;

	return FSL_USB2_PHY_NONE;
}

static int __init fsl_usb_of_init(void)
{
	struct device_node *np;
	unsigned int i;
	struct platform_device *usb_dev_mph = NULL, *usb_dev_dr = NULL;
	int ret;

	for (np = NULL, i = 0;
	     (np = of_find_compatible_node(np, "usb", "fsl-usb2-mph")) != NULL;
	     i++) {
		struct resource r[2];
		struct fsl_usb2_platform_data usb_data;
		const unsigned char *prop = NULL;

		memset(&r, 0, sizeof(r));
		memset(&usb_data, 0, sizeof(usb_data));

		ret = of_address_to_resource(np, 0, &r[0]);
		if (ret)
			goto err;

		r[1].start = r[1].end = irq_of_parse_and_map(np, 0);
		r[1].flags = IORESOURCE_IRQ;

		usb_dev_mph =
		    platform_device_register_simple("fsl-ehci", i, r, 2);
		if (IS_ERR(usb_dev_mph)) {
			ret = PTR_ERR(usb_dev_mph);
			goto err;
		}

		usb_dev_mph->dev.coherent_dma_mask = 0xffffffffUL;
		usb_dev_mph->dev.dma_mask = &usb_dev_mph->dev.coherent_dma_mask;

		usb_data.operating_mode = FSL_USB2_MPH_HOST;

		prop = get_property(np, "port0", NULL);
		if (prop)
			usb_data.port_enables |= FSL_USB2_PORT0_ENABLED;

		prop = get_property(np, "port1", NULL);
		if (prop)
			usb_data.port_enables |= FSL_USB2_PORT1_ENABLED;

		prop = get_property(np, "phy_type", NULL);
		usb_data.phy_mode = determine_usb_phy(prop);

		ret =
		    platform_device_add_data(usb_dev_mph, &usb_data,
					     sizeof(struct
						    fsl_usb2_platform_data));
		if (ret)
			goto unreg_mph;
	}

	for (np = NULL;
	     (np = of_find_compatible_node(np, "usb", "fsl-usb2-dr")) != NULL;
	     i++) {
		struct resource r[2];
		struct fsl_usb2_platform_data usb_data;
		const unsigned char *prop = NULL;

		memset(&r, 0, sizeof(r));
		memset(&usb_data, 0, sizeof(usb_data));

		ret = of_address_to_resource(np, 0, &r[0]);
		if (ret)
			goto unreg_mph;

		r[1].start = r[1].end = irq_of_parse_and_map(np, 0);
		r[1].flags = IORESOURCE_IRQ;

		usb_dev_dr =
		    platform_device_register_simple("fsl-ehci", i, r, 2);
		if (IS_ERR(usb_dev_dr)) {
			ret = PTR_ERR(usb_dev_dr);
			goto err;
		}

		usb_dev_dr->dev.coherent_dma_mask = 0xffffffffUL;
		usb_dev_dr->dev.dma_mask = &usb_dev_dr->dev.coherent_dma_mask;

		usb_data.operating_mode = FSL_USB2_DR_HOST;

		prop = get_property(np, "phy_type", NULL);
		usb_data.phy_mode = determine_usb_phy(prop);

		ret =
		    platform_device_add_data(usb_dev_dr, &usb_data,
					     sizeof(struct
						    fsl_usb2_platform_data));
		if (ret)
			goto unreg_dr;
	}
	return 0;

unreg_dr:
	if (usb_dev_dr)
		platform_device_unregister(usb_dev_dr);
unreg_mph:
	if (usb_dev_mph)
		platform_device_unregister(usb_dev_mph);
err:
	return ret;
}

arch_initcall(fsl_usb_of_init);

#ifdef CONFIG_CPM2

static const char fcc_regs[] = "fcc_regs";
static const char fcc_regs_c[] = "fcc_regs_c";
static const char fcc_pram[] = "fcc_pram";
static char bus_id[9][BUS_ID_SIZE];

static int __init fs_enet_of_init(void)
{
	struct device_node *np;
	unsigned int i;
	struct platform_device *fs_enet_dev;
	struct resource res;
	int ret;

	for (np = NULL, i = 0;
	     (np = of_find_compatible_node(np, "network", "fs_enet")) != NULL;
	     i++) {
		struct resource r[4];
		struct device_node *phy, *mdio;
		struct fs_platform_info fs_enet_data;
		const unsigned int *id, *phy_addr, *phy_irq;
		const void *mac_addr;
		const phandle *ph;
		const char *model;

		memset(r, 0, sizeof(r));
		memset(&fs_enet_data, 0, sizeof(fs_enet_data));

		ret = of_address_to_resource(np, 0, &r[0]);
		if (ret)
			goto err;
		r[0].name = fcc_regs;

		ret = of_address_to_resource(np, 1, &r[1]);
		if (ret)
			goto err;
		r[1].name = fcc_pram;

		ret = of_address_to_resource(np, 2, &r[2]);
		if (ret)
			goto err;
		r[2].name = fcc_regs_c;
		fs_enet_data.fcc_regs_c = r[2].start;

		r[3].start = r[3].end = irq_of_parse_and_map(np, 0);
		r[3].flags = IORESOURCE_IRQ;

		fs_enet_dev =
		    platform_device_register_simple("fsl-cpm-fcc", i, &r[0], 4);

		if (IS_ERR(fs_enet_dev)) {
			ret = PTR_ERR(fs_enet_dev);
			goto err;
		}

		model = get_property(np, "model", NULL);
		if (model == NULL) {
			ret = -ENODEV;
			goto unreg;
		}

		mac_addr = get_property(np, "mac-address", NULL);
		memcpy(fs_enet_data.macaddr, mac_addr, 6);

		ph = get_property(np, "phy-handle", NULL);
		phy = of_find_node_by_phandle(*ph);

		if (phy == NULL) {
			ret = -ENODEV;
			goto unreg;
		}

		phy_addr = get_property(phy, "reg", NULL);
		fs_enet_data.phy_addr = *phy_addr;

		phy_irq = get_property(phy, "interrupts", NULL);

		id = get_property(np, "device-id", NULL);
		fs_enet_data.fs_no = *id;
		strcpy(fs_enet_data.fs_type, model);

		mdio = of_get_parent(phy);
                ret = of_address_to_resource(mdio, 0, &res);
                if (ret) {
                        of_node_put(phy);
                        of_node_put(mdio);
                        goto unreg;
                }

		fs_enet_data.clk_rx = *((u32 *) get_property(np, "rx-clock", NULL));
		fs_enet_data.clk_tx = *((u32 *) get_property(np, "tx-clock", NULL));

		if (strstr(model, "FCC")) {
			int fcc_index = *id - 1;
			const unsigned char *mdio_bb_prop;

			fs_enet_data.dpram_offset = (u32)cpm_dpram_addr(0);
			fs_enet_data.rx_ring = 32;
			fs_enet_data.tx_ring = 32;
			fs_enet_data.rx_copybreak = 240;
			fs_enet_data.use_napi = 0;
			fs_enet_data.napi_weight = 17;
			fs_enet_data.mem_offset = FCC_MEM_OFFSET(fcc_index);
			fs_enet_data.cp_page = CPM_CR_FCC_PAGE(fcc_index);
			fs_enet_data.cp_block = CPM_CR_FCC_SBLOCK(fcc_index);

			snprintf((char*)&bus_id[(*id)], BUS_ID_SIZE, "%x:%02x",
							(u32)res.start, fs_enet_data.phy_addr);
			fs_enet_data.bus_id = (char*)&bus_id[(*id)];
			fs_enet_data.init_ioports = init_fcc_ioports;

			mdio_bb_prop = get_property(phy, "bitbang", NULL);
			if (mdio_bb_prop) {
				struct platform_device *fs_enet_mdio_bb_dev;
				struct fs_mii_bb_platform_info fs_enet_mdio_bb_data;

				fs_enet_mdio_bb_dev =
					platform_device_register_simple("fsl-bb-mdio",
							i, NULL, 0);
				memset(&fs_enet_mdio_bb_data, 0,
						sizeof(struct fs_mii_bb_platform_info));
				fs_enet_mdio_bb_data.mdio_dat.bit =
					mdio_bb_prop[0];
				fs_enet_mdio_bb_data.mdio_dir.bit =
					mdio_bb_prop[1];
				fs_enet_mdio_bb_data.mdc_dat.bit =
					mdio_bb_prop[2];
				fs_enet_mdio_bb_data.mdio_port =
					mdio_bb_prop[3];
				fs_enet_mdio_bb_data.mdc_port =
					mdio_bb_prop[4];
				fs_enet_mdio_bb_data.delay =
					mdio_bb_prop[5];

				fs_enet_mdio_bb_data.irq[0] = phy_irq[0];
				fs_enet_mdio_bb_data.irq[1] = -1;
				fs_enet_mdio_bb_data.irq[2] = -1;
				fs_enet_mdio_bb_data.irq[3] = phy_irq[0];
				fs_enet_mdio_bb_data.irq[31] = -1;

				fs_enet_mdio_bb_data.mdio_dat.offset =
					(u32)&cpm2_immr->im_ioport.iop_pdatc;
				fs_enet_mdio_bb_data.mdio_dir.offset =
					(u32)&cpm2_immr->im_ioport.iop_pdirc;
				fs_enet_mdio_bb_data.mdc_dat.offset =
					(u32)&cpm2_immr->im_ioport.iop_pdatc;

				ret = platform_device_add_data(
						fs_enet_mdio_bb_dev,
						&fs_enet_mdio_bb_data,
						sizeof(struct fs_mii_bb_platform_info));
				if (ret)
					goto unreg;
			}
			
			of_node_put(phy);
			of_node_put(mdio);

			ret = platform_device_add_data(fs_enet_dev, &fs_enet_data,
						     sizeof(struct
							    fs_platform_info));
			if (ret)
				goto unreg;
		}
	}
	return 0;

unreg:
	platform_device_unregister(fs_enet_dev);
err:
	return ret;
}

arch_initcall(fs_enet_of_init);

static const char scc_regs[] = "regs";
static const char scc_pram[] = "pram";

static int __init cpm_uart_of_init(void)
{
	struct device_node *np;
	unsigned int i;
	struct platform_device *cpm_uart_dev;
	int ret;

	for (np = NULL, i = 0;
	     (np = of_find_compatible_node(np, "serial", "cpm_uart")) != NULL;
	     i++) {
		struct resource r[3];
		struct fs_uart_platform_info cpm_uart_data;
		const int *id;
		const char *model;

		memset(r, 0, sizeof(r));
		memset(&cpm_uart_data, 0, sizeof(cpm_uart_data));

		ret = of_address_to_resource(np, 0, &r[0]);
		if (ret)
			goto err;

		r[0].name = scc_regs;

		ret = of_address_to_resource(np, 1, &r[1]);
		if (ret)
			goto err;
		r[1].name = scc_pram;

		r[2].start = r[2].end = irq_of_parse_and_map(np, 0);
		r[2].flags = IORESOURCE_IRQ;

		cpm_uart_dev =
		    platform_device_register_simple("fsl-cpm-scc:uart", i, &r[0], 3);

		if (IS_ERR(cpm_uart_dev)) {
			ret = PTR_ERR(cpm_uart_dev);
			goto err;
		}

		id = get_property(np, "device-id", NULL);
		cpm_uart_data.fs_no = *id;

		model = (char*)get_property(np, "model", NULL);
		strcpy(cpm_uart_data.fs_type, model);

		cpm_uart_data.uart_clk = ppc_proc_freq;

		cpm_uart_data.tx_num_fifo = 4;
		cpm_uart_data.tx_buf_size = 32;
		cpm_uart_data.rx_num_fifo = 4;
		cpm_uart_data.rx_buf_size = 32;
		cpm_uart_data.clk_rx = *((u32 *) get_property(np, "rx-clock", NULL));
		cpm_uart_data.clk_tx = *((u32 *) get_property(np, "tx-clock", NULL));

		ret =
		    platform_device_add_data(cpm_uart_dev, &cpm_uart_data,
					     sizeof(struct
						    fs_uart_platform_info));
		if (ret)
			goto unreg;
	}

	return 0;

unreg:
	platform_device_unregister(cpm_uart_dev);
err:
	return ret;
}

arch_initcall(cpm_uart_of_init);
#endif /* CONFIG_CPM2 */
