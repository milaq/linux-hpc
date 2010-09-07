/* drivers/video/s1d13mp900fb.c
 *
 * IT IS NOT INTENDED THAT THIS FILE BE SUBMITTED TO VANILLA
 *
 * for now this is a standalone driver for testing the
 * specifics of the MobilePro->S1D13806 interface
 *
 * the existing s1d13xxxfb.c driver should work with the Mobilepro900c
 * if it is told where in memory to find the chip
 *   physical addresses:
 *   base/registers 0x0c00_0000
 *   framebuffer    0x0c20_0000
 * and initial register settings
 * TODO establish default register values
 * perhaps all that belongs in
 *   arch/arm/mach-pxa/mp900.c
 *
 * blanking/backlight specific code should go in
 *   drivers/video/backlight/mp900_bl.c or so
 *
 * Michael Petchkovsky mkpetch@internode.on.net May 2007
 */

/* TODO
 * clear hardware cursor
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>

#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>

#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/hardware.h>

#include "console/fbcon.h"

#define PFX "s1d13mp900fb: "
#define S1D13MP900_FB_PHYS 	0x0C200000
#define S1D13MP900_REG_PHYS 	0x0C000000
#define S1D13MP900_FB_SIZE	0x00140000

u32 pseudo_pal[16];
static void *remapped_regs;
static void *remapped_fb;
struct fb_info fb_info;

static int s1d13mp900fb_setcolreg(unsigned regno, unsigned red, unsigned green,
				unsigned blue, unsigned transp,
				struct fb_info *fb_info)
{
	int bpp, m = 0;

	bpp = fb_info->var.bits_per_pixel;
	m = (bpp <= 8) ? (1 << bpp) : 256;
	if (regno >= m) {
		printk("regno %d out of range (max %d)\n", regno, m);
		return -EINVAL;
	}
	switch (bpp) {
		case 8:
			break;
		case 16:
			pseudo_pal[regno] = ((red & 0xF800) |
					((green & 0xFC00) >> 5) |
					((blue & 0xF800) >> 11));
			break;
	}

	return 0;
}

static int s1d13mp900fb_blank(int blank, struct fb_info *info)
{
	u32 rval;
	switch (blank) {
		case FB_BLANK_POWERDOWN:
		case FB_BLANK_VSYNC_SUSPEND:
		case FB_BLANK_HSYNC_SUSPEND:
		case FB_BLANK_NORMAL:
			/* we want to switch off the backlight via
			 * the s1d13xxx gpio pins and put the chip
			 * to sleep
			 *
			 * gpio pins are controlled through register
			 * 0x08/0x09, we clear pins 4,1,2 and set pin 0
			 *
			 * this could be done by
			 *   writel(0x0001, remapped_regs + 0x8)
			 * but safer to read initial values and set pins
			 * one-by-one, delays could be introduced between
			 * steps if required...
			 */
			rval = readl(remapped_regs + 0x8);
			rval &= 0xffef;
			writel(rval, remapped_regs + 0x8);
			rval &= 0xfffd;
			writel(rval, remapped_regs + 0x8);
			rval &= 0xfffd;
			writel(rval, remapped_regs + 0x8);
			rval |= 1;
			writel(rval, remapped_regs + 0x8);
			/* power save config register is at 0x1f0
			 * set it to 0x11 for zzz and 0x10 to wake
			 */
			writel(0x11, remapped_regs + 0x1f0);
			/* after this it would be safe to shutdown
			 * pixel and memory clocks, read 0x1f1 to
			 * confirm sleep-mode entered
			 *
			 * perhaps PWM0 clock can be disabled with
			 * backlight off to save a little power
			 */
			break;

		case FB_BLANK_UNBLANK:
			/* we reverse the blanking sequence */
			writel(0x10, remapped_regs + 0x1f0);
			rval = readl(remapped_regs + 0x8);
			rval &= 0xfffe;
			writel(rval, remapped_regs + 0x8);
			rval |= 4;
			writel(rval, remapped_regs + 0x8);
			rval |= 2;
			writel(rval, remapped_regs + 0x8);
			/* want a delay here? */
			rval |= 0x10;
			writel(rval, remapped_regs + 0x8);
	}
	
	return 0;
}

struct s1d13mp900fb_par {
	void __iomem	*regs;
	unsigned char	display;
};

