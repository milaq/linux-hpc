/* linux/drivers/input/keyboard/mp900_kbd.c
 *
 * input driver for the NEC MobilePro900/c keyboard and touchscreen
 *
 * keyboard and touchscreen data comes into the processor over
 * pxa's BTUART
 *
 * on keypress 0x12 is received, use this for interrupt
 * then poll by sending 0x13 over BTUART waiting for key up
 * and to pick up modifier key combos
 *
 * on touchscreen event (stylus touched to screen) a stream of
 * reporting comes through, first byte 0x04 then two bytes X position
 * two bytes Y posn - continually until stylus is lifted, then single
 * byte 0x05 indicates event end
 *
 * we get interrupt on character timeout, ie end of received string
 * and we're looking for complete, discrete packets (BTUART FIFO holds
 * up to 64 bytes)
 *
 * ignore and discard anything that doesn't read as a clean packet
 *
 * a little more info on the protocol as it's undocumented AFAIK
 *  	-it's a 2 way protocol, send and receive over BTUART
 *  	-when you send 0x13 you receive back a string like
 *  	-0x13 0xff 0xff 0xff 0xff 0xff 0xff 0xff 0xff 0xff 0xff 0xff 0xff 0xff
 *  	-single zero bits in the 0xff's indicate a key pressed or held
 *  	-this way presumably any combination of held keys can be reported
 *  	-all bits return to 1 when all keys are released
 *  	-no doubt more 'commands' are possible than just 0x13 !!
 *
 * thanks to cmonex and friends for recognising that the keyboard is
 * on BTUART and providing the initial reverse-engineering
 * to decode the protocol
 *
 * and to TyrianDreams for recognising that the PIC chip on the motherboard
 * was an ideal candidate to be doing serial comms ... all the pieces fall
 * in to place :)
 *
 * with reference to jlime's Jornada720 keyboard code
 * 	drivers/input/keyboard/jornada720_kbd.c
 * and pxa-serial driver
 * 	drivers/serial/pxa.c
 *
 * Michael Petchkovsky mkpetch@internode.on.net May 2007
 */

/* TODO implement a watchdog while touchscreen event on in case we miss end ?*/
/* TODO revisit the way you do all those readl() and writel()'s */
/* TODO trim comments ruthlessly ;) info on protocol
 * could go in Documentation/
 */

/* TODO probably some unnescessary includes here */
#include <linux/input.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/arch-pxa/pxa-regs.h>
#include <asm/arch-pxa/irqs.h>
#include <asm/arch-pxa/hardware.h>

MODULE_AUTHOR("Michael Petchkovsky");
MODULE_DESCRIPTION("MobilePro900/c keyboard driver");
MODULE_LICENSE("GPL");

#define PFX "mp900_keyboard: "

static char mp900_kb_name[] = "MobilePro900/c keyboard";
static char mp900_ts_name[] = "MobilePro900/c touchscreen";

static void mp900_kb_poll(struct work_struct *work);

static struct input_dev *dev_kb, *dev_ts;
static struct workqueue_struct *mp900_kb_workqueue;

static DECLARE_WORK(mp900_kb_work, mp900_kb_poll, 0);

/* TODO LCD closure button generates a keycode,
 * determine where it goes in here
 */

