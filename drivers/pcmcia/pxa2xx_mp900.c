/*
 * linux/drivers/pcmcia/pxa2xx_mp900.c
 *
 * NEC MobilePro900/c PCMCIA specific routines.
 *
 * based on pxa2xx_mainstone.c
 *
 * The MobilePro900 has one cf slot and one pcmcia slot
 * each managed by a NeoMagic NMC1110 companion chip
 *
 * Socket0 of the pxa255 (mapped to physical address 0x2000_0000)
 *   corresponds to the CF card
 *   with GPIO11 as nCD (card detect)
 *   and GPIO5 as PRDY (ready/busy)
 *   NMC1110 PRS/PRC register is at physical address 0x0900_0000
 *
 * Socket1 of the pxa255 (mapped to physical address 0x3000_0000)
 *   corresponds to the PCMCIA card
 *   with GPIO13 as nCD
 *   and GPIO7 as PRDY
 *   NMC1110 PRS/PRC register is at physical address 0x0a00_0000
 *
 * the NMC1110 PRS/PRC registers
 *   are single 16bit read and write (status and command)
 *   regs which are important for controlling slot power and RESET
 *
 * If STSCHG is available on GPIOs I havn't found it yet
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <pcmcia/ss.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/hardware.h>
#include <asm/irq.h>

#include <asm/arch/pxa-regs.h>

#include "soc_common.h"

#define CF_CARD_REG	0x09000000
#define PC_CARD_REG	0x0a000000

void *cf_prs_addr, *pc_prs_addr;

static struct pcmcia_irqs irqs[] = {
	{ 0, IRQ_GPIO(11), "PCMCIA0 CD" },
	{ 1, IRQ_GPIO(13), "PCMCIA1 CD" },
//	{ 0, MAINSTONE_S0_STSCHG_IRQ, "PCMCIA0 STSCHG" },
//	{ 1, MAINSTONE_S1_STSCHG_IRQ, "PCMCIA1 STSCHG" },
};

static int mp900_pcmcia_hw_init(struct soc_pcmcia_socket *skt)
{
	printk(KERN_INFO "mp900_pcmcia_hw_init %d\n", skt->nr);
	/* we just want to set up the gpios for the
	 * slots and set interrupts on RDY
	 */

	/* Setup default state of GPIO outputs
	 * before we enable them as outputs.
	 *
	 * trust bootloader/wince to do this for now
	 * TODO this MUST happen here as we don't want to rely on
	 * bootloader/wince... double check that below is as desired
	 * before uncommenting.
	 */
/*	GPSR(GPIO48_nPOE) =
		GPIO_bit(GPIO48_nPOE) |
		GPIO_bit(GPIO49_nPWE) |
		GPIO_bit(GPIO50_nPIOR) |
		GPIO_bit(GPIO51_nPIOW) |
		GPIO_bit(GPIO85_nPCE_1) |
		GPIO_bit(GPIO54_nPCE_2);

	pxa_gpio_mode(GPIO48_nPOE_MD);
	pxa_gpio_mode(GPIO49_nPWE_MD);
	pxa_gpio_mode(GPIO50_nPIOR_MD);
	pxa_gpio_mode(GPIO51_nPIOW_MD);
	pxa_gpio_mode(GPIO85_nPCE_1_MD);
	pxa_gpio_mode(GPIO54_nPCE_2_MD);
	pxa_gpio_mode(GPIO79_pSKTSEL_MD);
	pxa_gpio_mode(GPIO55_nPREG_MD);
	pxa_gpio_mode(GPIO56_nPWAIT_MD);
	pxa_gpio_mode(GPIO57_nIOIS16_MD);
*/
	/* PRDY signals for socket irqs? */
	skt->irq = (skt->nr == 0) ? IRQ_GPIO(5) : IRQ_GPIO(7);
	return soc_pcmcia_request_irqs(skt, irqs, ARRAY_SIZE(irqs));
}

static void mp900_pcmcia_hw_shutdown(struct soc_pcmcia_socket *skt)
{
	printk(KERN_INFO "mp900_pcmcia_hw_shutdown %d\n", skt->nr);
	soc_pcmcia_free_irqs(skt, irqs, ARRAY_SIZE(irqs));
}

