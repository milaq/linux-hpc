/*
 * linux/arch/arm/mach-sa1100/jornada720.c
 *
 * HP Jornada720 init code
 *
 * Copyright (C) 2006 Filip Zyzniewski <filip.zyzniewski@tefnet.pl>
 *  Copyright (C) 2005 Michael Gernoth <michael@gernoth.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <video/s1d13xxxfb.h>

#include <asm/hardware.h>
#include <asm/hardware/sa1111.h>
#include <asm/irq.h>
#include <asm/mach-types.h>
#include <asm/setup.h>
#include <asm/s1d13506.h>
#include <asm/mach/arch.h>
#include <asm/mach/flash.h>
#include <asm/mach/map.h>
#include <asm/mach/serial_sa1100.h>

#include "generic.h"

/*
 * HP Documentation referred in this file:
 * http://www.jlime.com/downloads/development/docs/jornada7xx/jornada720.txt
 */

/* line 110 of HP's doc */
#define TUCR_VAL	0x20000400

/* memory space (line 52 of HP's doc) */
#define SA1111REGSTART	0x40000000
#define SA1111REGLEN	0x00001fff
#define EPSONREGSTART	0x48000000
#define EPSONREGLEN	0x00100000
#define EPSONFBSTART	0x48200000
/* 512kB framebuffer */
#define EPSONFBLEN	512*1024

static struct s1d13xxxfb_pdata s1d13xxxfb_data = {
	.initregs		= s1d13xxxfb_initregs,
	.initregssize		= ARRAY_SIZE(s1d13xxxfb_initregs),
	.platform_init_video	= NULL
};

static struct resource s1d13xxxfb_resources[] = {
	[0] = {
		.start	= EPSONFBSTART,
		.end	= EPSONFBSTART + EPSONFBLEN,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= EPSONREGSTART,
		.end	= EPSONREGSTART + EPSONREGLEN,
		.flags	= IORESOURCE_MEM,
	}
};

static struct platform_device s1d13xxxfb_device = {
	.name		= S1D_DEVICENAME,
	.id		= 0,
	.dev		= {
		.platform_data	= &s1d13xxxfb_data,
	},
	.num_resources	= ARRAY_SIZE(s1d13xxxfb_resources),
	.resource	= s1d13xxxfb_resources,
};

static struct resource sa1111_resources[] = {
	[0] = {
		.start		= SA1111REGSTART,
		.end		= SA1111REGSTART + SA1111REGLEN,
		.flags		= IORESOURCE_MEM,
	},
	[1] = {
		.start		= IRQ_GPIO1,
		.end		= IRQ_GPIO1,
		.flags		= IORESOURCE_IRQ,
	},
};

static u64 sa1111_dmamask = 0xffffffffUL;

static struct platform_device sa1111_device = {
	.name		= "sa1111",
	.id		= 0,
	.dev		= {
		.dma_mask = &sa1111_dmamask,
		.coherent_dma_mask = 0xffffffff,
	},
	.num_resources	= ARRAY_SIZE(sa1111_resources),
	.resource	= sa1111_resources,
};

static struct platform_device jornada720_mcu_device = {
	.name		= "jornada720_mcu",
	.id		= -1,
};

static struct platform_device *devices[] __initdata = {
	&sa1111_device,
	&jornada720_mcu_device,
	&s1d13xxxfb_device,
};

/* a stub for now, we theoretically cannot suspend without a flashboard */
int pm_suspend(suspend_state_t state)
{
	return -1;
}

static int __init jornada720_init(void)
{
	int ret = -ENODEV;

	if (machine_is_jornada720()) {
		GPDR |= GPIO_GPIO20;
		/* oscillator setup (line 116 of HP's doc) */
		TUCR = TUCR_VAL;
		/* resetting SA1111 (line 118 of HP's doc) */
		GPSR = GPIO_GPIO20;
		udelay(1);
		GPCR = GPIO_GPIO20;
		udelay(1);
		GPSR = GPIO_GPIO20;
		udelay(20);

		ret = platform_add_devices(devices, ARRAY_SIZE(devices));
	}
	return ret;
}

arch_initcall(jornada720_init);

static struct map_desc jornada720_io_desc[] __initdata = {
	{	/* Epson registers */
		.virtual	= 0xf0000000,
		.pfn		= __phys_to_pfn(EPSONREGSTART),
		.length		= EPSONREGLEN,
		.type		= MT_DEVICE
	}, {	/* Epson frame buffer */
		.virtual	= 0xf1000000,
		.pfn		= __phys_to_pfn(EPSONFBSTART),
		.length		= EPSONFBLEN,
		.type		= MT_DEVICE
	}, {	/* SA-1111 */
		.virtual	= 0xf4000000,
		.pfn		= __phys_to_pfn(SA1111REGSTART),
		.length		= SA1111REGLEN,
		.type		= MT_DEVICE
	}
};

static void __init jornada720_map_io(void)
{
	sa1100_map_io();
	iotable_init(jornada720_io_desc, ARRAY_SIZE(jornada720_io_desc));

	sa1100_register_uart(0, 3);
	sa1100_register_uart(1, 1);
}

static struct mtd_partition jornada720_partitions[] = {
	{
		.name		= "JORNADA720 boot firmware",
		.size		= 0x00040000,
		.offset		= 0,
		.mask_flags	= MTD_WRITEABLE, /* force read-only */
	}, {
		.name		= "JORNADA720 kernel",
		.size		= 0x000c0000,
		.offset		= 0x00040000,
	}, {
		.name		= "JORNADA720 params",
		.size		= 0x00040000,
		.offset		= 0x00100000,
	}, {
		.name		= "JORNADA720 initrd",
		.size		= 0x00100000,
		.offset		= 0x00140000,
	}, {
		.name		= "JORNADA720 root cramfs",
		.size		= 0x00300000,
		.offset		= 0x00240000,
	}, {
		.name		= "JORNADA720 usr cramfs",
		.size		= 0x00800000,
		.offset		= 0x00540000,
	}, {
		.name		= "JORNADA720 usr local",
		.size		= 0, /* will expand to the end of the flash */
		.offset		= 0x00d00000,
	}
};

static void jornada720_set_vpp(int vpp)
{
	if (vpp)
		/* enabling flash write (line 470 of HP's doc) */
		PPSR |= PPC_LDD7;
	else
		/* disabling flash write (line 470 of HP's doc) */
		PPSR &= ~PPC_LDD7;
	PPDR |= PPC_LDD7;
}

static struct flash_platform_data jornada720_flash_data = {
	.map_name	= "cfi_probe",
	.set_vpp	= jornada720_set_vpp,
	.parts		= jornada720_partitions,
	.nr_parts	= ARRAY_SIZE(jornada720_partitions),
};

static struct resource jornada720_flash_resource = {
	.start		= SA1100_CS0_PHYS,
	.end		= SA1100_CS0_PHYS + SZ_32M - 1,
	.flags		= IORESOURCE_MEM,
};

static void __init jornada720_mach_init(void)
{
	sa11x0_set_flash_data(&jornada720_flash_data, &jornada720_flash_resource, 1);
}

MACHINE_START(JORNADA720, "HP Jornada 720")
	/* Maintainer: Michael Gernoth <michael@gernoth.net> */
	.phys_io	= 0x80000000,
	.io_pg_offst	= ((0xf8000000) >> 18) & 0xfffc,
	.boot_params	= 0xc0000100,
	.map_io		= jornada720_map_io,
	.init_irq	= sa1100_init_irq,
	.timer		= &sa1100_timer,
	.init_machine	= jornada720_mach_init,
MACHINE_END