/* TODO : allocate memory for keymap so we can change it at runtime */
static unsigned int mp900_keymap[128] = {
	0, 0, KEY_F1, KEY_F5, KEY_F9, KEY_FN, KEY_ESC,
	KEY_1, KEY_9, KEY_Q, KEY_A, KEY_Z, KEY_O, KEY_L, 0, 0, 0, 0,
	KEY_F2, KEY_F6, KEY_F10, KEY_TAB, KEY_DELETE, KEY_2, KEY_0,
	KEY_W, KEY_S, KEY_X, KEY_DOT, KEY_ENTER, 0, 0, 0, 0, KEY_F3,
	KEY_F7, KEY_BRIGHTNESSUP, KEY_P, KEY_CAPSLOCK, KEY_3, 0,  KEY_E,
	KEY_D, KEY_C, KEY_DOWN, KEY_RIGHT, 0, 0, 0, 0, KEY_F4, KEY_F8,
	KEY_BRIGHTNESSDOWN, KEY_BACKSPACE, 0, KEY_4, 0, KEY_R, KEY_F, KEY_V,
	KEY_UP, KEY_LEFT, 0, 0, 0, 0, KEY_LEFTSHIFT, 0, 0, 0, KEY_T, KEY_G,
	KEY_B, KEY_5, KEY_GRAVE, KEY_MSDOS, KEY_SEMICOLON, KEY_SLASH, 0, 0,
	0, 0, 0, KEY_LEFTCTRL, 0, 0, KEY_Y, KEY_H, KEY_N, KEY_6, KEY_MINUS,
	0, KEY_APOSTROPHE, KEY_BACKSLASH, 0, 0, 0, 0, 0, 0, KEY_LEFTALT,
	0, KEY_U, KEY_J, KEY_M, KEY_7, KEY_EQUAL, 0, KEY_LEFTBRACE,
	KEY_RIGHTBRACE, 0, 0, 0, 0, 0, 0, 0, KEY_RIGHTALT, KEY_I, KEY_K,
	KEY_COMMA, KEY_8, 0, 0, 0, KEY_SPACE, KEY_POWER, 0
};
/* trialing a different keymapping where special function keys
 * are read as keys F1-F10 above
 * 
 * below is the array where special function keys are 'special functions'
 *
static unsigned int mp900_keymap[128] = {
	0, 0, KEY_MAIL, KEY_CYCLEWINDOWS, KEY_RECORD, KEY_FN, KEY_ESC,
	KEY_1, KEY_9, KEY_Q, KEY_A, KEY_Z, KEY_O, KEY_L, 0, 0, 0, 0,
	KEY_WWW, KEY_PROG1, KEY_CALC, KEY_TAB, KEY_DELETE, KEY_2, KEY_0,
	KEY_W, KEY_S, KEY_X, KEY_DOT, KEY_ENTER, 0, 0, 0, 0, KEY_CALENDAR,
	KEY_PROG2, KEY_BRIGHTNESSUP, KEY_P, KEY_CAPSLOCK, KEY_3, 0,  KEY_E,
	KEY_D, KEY_C, KEY_DOWN, KEY_RIGHT, 0, 0, 0, 0, KEY_EMAIL, KEY_PROG3,
	KEY_BRIGHTNESSDOWN, KEY_BACKSPACE, 0, KEY_4, 0, KEY_R, KEY_F, KEY_V,
	KEY_UP, KEY_LEFT, 0, 0, 0, 0, KEY_LEFTSHIFT, 0, 0, 0, KEY_T, KEY_G,
	KEY_B, KEY_5, KEY_GRAVE, KEY_MSDOS, KEY_SEMICOLON, KEY_SLASH, 0, 0,
	0, 0, 0, KEY_LEFTCTRL, 0, 0, KEY_Y, KEY_H, KEY_N, KEY_6, KEY_MINUS,
	0, KEY_APOSTROPHE, KEY_BACKSLASH, 0, 0, 0, 0, 0, 0, KEY_LEFTALT,
	0, KEY_U, KEY_J, KEY_M, KEY_7, KEY_EQUAL, 0, KEY_LEFTBRACE,
	KEY_RIGHTBRACE, 0, 0, 0, 0, 0, 0, 0, KEY_RIGHTALT, KEY_I, KEY_K,
	KEY_COMMA, KEY_8, 0, 0, 0, KEY_SPACE, KEY_POWER, 0
};
*/
static int exiting=0;
static int keydown=0;

/* key decoding function
 *
 * takes a packet of the format
 *	0x13XXXXXXXXXXXXXXXXXXXXXXXXXX
 * and determines keyboard state, reports that to input layer
 * check if all keys are released
 *
 * TODO nesting gets too deep in these functions
 */