/* _socket_state gets polled every second or so as not all
 * signals generate interrupts, see soc_common.c
 */
static void mp900_pcmcia_socket_state(struct soc_pcmcia_socket *skt,
				    struct pcmcia_state *state)
{
	int prs_value=0;

	/* first read NMC1110's PRS register for the slot */
	if (skt->nr)
	{
		prs_value = readw(pc_prs_addr);
	} else {
		prs_value = readw(cf_prs_addr);
	}

	prs_value &= 0xffff;

	/* these are determined from gpios */
	state->detect = (skt->nr == 0) ? !(GPLR(11) & GPIO_bit(11))
				       : !(GPLR(13) & GPIO_bit(13));
	state->ready  = (skt->nr == 0) ? !!(GPLR(5) & GPIO_bit(5))
				       : !!(GPLR(7) & GPIO_bit(7));
	/* these are read from PRS register */
	state->bvd1   = !!(prs_value & 0x10);
	state->bvd2   = !!(prs_value & 0x20);
	state->vs_3v  = !(prs_value & 0x40);
	state->vs_Xv  = !(prs_value & 0x80); /* seems to detect 5V cards OK */
	state->wrprot = 0;  /* what is wrprot? */
}

/* this is where we apply or remove power from a socket so we need to get
 * this right. Also card reset is implemented here.
 *
 * Power and reset are handled through the NMC1110 PRC register
 */
static int mp900_pcmcia_configure_socket(struct soc_pcmcia_socket *skt,
				       const socket_state_t *state)
{
	int ret=0;
	int prs_value=0;
	int prc_value=0;
	int is_powered=0;

	/* check current power state */
	if (skt->nr) {
		prs_value = readw(pc_prs_addr);
	} else {
		prs_value = readw(cf_prs_addr);
	}

	is_powered = prs_value & 0xf;

	/* we will set SOE bit */
	prc_value |= 0x80;

	printk(KERN_INFO "mp900_pcmcia_configure_socket %d\n", skt->nr);

	switch (state->Vcc) {
	case 0:
		/* you can clear whole reg when applying 0v */
//		prc_value &= 0xfff0;
		prc_value = 0x0;
		printk(KERN_INFO "    state->Vcc = 0\n");
		break;
	case 33:
//		prc_value |= 0x8;
		prc_value |= 0x9;
		printk(KERN_INFO "    state->Vcc = 33\n");
		break;
	case 50:
//		prc_value |= 0x4;
		prc_value |= 0x6;
		printk(KERN_INFO "    state->Vcc = 50\n");
		break;
	default:
		 printk(KERN_ERR "    bad Vcc %u\n", state->Vcc);
		 ret = -1;
	}

	switch (state->Vpp) {
	case 0:
		printk(KERN_INFO "    state->Vpp = 0\n");
		break;
	case 120:
		printk(KERN_INFO "    state->Vpp = 120\n");
		break;
	default:
		  if(state->Vpp == state->Vcc) {
			  printk(KERN_INFO "    state->Vpp = %u\n",
					  		state->Vpp);
		  } else {
			  printk(KERN_ERR "    bad Vpp %u\n", state->Vpp);
			  ret = -1;
		  }
	}

	if (state->flags & SS_RESET) {
		prc_value |= 0x10;
		printk(KERN_INFO "    socket reset requested\n");
	}

	if (skt->nr) {
		/* this is the pcmcia slot */
		/* leave CFE and SSP till later
		 * set S1-4 and SOE then delay
		 */
		if (!is_powered) {
			/* we applying power to unpowered socket */
			/* first set S3 and SOE (mask S1/2) */
			writew(prc_value & 0xfffc, pc_prs_addr);
			/* then write again with S1/2 unmasked */
			writew(prc_value, pc_prs_addr);
			/* then delay */
			udelay(1000);
			prc_value &= 0xffbf;	// CFE bit = 0
			prc_value |= 0x100;	// SSP bit = 1
			writew(prc_value, pc_prs_addr);
		} else {
			/* we are removing power or it's a reset */
			if (prc_value & 0xf) {
				prc_value &= 0xffbf;
				prc_value |= 0x100;
			}
			writew(prc_value, pc_prs_addr);
		}

		udelay(10);
		prs_value = readw(pc_prs_addr);
	} else {
		/* this is cf slot */
		if (!is_powered) {
			writew(prc_value & 0xfffc, cf_prs_addr);
			writew(prc_value, cf_prs_addr);
			udelay(1000);
			prc_value |= 0x40;	// CFE bit = 1
			prc_value &= 0xfeff;	// SSP bit = 0
			writew(prc_value, cf_prs_addr);
		} else {
			if (prc_value & 0xf) {
				prc_value |= 0x40;
				prc_value &= 0xfeff;
			}
			writew(prc_value, cf_prs_addr);
		}

		udelay(10);
		prs_value = readw(cf_prs_addr);
	}

	prs_value &= 0xffff;

	printk(KERN_INFO "    PRS register value is 0x%04x\n", prs_value);

	return ret;
}

