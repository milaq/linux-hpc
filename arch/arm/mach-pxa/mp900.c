/*
 *  linux/arch/arm/mach-pxa/mp900.c
 *
 *  Support for the NEC MobilePro900/C platform
 *
 *  this really does nothing much so far...
 *  it was derived from a gumstix mach-pxa/gumstix.c
 *
 *  this is where the machine specific initialisation for the
 *  MobilePro900/c should happen
 *
 *  TODO anything and everything...
 *
 *  Michael Petchkovsky mkpetch@internode.on.net
 *  17 April 2007
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#include <asm/types.h>

#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/usb/isp116x.h>

#include <asm/hardware.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/arch/pxa-regs.h>

/* asm/arch/mp900c.h would be a good idea... */

#include "generic.h"

#define IRQ_USB	PXA_IRQ(11)

static void isp116x_pfm_delay(struct device *dev, int delay)
{
}

static struct isp116x_platform_data isp116x_pfm_data = {
    .remote_wakeup_enable = 1,
    .delay = isp116x_pfm_delay,
};

static struct resource isp116x_pfm_resources[] = {
	[0] = {
		.start	= 0x4c000000,
		.end	= 0x4c100000,
		.flags	= IORESOURCE_MEM,
	       },
//	[1] = {
//		.start	= ,
//		.end	= ,
//		.flags	= IORESOURCE_MEM,
//	      },
	[2] = {
		.start	= IRQ_USB,
		.end	= IRQ_USB,
		.flags	= IORESOURCE_IRQ,
	      },
};

static struct platform_device mp900c_dummy_device = {
	.name		= "mp900c_dummy",
	.id		= -1,
};

static struct platform_device mp900c_usb = {
	.name		= "isp116x-hcd",
	.num_resources	= ARRAY_SIZE(isp116x_pfm_resources),
	.resource	= isp116x_pfm_resources,
	.dev.platform_data = &isp116x_pfm_data,
};

static struct platform_device *devices[] __initdata = {
	&mp900c_dummy_device,
	&mp900c_usb,
};


static void __init mp900c_init(void)
{
	printk (KERN_NOTICE "MobilePro900C init routine\n");
	(void) platform_add_devices(devices, ARRAY_SIZE(devices));
}

MACHINE_START(NEC_MP900, "MobilePro900C")
//	.phys_ram	= 0xa0000000,
	.phys_io	= 0x40000000,
	.boot_params	= 0xa0220100,
	.io_pg_offst	= (io_p2v(0x40000000) >> 18) & 0xfffc,
	.timer          = &pxa_timer,
	.map_io		= pxa_map_io,
	.init_irq	= pxa_init_irq,  //pxa25x_init_irq,
	.init_machine	= mp900c_init,
MACHINE_END