static void mp900_kb_decode(char cur_buffer[], int packet_length)
{
	int i, j, ff, keycount=0;
	int length=packet_length;
	unsigned char keys_buffer[16]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	static unsigned char last_buffer[32] = {0x13, 0xff, 0xff, 0xff, 0xff,
			     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			     0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			     0, 0, 0, 0};
	
	if ((packet_length == 14) || (packet_length == 10)) {
		ff = 0;
		for (i=1; i < length; i++) {
			keys_buffer[keycount] = cur_buffer[i] ^ last_buffer[i];
			if (cur_buffer[i] == 0xff)
				ff++;
			j = 0;
			while (keys_buffer[keycount]) {
				/* keyboard state has changed */
				if (keys_buffer[keycount] & 1) {
					keys_buffer[keycount+1] =
						keys_buffer[keycount];
					/* TODO adjust mp900_keymap by 1 */
					keys_buffer[keycount] = j * 16 + i + 1;
					if ((1<<j) & cur_buffer[i])
						keys_buffer[keycount]+=128;
					keycount++;
				}

				keys_buffer[keycount]>>=1;
				j++;
			}

			if (keycount > 5) {
				/* assume we have garbage packet */
				keycount = 0;
				ff = 0;
				break;
			}
		}

		if (keycount) {
			for (i=1; i<length; i++) {
				last_buffer[i] = cur_buffer[i];
			}
		}

		while (keycount) {
			keycount--;
			if (keys_buffer[keycount] < 128) {
				input_report_key(dev_kb,
					mp900_keymap[keys_buffer[keycount]],
					1);
				input_sync(dev_kb);
			}
			else {
				input_report_key(dev_kb,
					mp900_keymap[keys_buffer[keycount]-128],
					0);
				input_sync(dev_kb);
			}
			keys_buffer[keycount] = 0;
		}

		/* will we poll for more? */
		keydown = (ff != 13);
	}
}

/* ts_report
 *
 * receives a touchscreen packet
 *	0x04XXYY
 * and reports position to input layer
 */
static void mp900_ts_report(char cur_buffer[], int packet_length)
{
	int x,y;

	if (packet_length == 5) {
		x = cur_buffer[2] + 256 * cur_buffer[1];
		y = cur_buffer[4] + 256 * cur_buffer[3];

		input_report_key(dev_ts, BTN_TOUCH, 1);
		input_report_abs(dev_ts, ABS_X, x);
		input_report_abs(dev_ts, ABS_Y, y);
		input_report_abs(dev_ts, ABS_PRESSURE, 1);
		input_sync(dev_ts);
	}
}

/* workqueue function polls for keyboard state,
 * queue it with delay
 */
static void mp900_kb_poll(struct work_struct *work)
{
	writel(0x13, (void *)&BTTHR);
}

/* Interrupt handler
 *
 * character timeout interrupt should occur on receipt of a string from BTUART
 * 	-string could be single 0x12 ie new keypress
 *	-0x13xxxxxxxxxxxxxxxxxxxxxxxxxx result of keypoll
 *	-single 0x05 ie end of touchscreen event
 *	-0x04XXYY touchscreen absolute position
 *
 *	regard any other string as garbage and drop it
 */
