/*
 * drivers/input/keyboard/jornada720_kbd.c
 *
 * Jornada 720 keyboard interface
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
MODULE_DESCRIPTION("Jornada 720 keyboard driver");
MODULE_LICENSE("GPL");

/*
 * HP Documentation referred in this file:
 * http://www.jlime.com/downloads/development/docs/jornada7xx/jornada720.txt
 */
	
static struct input_dev *dev;

static char jornada720_kbd_name[] = "Jornada 720 keyboard";

/* line 227 of HP's doc */
static unsigned char jornada720_normal_keymap[128] = {
	0, 1, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE,
	0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 0, 0,
	0, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 43, 14, 0, 0, 0,
	0, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, KEY_LEFTBRACE, KEY_RIGHTBRACE, 0, 0, 0,
	0, 44, 45, 46, 47, 48, 49, 50, 51, 52, KEY_KPMINUS, 40, 28, 0, 0, 0,
	0, 15, 0, 42, 0, 40, 0, 0, 0, 0, 103, 0, 54, 0, 0, 0,
	0, 0, 0, 0, 0, 56, KEY_GRAVE, 0, 0, 105, 108, 106, 0, 0, 0, 0,
	0, 55, 29, 0, 57, 0, 0, 0, 53, 111, 0, 0, 0, 0, 0, 116,
};

static irqreturn_t jornada720_kbd_interrupt(int irq, void *dev_id)
{
	int key, keycode;
	int count, mcu_data=0;

	/* start of dialogue with the MCU (line 217 of HP's doc) */
	if(jornada720_mcu_start(jornada720_mcu_GetScanKeyCode)) {
		printk(KERN_WARNING "jornada720_kbd: GetScanKeyCode failed\n");
		jornada720_mcu_end();
		return IRQ_HANDLED;
	}

	/* amount of key events (line 218 of HP's doc) */
	count = jornada720_mcu_read();

	while (count-- > 0) {
		/* keycode (line 219 of HP's doc) */
		key = mcu_data = jornada720_mcu_read();

		if (key < 0) {
			jornada720_mcu_end();
			return IRQ_HANDLED;
		}

		/* scancode > 128 means key release (line 223 of HP's doc) */
		if (key > 128)
			key = key - 128;

		keycode = jornada720_normal_keymap[key];

		if (mcu_data < 128) {
			/* key pressed (line 223 of HP's doc) */
			input_report_key(dev, keycode, 1);
			input_sync(dev);
		}
		else {
			/* key released (line 223 of HP's doc) */
			input_report_key(dev, keycode, 0);
			input_sync(dev);
		}
	}
	

	jornada720_mcu_end();

	return IRQ_HANDLED;
}

static int jornada720_kbd_probe(struct device *_dev)
{
	int i, ret;
	
	dev = input_allocate_device();
	if (!dev)
		return -ENOMEM;
	
	dev->evbit[0] = BIT(EV_KEY) | BIT(EV_REP);
	dev->keybit[LONG(KEY_SUSPEND)] |= BIT(KEY_SUSPEND);

	for ( i=0 ; i<=128 ; i++ ) {
		if (jornada720_normal_keymap[i])
			set_bit(jornada720_normal_keymap[i], dev->keybit);
	}

	dev->name = jornada720_kbd_name;

	/* keyboard is on GPIO0 (line 215 of HP's doc) */
	ret = request_irq(IRQ_GPIO0,
			jornada720_kbd_interrupt,
			IRQF_DISABLED | IRQF_TRIGGER_FALLING,
			jornada720_kbd_name, dev);
	if (ret) {
		printk(KERN_WARNING "Unable to grab IRQ for %s: %d\n", jornada720_kbd_name, ret);
		input_free_device(dev);
		return ret;
	}

	input_register_device(dev);

	return 0;

}

static int jornada720_kbd_remove(struct device *_dev)
{
	free_irq(IRQ_GPIO0, dev);
	input_unregister_device(dev);
	return 0;
}

static struct device_driver jornada720_kbd_driver = {
	.name	= jornada720_mcu_bus_id_kbd,
	.bus	= &jornada720_mcu_bus_type,
	.probe	= jornada720_kbd_probe,
	.remove	= jornada720_kbd_remove,
	.owner	= THIS_MODULE
};

static int __init jornada720_kbd_init(void)
{
	return driver_register(&jornada720_kbd_driver);
}

static void __exit jornada720_kbd_exit(void)
{
	driver_unregister(&jornada720_kbd_driver);
}

module_init(jornada720_kbd_init);
module_exit(jornada720_kbd_exit);