static void mp900_pcmcia_socket_init(struct soc_pcmcia_socket *skt)
{
	printk(KERN_INFO "mp900_pcmcia_socket_init %d\n", skt->nr);
}

static void mp900_pcmcia_socket_suspend(struct soc_pcmcia_socket *skt)
{
	printk(KERN_INFO "mp900_pcmcia_socket_suspend %d\n", skt->nr);
}

static struct pcmcia_low_level mp900_pcmcia_ops = {
	.owner			= THIS_MODULE,
	.hw_init		= mp900_pcmcia_hw_init,
	.hw_shutdown		= mp900_pcmcia_hw_shutdown,
	.socket_state		= mp900_pcmcia_socket_state,
	.configure_socket	= mp900_pcmcia_configure_socket,
	.socket_init		= mp900_pcmcia_socket_init,
	.socket_suspend		= mp900_pcmcia_socket_suspend,
	.nr			= 2,
};

static struct platform_device *mp900_pcmcia_device;

static int __init mp900_pcmcia_init(void)
{
	int ret;

	/* request and ioremap NMC1110 registers */
	if (!request_mem_region(CF_CARD_REG, 4, "cfcard_prs")) {
		printk(KERN_ERR "mp900_pcmcia_init: unable to reserve "
							"CF_CARD_REG\n");
		return -ENODEV;
	}
	cf_prs_addr = ioremap(CF_CARD_REG, 4);
	if (!cf_prs_addr) {
		printk(KERN_ERR "mp900_pcmcia_init: unable to map "
							"cfcard_prs\n");
		return -ENODEV;
	}

	if (!request_mem_region(PC_CARD_REG, 4, "pccard_prs")) {
		printk(KERN_ERR "mp900_pcmcia_init: unable to reserve "
							"PC_CARD_REG\n");
		return -ENODEV;
	}
	pc_prs_addr = ioremap(PC_CARD_REG, 4);
	if (!pc_prs_addr) {
		printk(KERN_ERR "mp900_pcmcia_init: unable to map "
							"pccard_prs\n");
		return -ENODEV;
	}

	mp900_pcmcia_device = platform_device_alloc("pxa2xx-pcmcia", -1);
	if (!mp900_pcmcia_device)
		return -ENOMEM;

	mp900_pcmcia_device->dev.platform_data = &mp900_pcmcia_ops;

	ret = platform_device_add(mp900_pcmcia_device);

	if (ret)
		platform_device_put(mp900_pcmcia_device);

	return ret;
}

static void __exit mp900_pcmcia_exit(void)
{
	/* release NMC1110 register memory */
	iounmap(cf_prs_addr);
	release_mem_region(CF_CARD_REG, 4);
	iounmap(pc_prs_addr);
	release_mem_region(PC_CARD_REG, 4);

	platform_device_unregister(mp900_pcmcia_device);
}

fs_initcall(mp900_pcmcia_init);
module_exit(mp900_pcmcia_exit);

MODULE_LICENSE("GPL");
