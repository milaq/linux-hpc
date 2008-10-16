/*
 * drivers/video/backlight/jornada720_lcd.c
 *
 * LCD display driver for HP Jornada 720
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
#include <linux/lcd.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <asm/hardware.h>
#include <asm/arch/jornada720_mcu.h>
#include <video/s1d13xxxfb.h>

MODULE_AUTHOR("Filip Zyzniewski <filip.zyzniewski@tefnet.pl>");
MODULE_DESCRIPTION("HP Jornada 720 LCD control");
MODULE_LICENSE("GPL");

/*
 * HP Documentation referred in this file:
 * http://www.jlime.com/downloads/development/docs/jornada7xx/jornada720.txt
 */

#define JORNADA720_LCD_MAX_CONTRAST 0xff

/* default contrast (line 457 of HP's doc, but it looks bad with that one) */
#define JORNADA720_LCD_DEFAULT_CONTRAST 0x80

static struct lcd_device *jornada720_lcd_device;

static int jornada720_lcd_set_contrast(struct lcd_device *dev, int contrast)
{
	int ret=0;
	
	/* line 312 of HP's doc */
	if ( !(ret=jornada720_mcu_start(jornada720_mcu_SetContrast)) ) {
		if(jornada720_mcu_byte(contrast) != jornada720_mcu_TxDummy)
			ret = -1;
	} else
		printk(KERN_WARNING "jornada720_lcd: SetContrast failed\n");

	jornada720_mcu_end();

	return ret;
}

static int jornada720_lcd_get_power(struct lcd_device *dev) {
	/* LDD2 in PPC is responsible for LCD power (line 458 of HP's doc */
	if(PPSR & PPC_LDD2)
		return FB_BLANK_UNBLANK;
	else
		return FB_BLANK_POWERDOWN;
}

static int jornada720_lcd_set_power(struct lcd_device *dev, int power) {
	if (power != FB_BLANK_UNBLANK) {
		/* turn off the LCD (line 458 of HP's doc) */
		PPSR &= ~PPC_LDD2;
		PPDR |= PPC_LDD2;
	} else 
		/* turn on the LCD (line 458 of HP's doc) */
		PPSR |= PPC_LDD2;
	return 0;
}

static int jornada720_lcd_get_contrast(struct lcd_device *dev)
{
	int ret;

	/* check if LCD is on (line 458 of HP's doc */
	if(jornada720_lcd_get_power(dev) != FB_BLANK_UNBLANK)
		return 0;

	/* get the data from MCU (line 312 of HP's doc) */
	if(jornada720_mcu_start(jornada720_mcu_GetContrast)) {
		printk(KERN_WARNING "jornada720_lcd: GetContrast failed\n");
		ret=256;
	} else
		ret=jornada720_mcu_read();

	jornada720_mcu_end();

	return ret;
}

static struct lcd_properties jornada720_lcd_data = {
	.owner = THIS_MODULE,
	.max_contrast = JORNADA720_LCD_MAX_CONTRAST,
	.get_contrast = jornada720_lcd_get_contrast,
	.set_contrast = jornada720_lcd_set_contrast,
	.get_power = jornada720_lcd_get_power,
	.set_power = jornada720_lcd_set_power,
};

static int jornada720_lcd_probe(struct device *_dev)
{
	/* 
	 * name must match fb driver name (documentation of
	 * lcd_device_register() in lcd.c
	 */
	jornada720_lcd_device = lcd_device_register (S1D_DEVICENAME,
		NULL, &jornada720_lcd_data);
	if (IS_ERR (jornada720_lcd_device))
		return PTR_ERR (jornada720_lcd_device);

	/* line 457 of HP's doc */
	jornada720_lcd_set_contrast(jornada720_lcd_device, JORNADA720_LCD_DEFAULT_CONTRAST);
	/* line 458 of HP's doc */
	jornada720_lcd_set_power(jornada720_lcd_device, FB_BLANK_UNBLANK);
	/* line 459 of HP's doc */
	msleep(100);

	return 0;
	
}

static int jornada720_lcd_remove(struct device *_dev)
{
	lcd_device_unregister(jornada720_lcd_device);
	return 0;
}

static struct device_driver jornada720_lcd_driver = {
	.name	= jornada720_mcu_bus_id_lcd,
	.bus	= &jornada720_mcu_bus_type,
	.probe	= jornada720_lcd_probe,
	.remove	= jornada720_lcd_remove,
	.owner	= THIS_MODULE
};

static int __init jornada720_lcd_init(void)
{
	return driver_register(&jornada720_lcd_driver);
}

static void __exit jornada720_lcd_exit(void)
{
	driver_unregister(&jornada720_lcd_driver);
}

module_init(jornada720_lcd_init);
module_exit(jornada720_lcd_exit);
