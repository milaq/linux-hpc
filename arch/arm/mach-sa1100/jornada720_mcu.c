/*
 * arch/arm/mach-sa1100/jornada720_mcu.c
 *
 * HP Jornada 720 Microprocessor Control Unit driver
 *
 * Copyright (C) 2006 Filip Zyzniewski <filip.zyzniewski@tefnet.pl>
 *  Copyright (C) 2005 Michael Gernoth <michael@gernoth.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#include <asm/hardware.h>
#include <asm/arch/jornada720_mcu.h>

/* TODO:
 * when CONFIG_SA1100_SSP will be definable and ssp header will be present,
 * CONFIG_SA1100_SSP should be added to this module's dependencies and this
 * include should be removed in favour of ssp header file
 */
#include "ssp.c"

MODULE_AUTHOR("Filip Zyzniewski <filip.zyzniewski@tefnet.pl>");
MODULE_DESCRIPTION("HP Jornada 720 Microprocessor Controller Unit driver");
MODULE_LICENSE("GPL");

/*
 * HP Documentation referred in this file:
 * http://www.jlime.com/downloads/development/docs/jornada7xx/jornada720.txt
 */


/* 
 * we have to lock to avoid starting
 * second transmission during one taking place 
 */
static spinlock_t jornada720_mcu_lock = SPIN_LOCK_UNLOCKED;
static unsigned long jornada720_mcu_flags;


u8 inline jornada720_mcu_reverse(u8 x)
{
	/* byte ghijklmn becomes nmlkjihg (line 205 of HP's doc) */
	return 
		((0x80 & x) >> 7) |
		((0x40 & x) >> 5) |
		((0x20 & x) >> 3) |
		((0x10 & x) >> 1) |
		((0x08 & x) << 1) |
		((0x04 & x) << 3) |
		((0x02 & x) << 5) |
		((0x01 & x) << 7) ;
}


int jornada720_mcu_byte(u8 byte)
{
	u16 ret;
	int timeout = 400000;

	/* wating for GPIO 10 to go low (line 197 of HP's doc) */
	while ((GPLR & GPIO_GPIO10)) {
		if (!--timeout) {
			printk("jornada720_mcu_byte ret -ETIMEDOUT\n");
			return -ETIMEDOUT;
		}
		cpu_relax();
	}

	ret=jornada720_mcu_reverse(byte) << 8;

	ssp_write_word(ret);
	ssp_read_word(&ret);

	return jornada720_mcu_reverse(ret);
}

EXPORT_SYMBOL(jornada720_mcu_byte);


/* 
 * WARNING: remember to jornada720_mcu_end() after every
 * jornada720_mcu_start() or you will deadlock!
 */

int jornada720_mcu_start(u8 byte)
{
	int i;
	
	/* we don't want other access to the MCU now */
	spin_lock_irqsave(&jornada720_mcu_lock, jornada720_mcu_flags);
	
	/* clear to enable MCU (line 194 of HP's doc) */
	GPCR = GPIO_GPIO25;

	/* we should always get TxDummy after first request */
	if (jornada720_mcu_byte(byte) != jornada720_mcu_TxDummy)
	{
		printk(KERN_WARNING "jornada720_mcu: leftover MCU data, flushing\n");
		for (i = 0; i < 256; i++)
			if (jornada720_mcu_read() == -1)
				break;
		return -1;
	}

	return 0;
}

EXPORT_SYMBOL(jornada720_mcu_start);


void jornada720_mcu_end(void)
{
	/* end of transmission (line 203 of HP's doc) */
	GPSR = GPIO_GPIO25;
	spin_unlock_irqrestore(&jornada720_mcu_lock, jornada720_mcu_flags);
}

EXPORT_SYMBOL(jornada720_mcu_end);


/* Jornada 720 devices accessed by the MCU */
static struct device jornada720_mcu_bus_devices[] = {
	/* keyboard */
	{ .bus_id = jornada720_mcu_bus_id_kbd },

	/* touchscreen */
	{ .bus_id = jornada720_mcu_bus_id_ts },

	/* apm (batteries) */
	{ .bus_id = jornada720_mcu_bus_id_apm },

	/* backlight */
	{ .bus_id = jornada720_mcu_bus_id_bl },

	/* lcd display */
	{ .bus_id = jornada720_mcu_bus_id_lcd },
};

static int jornada720_mcu_bus_match(struct device *dev, struct device_driver *drv)
{
	return ! strcmp(dev->bus_id, drv->name);
}

struct bus_type jornada720_mcu_bus_type = {
	.name		= "jornada720_mcu_bus",
	.match		= jornada720_mcu_bus_match,
};

EXPORT_SYMBOL(jornada720_mcu_bus_type);


void jornada720_mcu_bus_device_release(struct device *dev)
{
	/*
	 * those device structures are static, so we don't
	 * have anything to free
	 */
}

static int __init jornada720_mcu_probe(struct platform_device *dev)
{
	int ret, i;
	
	/* we don't want any data yet (line 203 of HP's doc) */
	GPSR = GPIO_GPIO25;
	ret=ssp_init();
	if (ret)
		return ret;
	
	/* initialization of MCU serial interface (line 182 of HP's doc) */
	Ser4MCCR0 = 0;
	Ser4SSCR0 = 0x0387;
	Ser4SSCR1 = 0x18;

	ssp_flush();

	/* test of MCU presence */
	ret = jornada720_mcu_start(jornada720_mcu_GetBrightness);
	if(!ret) jornada720_mcu_read();
	jornada720_mcu_end();

	if(ret < 0) {
		ssp_exit();
		return -ENODEV;
	}

	/* registering devices accessible through this bus */
	for(i=0; i<ARRAY_SIZE(jornada720_mcu_bus_devices); i++) {
		jornada720_mcu_bus_devices[i].bus = &jornada720_mcu_bus_type;
		jornada720_mcu_bus_devices[i].parent = &(dev->dev);
		jornada720_mcu_bus_devices[i].release = jornada720_mcu_bus_device_release;
		ret = device_register(jornada720_mcu_bus_devices + i);
		if (ret)
			break;
	}

	if (ret) {
		for(; i <= 0; i--) {
			device_unregister(jornada720_mcu_bus_devices + i);
		}
		ssp_exit();
	}
	
	return ret;
}

static int jornada720_mcu_remove(struct platform_device *dev)
{
	int i;
	for(i=0; i<ARRAY_SIZE(jornada720_mcu_bus_devices); i++)
		device_unregister(jornada720_mcu_bus_devices + i);
	/* we don't want data anymore (line 203 of HP's doc) */
	GPSR = GPIO_GPIO25;
	ssp_exit();
	return 0;
}

struct platform_driver jornada720_mcu_driver = {
	.probe = jornada720_mcu_probe,
	.remove = jornada720_mcu_remove,
	.driver = {
		.name = "jornada720_mcu",
	},
};

static int __init jornada720_mcu_init(void) {
	int ret = bus_register(&jornada720_mcu_bus_type);
	if (!ret)
		return platform_driver_register(&jornada720_mcu_driver);
	return ret;
}

static void __exit jornada720_mcu_exit(void) {
	bus_unregister(&jornada720_mcu_bus_type);
	platform_driver_unregister(&jornada720_mcu_driver);
}

module_init(jornada720_mcu_init);
module_exit(jornada720_mcu_exit);