static irqreturn_t mp900_kb_interrupt(int irq,void *dev_id)
{
	int i;
	int count;
	char packet_buffer[32] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
				0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

	/* TODO pause serial traffic if you can */
	writel(0x08, (void *)&BTMCR); // MCR say not RTS

	/* first check for BTUART FIFO overflow */
	if (readl((void *)&BTLSR) & 0x02) {
		writel(0xC7, (void *)&BTFCR); // flush FIFO
		printk(KERN_ERR PFX "btuart FIFO overflow\n");
		if (exiting == 0) {
			schedule_delayed_work(&mp900_kb_work, 4);
		}
		writel(0x0a, (void *)&BTMCR); // MCR RTS
		return IRQ_HANDLED;
	}

	/* check for FIFO trigger level reached ?? */
	/* check for parity/framing/break errors */

	/* next read out FIFO contents, count chars */
	i=0;

	while (readl((void *)&BTLSR) & 0x01) {
		packet_buffer[i] = readl((void *)&BTRBR);
		if (++i > 14)
			break;
	}

	/* Switch between buffer size */
	switch (i) {
		case 1:
			if (packet_buffer[0] == 0x05) {		/* End touchscreen event */
				input_report_key(dev_ts, BTN_TOUCH, 0);
				input_report_abs(dev_ts, ABS_PRESSURE, 0);
				input_sync(dev_ts);

				writel(0x01, (void *)&BTTHR);
				writel(0x0a, (void *)&BTMCR);
				return IRQ_HANDLED;
			}
			if (packet_buffer[0] == 0x12) {		/* Keypress */
				if (keydown)
					break;
				else {				/* ? */
					keydown = 1;
					if (exiting == 0) {
						schedule_delayed_work(
								&mp900_kb_work,
								2);
					}
					break;
				}
			}
			break;

		case 5:
			if (packet_buffer[0] == 0x04) {		/* Touchscreen X/Y event */
				mp900_ts_report(packet_buffer, i);
			}
			break;
		
		case 10: /* Axim X30 reports its special keys here */
			printk(KERN_ERR "btuart : received unsupported packetsize of %d with packet_buffer[0]=%x\n", i, packet_buffer[0]);
			mp900_kb_decode(packet_buffer, i);
			
			if (keydown) {
			    if (exiting == 0) {
				schedule_delayed_work(&mp900_kb_work, 2);
			    }
			}
					
			break;

		case 14: /* Nec Mobilepro 900/c reports keys here */
			if (packet_buffer[0] == 0x13) {		/* Keyboard poll packet */
				mp900_kb_decode(packet_buffer, i);

				if (keydown) {
					if (exiting == 0) {
						schedule_delayed_work(
								&mp900_kb_work,
								2);
					}
				}
			}
			break;
		case 15:
			printk(KERN_ERR "btuart : received unsupported packetsize of %d with packet_buffer[0]=%x\n", i, packet_buffer[0]);
			break;

		default:
			/* make sure ts is clear to transmit */
			writel(0x01, (void *)&BTTHR);

			if (keydown) {
				if (exiting == 0) {
					schedule_delayed_work(&mp900_kb_work,
							2);
				}
			} else {
			    printk(KERN_ERR "btuart : received unsupported paketsize of %d with packet_buffer[0]=%x\n", i, packet_buffer[0]);
			}
			break;
	}

	writel(0x0a, (void *)&BTMCR); // MCR RTS
	return IRQ_HANDLED;
}

static void __exit mp900_kb_exit(void)
{
	int i;

	exiting = 1;
	cancel_delayed_work(&mp900_kb_work);
	flush_workqueue(mp900_kb_workqueue);
	destroy_workqueue(mp900_kb_workqueue);

	/* stop the BTUART clock */
	/* TODO see arch/arm/mach-pxa/generic.c for pxa_set_cken()
	 * a much nicer way to do this...
	 */
	i = readl((void *)&CKEN) & 0xFFFF;
	i &= 0xFFEF;
	writel(i, (void *)&CKEN);

	free_irq(IRQ_BTUART, NULL);
	input_unregister_device(dev_ts);
	input_unregister_device(dev_kb);
	printk(KERN_INFO PFX "devices removed\n");
}

/* initialize
 * ought to test kb presence
 *
 * need to disallow userland access to BTUART while keyboard driver
 * is operational ... it should be enough to set up a udev rule to
 * keep /dev/ttyS1 out of /dev - because pxa-serial driver leaves
 * interrupts alone except while someone reads or writes to /dev/ttySx ???
 *
 * will need to set baud rate, FIFO, LCR, MCR, interrupt type in
 * BTUART registers
 */
