/*
 * drivers/input/touchscreen/jornada720_ts.c
 *
 * Jornada 720 touchscreen interface
 *
 * Copyright (C) 2006 Filip Zyzniewski <filip.zyzniewski@tefnet.pl>
 *  Copyright (C) 2004 Alex Lange <chicken@handhelds.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/device.h>

#include <asm/hardware.h>
#include <asm/arch/jornada720_mcu.h>

MODULE_AUTHOR("Alex Lange <chicken@handhelds.org>");
MODULE_DESCRIPTION("Jornada 720 touchscreen driver");
MODULE_LICENSE("GPL");

/*
 * HP Documentation referred in this file:
 * http://www.jlime.com/downloads/development/docs/jornada7xx/jornada720.txt
 */

static struct input_dev *dev;

static char jornada720_ts_name[] = "Jornada 720 touchscreen";


static irqreturn_t jornada720_ts_interrupt(int irq, void *dev_id)
{
	int X[3], Y[3], high_x, high_y, x, y;

	/* check if pen is up (line 264 of HP's doc) */
	if(GPLR & GPIO_GPIO(9)) {
		/* report pen up */
		input_report_key(dev, BTN_TOUCH, 0);
		input_report_abs(dev, ABS_PRESSURE, 0);
		input_sync(dev);

		return IRQ_HANDLED;
	}

	/* 
	 * read x & y data from mcu interface and
	 * pass it on (line 265 of HP's doc)
	 */
	if(jornada720_mcu_start(jornada720_mcu_GetTouchSamples)) {
		jornada720_mcu_end();
		return IRQ_HANDLED;
	}

	/* 
	 * beware: HP's doc specifies incorrect order of sample
	 * bytes, MSBs for X and Y are the last two ones.
	 */

	/* LSBs for X (line 272 of HP's doc */
	X[0] = jornada720_mcu_read();
	X[1] = jornada720_mcu_read();
	X[2] = jornada720_mcu_read();
	
	/* LSBs for Y (line 276 of HP's doc */
	Y[0] = jornada720_mcu_read();
	Y[1] = jornada720_mcu_read();
	Y[2] = jornada720_mcu_read();
	
	/* MSBs for X (line 275 of HP's doc */
	high_x = jornada720_mcu_read();

	/* MSBs for Y (line 279 of HP's doc) */
	high_y = jornada720_mcu_read();

	jornada720_mcu_end();

	/* calculating actual values (line 281 of HP's doc) */
	X[0] |= (high_x & 3) << 8;
	X[1] |= (high_x & 0xc) << 6;
	X[2] |= (high_x & 0x30) << 4;

	Y[0] |= (high_y & 3) << 8;
	Y[1] |= (high_y & 0xc) << 6;
	Y[2] |= (high_y & 0x30) << 4;

	/* simple averaging filter */
	x = (X[0] + X[1] + X[2])/3;
	y = (Y[0] + Y[1] + Y[2])/3;

	/* report pen down */
	input_report_key(dev, BTN_TOUCH, 1);
	input_report_abs(dev, ABS_X, x);
	input_report_abs(dev, ABS_Y, y);
	input_report_abs(dev, ABS_PRESSURE, 1);
	input_sync(dev);

	return IRQ_HANDLED;

}


static int jornada720_ts_probe(struct device *_dev)
{
	int ret;

	dev = input_allocate_device();
	if (!dev)
		return -ENOMEM;

	dev->evbit[0] = BIT(EV_KEY) | BIT(EV_ABS);
	dev->absbit[0] = BIT(ABS_X) | BIT(ABS_Y) | BIT(ABS_PRESSURE);
	dev->keybit[LONG(BTN_TOUCH)] = BIT(BTN_TOUCH);

	dev->absmin[ABS_X] = 270; dev->absmin[ABS_Y] = 180;
	dev->absmax[ABS_X] = 3900; dev->absmax[ABS_Y] = 3700;

	dev->name = jornada720_ts_name;

	/*
	 * touchscreen is on GPIO9 (Line 263 of HP's doc. It says we should
	 * care for falling edge, but in fact we need a rising edge.)
	 */
	ret = request_irq(IRQ_GPIO9,
			jornada720_ts_interrupt,
			IRQF_DISABLED | IRQF_TRIGGER_RISING,
			jornada720_ts_name, dev);
	if (ret) {
		printk("Unable to grab IRQ for %s: %d\n", jornada720_ts_name, ret);
		input_free_device(dev);
		return ret;
	}

	input_register_device(dev);

	return 0;
}


static int jornada720_ts_remove(struct device *_dev)
{
	free_irq(IRQ_GPIO9, dev);
	input_unregister_device(dev);
	return 0;
}

static struct device_driver jornada720_ts_driver = {
	.name	= jornada720_mcu_bus_id_ts,
	.bus	= &jornada720_mcu_bus_type,
	.probe	= jornada720_ts_probe,
	.remove	= jornada720_ts_remove,
	.owner	= THIS_MODULE
};

static int __init jornada720_ts_init(void)
{
	return driver_register(&jornada720_ts_driver);
}

static void __exit jornada720_ts_exit(void)
{
	driver_unregister(&jornada720_ts_driver);
}

module_init(jornada720_ts_init);
module_exit(jornada720_ts_exit);
