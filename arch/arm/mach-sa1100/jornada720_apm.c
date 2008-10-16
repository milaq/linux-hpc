/*
 * HP Jornada 720 APM driver
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/device.h>

#include <asm/apm.h>

#include <asm/hardware.h>
#include <asm/arch/jornada720_mcu.h>

/*
 * HP Documentation referred in this file:
 * http://www.jlime.com/downloads/development/docs/jornada7xx/jornada720.txt
 */

MODULE_AUTHOR("Filip Zyzniewski <filip.zyzniewski@tefnet.pl>");
MODULE_DESCRIPTION("Jornada 720 battery status reporting");
MODULE_LICENSE("GPL");

#define jornada720_apm_battery_charging() (! (GPLR & GPIO_GPIO26) )
#define jornada720_apm_ac_online() ((GPLR & GPIO_GPIO4) ? 0 : 1)

#define JORNADA720_APM_BACKUP_BATTERY 0
#define JORNADA720_APM_MAIN_BATTERY 1

#define JORNADA720_APM_MAIN_BATT_MIN_VOLTAGE 430

/* without ac power */
#define JORNADA720_APM_MAIN_BATT_MAX_VOLTAGE 670

/* divisor to convert jornada720_apm_get_main_battery() return value into % */
#define JORNADA720_APM_MAIN_BATT_DIVISOR \
	(JORNADA720_APM_MAIN_BATT_MAX_VOLTAGE-JORNADA720_APM_MAIN_BATT_MIN_VOLTAGE) * \
	(JORNADA720_APM_MAIN_BATT_MAX_VOLTAGE-JORNADA720_APM_MAIN_BATT_MIN_VOLTAGE) / 100
/*
 * coeffient correcting battery voltage altered by
 * ac power
 */
#define JORNADA720_APM_MAIN_BATT_AC_COEFF 100 / 105


int jornada720_apm_get_battery_raw(int battnum)
{
	unsigned char lsb, msb;

	/* getting batteries data (line 289 of HP's doc) */
	if (!jornada720_mcu_start(jornada720_mcu_GetBatteryData)) {
		lsb=jornada720_mcu_read();
		if (battnum==JORNADA720_APM_BACKUP_BATTERY)
			/* we are interested in the second byte */
			lsb=jornada720_mcu_read();
		else
			/* we are interested in the first byte */
			jornada720_mcu_read();

		msb=jornada720_mcu_read();
	} else {
		jornada720_mcu_end();
		return -1;
	}

	jornada720_mcu_end();

	if (battnum==JORNADA720_APM_MAIN_BATTERY) {
		/*
		* main battery absent
		* (http://mail-index.netbsd.org/port-hpcarm/2005/09/18/0000.html)
		*/
		if ((msb & 0x03) == 0x03) return -1;
		/* putting the value together (line 300 of HP's doc) */
		return ((msb & 0x03) << 8) + lsb;
	} else {
		/*
		* backup battery absent
		* (http://mail-index.netbsd.org/port-hpcarm/2005/09/18/0000.html)
		*/
		if ((msb & 0x0c) == 0x00) return -1;
		/* putting the value together (line 300 of HP's doc) */
		return ((msb & 0x0c) << 6) + lsb;
	}
}

int jornada720_apm_get_battery(int battnum)
{
	int ret = jornada720_apm_get_battery_raw(battnum);
	
	if (ret == -1)
		return ret;

	if (battnum==JORNADA720_APM_MAIN_BATTERY) {
		/* we want 0 for a completely drained battery */
		ret -= JORNADA720_APM_MAIN_BATT_MIN_VOLTAGE;

		/* voltage(time) is not linear */
		ret *= ret;

		/* let's bring it down to 0-100% range */
		ret /= JORNADA720_APM_MAIN_BATT_DIVISOR;

		/* plugging AC power causes voltage to rise a bit */
		if (jornada720_apm_ac_online())
			ret = ret * JORNADA720_APM_MAIN_BATT_AC_COEFF;

		/* 
		 * sometimes it tends to fluctuate a bit above 100%, which
		 * looks funny, so we bring it down
		 */
		if (ret > 100) ret=100;

	}
	
	/* 
	 * returning a raw value, because we don't know how to calculate %
	 * for a backup battery 
	 */
	return ret;
}

static void jornada720_apm_get_power_status(struct apm_power_info *info)
{

	info->battery_life=jornada720_apm_get_battery(JORNADA720_APM_MAIN_BATTERY);

	if (info->battery_life==-1) {
		info->battery_status = APM_BATTERY_STATUS_NOT_PRESENT;
		info->battery_flag = APM_BATTERY_FLAG_NOT_PRESENT;

	} else if (info->battery_life < 30) {
		info->battery_status = APM_BATTERY_STATUS_LOW;
		info->battery_flag = APM_BATTERY_FLAG_LOW;

	} else if (info->battery_life < 5) {
		info->battery_status = APM_BATTERY_STATUS_CRITICAL;
		info->battery_flag = APM_BATTERY_FLAG_CRITICAL;

	} else {
		info->battery_status = APM_BATTERY_STATUS_HIGH;
		info->battery_flag = APM_BATTERY_FLAG_HIGH;
	}

	if (jornada720_apm_battery_charging())
		info->battery_status = APM_BATTERY_STATUS_CHARGING;

	info->ac_line_status = jornada720_apm_ac_online();
}

static int jornada720_apm_probe(struct device *dev) {

	/* we provide a function to check battery levels etc */
	apm_get_power_status=jornada720_apm_get_power_status;

	return 0;
}

static int jornada720_apm_remove(struct device *dev) {
	if(apm_get_power_status==jornada720_apm_get_power_status)
		apm_get_power_status=NULL;
	return 0;
}

static struct device_driver jornada720_apm_driver = {
	.name	= jornada720_mcu_bus_id_apm,
	.bus	= &jornada720_mcu_bus_type,
	.probe	= jornada720_apm_probe,
	.remove	= jornada720_apm_remove, 
	.owner	= THIS_MODULE,
};

static int __init jornada720_apm_init(void) {
	int ret, backup_level;
	
	ret = driver_register(&jornada720_apm_driver);
	if(ret) return ret;

	backup_level = jornada720_apm_get_battery(JORNADA720_APM_BACKUP_BATTERY);
	
	if(backup_level != -1)
		printk(KERN_INFO "jornada720_apm: backup battery level: %i\n", backup_level);
	else
		printk(KERN_INFO "jornada720_apm: backup battery not present\n");
	
	return ret;
}

static void __exit jornada720_apm_exit(void) {
	driver_unregister(&jornada720_apm_driver);
}


module_init(jornada720_apm_init);
module_exit(jornada720_apm_exit);