static int __init mp900_kb_init(void)
{
	int i;

	mp900_kb_workqueue = create_workqueue("poll4key");
//	INIT_DELAYED_WORK(&mp900_kb_work, mp900_kb_poll);

	printk(KERN_INFO PFX "initializing keyboard and touchscreen\n");

	/* enable BTUART clock */
	/* TODO see arch/arm/mach-pxa/generic.c for pxa_set_cken()
	 * a much nicer way to do this...
	 */
	i = readl((void *)&CKEN) & 0xFFFF;
	i |= 0x80;
	writel(i, (void *)&CKEN);

	/* Keyboard */
	dev_kb = input_allocate_device();
	dev_kb->evbit[0] = BIT(EV_KEY) | BIT(EV_REP);
	set_bit(KEY_SUSPEND, dev_kb->keybit);
	dev_kb->name = mp900_kb_name;
	for (i=0; i < 128; i++)
		if (mp900_keymap[i])
			set_bit(mp900_keymap[i], dev_kb->keybit);

	/* Touchscreen */
	dev_ts = input_allocate_device();
	dev_ts->evbit[0] = BIT(EV_KEY) | BIT(EV_ABS);
	dev_ts->absbit[0] = BIT(ABS_X) | BIT(ABS_Y) | BIT(ABS_PRESSURE);
	set_bit(BTN_TOUCH, dev_ts->keybit);

	/* Not generic values, will need adjusting */
	dev_ts->absmin[ABS_X] = 32;
	dev_ts->absmin[ABS_Y] = 96;
	dev_ts->absmax[ABS_X] = 1004;
	dev_ts->absmax[ABS_Y] = 792;
	dev_ts->name = mp900_ts_name;

	input_register_device(dev_kb);
	input_register_device(dev_ts);

	if (request_irq(IRQ_BTUART, mp900_kb_interrupt, 0,
				"MobilePro900/c keyboard", NULL))
		printk(KERN_ERR PFX "request irq failed!\n");

	/* set up BTUART regs */

	/* clear FIFO,clear interrupt regs (by reading)
	 * set LCR, set MCR, enable interrupts (IER)
	 * clear interrupt regs again
	 * that's roughly how pxa-serial.c does it...
	 *
	 * TODO oh boy this looks messy...
	 */
	writel(0x03, (void *)&BTLCR);	// LCR DLAB bit=0
	writel(0xC1, (void *)&BTFCR);	// FCR enable, trigger level 32 bytes
	writel(0xC7, (void *)&BTFCR);	// FCR clear
	readl((void *)&BTLSR);		// read/clear LSR
	readl((void *)&BTRBR);		// read/clear RX
	readl((void *)&BTIIR);		// read/clear IIR
	readl((void *)&BTMSR);		// read/clear MSR
	writel(0x03, (void *)&BTLCR);	// LCR again

	/* do we need to spinlock for this ??
	 * once OUT2 bit is set interrupt is LIVE
	 */
	writel(0x0a, (void *)&BTMCR);	// MCR OUT2, RTS

	/* what type of interrupts do we catch ? */
	writel(0x51, (void *)&BTIER);	// IER UUE, RAVIE, RTOIE (char timeout)
	/* TODO - what causes an RLSE interrupt? */
//	writel(0x41, (void *)&BTIER);	// IER UUE, RAVIE
//	writel(0x55, (void *)&BTIER);	// IER UUE, RAVIE, RLSE, RTOIE

	/* once again for good luck ?? */
	readl((void *)&BTLSR);
	readl((void *)&BTRBR);
	readl((void *)&BTIIR);
	readl((void *)&BTMSR);

	/* make sure ts is clear to transmit */
	writel(0x01, (void *)&BTTHR);

	printk(KERN_INFO PFX "registers, and irq set up\n");

	return 0;
}

module_init(mp900_kb_init);
module_exit(mp900_kb_exit);

