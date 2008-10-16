/*
 * drivers/video/backlight/jornada720_bl.c
 *
 * Backlight Driver for HP Jornada 720
 *
 * Copyright (c) 2006 Filip Zyzniewski <filip.zyzniewski@tefnet.pl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/device.h>
#include <asm/hardware.h>
#include <asm/arch/jornada720_mcu.h>
#include <video/s1d13xxxfb.h>

MODULE_AUTHOR("Filip Zyzniewski <filip.zyzniewski@tefnet.pl>");
MODULE_DESCRIPTION("HP Jornada 720 Backlight Driver");
MODULE_LICENSE("GPL");

/*
 * HP Documentation referred in this file:
 * http://www.jlime.com/downloads/development/docs/jornada7xx/jornada720.txt
 */

#define JORNADA720_BL_MAX_BRIGHTNESS 0xff

/* default brightness (line 460 of HP's doc - it's
 * mistakenly referred as default contrast there) */
#define JORNADA720_BL_DEFAULT_BRIGHTNESS 0x19

static struct backlight_device *jornada720_bl_device;

static int jornada720_bl_get_brightness(struct backlight_device *dev)
{
	int ret;

	/* check if backlight is on (line 461 of HP's doc */
	if(!(PPSR & PPC_LDD1))
		return 255;

	/* get data from the MCU (line 310 of HP's doc) */
	if(jornada720_mcu_start(jornada720_mcu_GetBrightness)) {
		printk(KERN_WARNING "jornada720_bl: GetBrightness failed\n");
		ret=256;
	} else
		ret=jornada720_mcu_read();

	jornada720_mcu_end();

	/* 0 is max brightness for the kernel, opposite for the MCU */
	return 255-ret;
}

static int jornada720_bl_update_status(struct backlight_device *dev)
{
	int ret=0;


	if (dev->props->power != FB_BLANK_UNBLANK || dev->props->fb_blank != FB_BLANK_UNBLANK) {
		/* turn off the backlight PWM (line 313 of HP's doc) */
		ret=jornada720_mcu_start(jornada720_mcu_BrightnessOff);
		if(ret)
			printk(KERN_WARNING "jornada720_bl: BrightnessOff failed\n");
		/* turn off the backlight (line 461 of HP's doc) */
		PPSR &= ~PPC_LDD1;
		PPDR |= PPC_LDD1;
	}
	else {
		/* turn the backlight on (line 461 of HP's doc) */
		PPSR |= PPC_LDD1;
		/* line 309 of HP's doc */
		if (!(ret=jornada720_mcu_start(jornada720_mcu_SetBrightness))) {
			/* 0 is max brightness for the kernel, opposite for the MCU */
			if(jornada720_mcu_byte(255 - dev->props->brightness) != jornada720_mcu_TxDummy)
				ret = -1;
		} else
			printk(KERN_WARNING "jornada720_bl: SetBrightness failed\n");
	}

	jornada720_mcu_end();

	return ret;
}


static struct backlight_properties jornada720_bl_data = {
	.owner		= THIS_MODULE,
	.max_brightness = JORNADA720_BL_MAX_BRIGHTNESS,
	.get_brightness = jornada720_bl_get_brightness,
	.update_status	= jornada720_bl_update_status,
};

static int jornada720_bl_probe(struct device *_dev)
{
	/* 
	 * name must match fb driver name (documentation of
	 * backlight_device_register() in backlight.c
	 */
	jornada720_bl_device = backlight_device_register (S1D_DEVICENAME,
		NULL, &jornada720_bl_data);
	if (IS_ERR (jornada720_bl_device))
		return PTR_ERR (jornada720_bl_device);

	jornada720_bl_data.power = FB_BLANK_UNBLANK;
	jornada720_bl_data.brightness = JORNADA720_BL_DEFAULT_BRIGHTNESS;
	jornada720_bl_update_status(jornada720_bl_device);

	return 0;
	
}

static int jornada720_bl_remove(struct device *_dev)
{
	backlight_device_unregister(jornada720_bl_device);
	return 0;
}

static struct device_driver jornada720_bl_driver = {
	.name	= jornada720_mcu_bus_id_bl,
	.bus	= &jornada720_mcu_bus_type,
	.probe	= jornada720_bl_probe,
	.remove = jornada720_bl_remove,
	.owner	= THIS_MODULE
};

static int __init jornada720_bl_init(void)
{
	return driver_register(&jornada720_bl_driver);
}

static void __exit jornada720_bl_exit(void)
{
	driver_unregister(&jornada720_bl_driver);
}

module_init(jornada720_bl_init);
module_exit(jornada720_bl_exit);