static struct fb_fix_screeninfo s1d13mp900fb_fix __initdata = {
	.id =		"S1DMP_FBID",
//	.smem_len =	(640 * 240 * 16) / 8, //TODO check this
	.smem_len =	S1D13MP900_FB_SIZE,
	.smem_start =	S1D13MP900_FB_PHYS,
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.line_length =	(640 * 16) / 8,
	.type_aux =	0,
	.xpanstep =	0,
	.ypanstep =	1,
	.ywrapstep =	0,
	.accel =	FB_ACCEL_NONE,
};

static struct fb_var_screeninfo s1d13mp900fb_screeninfo = {
	.xres =			640,
	.yres =			240,
	.xres_virtual =		640,
	.yres_virtual =		240,
	.bits_per_pixel =	16,
	.red.length =		5,
	.green.length =		6,
	.blue.length = 		5,
	.transp.length =	0,
	.red.offset =		11,
	.green.offset =		5,
	.blue.offset =		0,
	.transp.offset =	0,
	.activate =		FB_ACTIVATE_NOW,
	.height =		-1,
	.width =		-1,
	.vmode =		FB_VMODE_NONINTERLACED,
	.accel_flags =		0,
	.nonstd =		0,
};

static struct fb_ops s1d13mp900fb_ops = {
	.owner =	THIS_MODULE,
	.fb_setcolreg =	s1d13mp900fb_setcolreg,
	.fb_fillrect =	cfb_fillrect,
	.fb_copyarea =	cfb_copyarea,
	.fb_imageblit =	cfb_imageblit,
//	.fb_cursor =	soft_cursor,
	.fb_blank =	s1d13mp900fb_blank,
};

/* colour lookup tables? l8r if we need them
 */

unsigned char LUT8[256*3];

//static char lut_base[90];

void s1d13mp900fb_init_hardware (void)
{
//	unsigned char *pLUT = LUT8;
//	unsigned char *pseed = lut_base;
//	unsigned char plast[3];
//	int i, j, rgb;
	int rval;

	/* OK let's assume chip has been set up by bootloader for now
	 * this would be a good chance to take a peek at the regs ;)
	 * TODO let's not assume...
	 */

	rval = readb(remapped_regs);
	printk (KERN_INFO PFX "reg[0x000] revision code is 0x%X\n", rval);
}

int __init s1d13mp900fb_init(void)
{
	if (fb_get_options("s1d13mp900fb", NULL))
		return -ENODEV;

	printk (KERN_INFO PFX "initing now...\n");

	/* remap framebuffer and registers */

	/* do we need to request_mem_region ? */
	if (!request_mem_region(S1D13MP900_FB_PHYS,
				S1D13MP900_FB_SIZE, "s1d13806_fb")) {
		printk (KERN_ERR PFX "unable to reserve framebuffer\n");
	} else {
		remapped_fb = ioremap_nocache(S1D13MP900_FB_PHYS,
						S1D13MP900_FB_SIZE);
		if (!remapped_fb)
			printk (KERN_INFO PFX "unable to map framebuffer\n");
	}

	if (!request_mem_region(S1D13MP900_REG_PHYS, 512, "s1d13806_regs")) {
		printk (KERN_ERR PFX "unable to reserve registers\n");
	} else {
		remapped_regs = ioremap_nocache(S1D13MP900_REG_PHYS, 512);
		if (!remapped_regs)
			printk(KERN_ERR PFX "unable to map registers\n");
	}

	fb_info.screen_base = remapped_fb;
	fb_info.screen_size = S1D13MP900_FB_SIZE; //TODO correct??
	memset(&fb_info.var, 0, sizeof(fb_info.var));

	s1d13mp900fb_init_hardware();
	/* you could zero out the display here with memset */

	fb_info.fbops =		&s1d13mp900fb_ops;
	fb_info.var =		s1d13mp900fb_screeninfo;
	fb_info.fix =		s1d13mp900fb_fix;
	fb_info.flags =		FBINFO_DEFAULT;
	fb_info.pseudo_palette=	&pseudo_pal;

	if (register_framebuffer(&fb_info) < 0)
		return 1;

	return 0;
}

static void __exit s1d13mp900fb_exit(void)
{
	printk (KERN_INFO PFX "unregistering framebuffer device\n");
	iounmap(remapped_regs);
	iounmap(remapped_fb);
	release_mem_region(S1D13MP900_REG_PHYS, 512);
	release_mem_region(S1D13MP900_FB_PHYS, S1D13MP900_FB_SIZE);
	unregister_framebuffer(&fb_info);
}

module_init(s1d13mp900fb_init);
module_exit(s1d13mp900fb_exit);
MODULE_AUTHOR("Michael Petchkovsky");
MODULE_DESCRIPTION("Epson S1D13806 fb interface for NEC MobilePro900/c");
MODULE_LICENSE("GPL");
