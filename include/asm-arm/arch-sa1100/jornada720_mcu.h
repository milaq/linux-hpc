/*
 * include/asm-arm/arch-sa1100/jornada720_mcu.h
 *
 * This file contains MCU communication API defintions for HP Jornada 720
 *
 * Copyright (C) 2006 Filip Zyzniewski <filip.zyzniewski@tefnet.pl>
 *  Copyright (C) 2000 John Ankcorn <jca@lcs.mit.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

/* Jornada 720 Microprocessor Control Unit commands */
#define jornada720_mcu_GetBatteryData	0xc0
#define jornada720_mcu_GetScanKeyCode	0x90
#define jornada720_mcu_GetTouchSamples	0xa0
#define jornada720_mcu_GetContrast	0xD0
#define jornada720_mcu_SetContrast	0xD1
#define jornada720_mcu_GetBrightness	0xD2
#define jornada720_mcu_SetBrightness	0xD3
#define jornada720_mcu_ContrastOff	0xD8
#define jornada720_mcu_BrightnessOff	0xD9
#define jornada720_mcu_PWMOFF		0xDF
#define jornada720_mcu_TxDummy		0x11
#define jornada720_mcu_ErrorCode	0x00

/* devices accessible through the MCU */
#define jornada720_mcu_bus_id_kbd	"jornada720_kbd"
#define jornada720_mcu_bus_id_ts	"jornada720_ts"
#define jornada720_mcu_bus_id_apm	"jornada720_apm"
#define jornada720_mcu_bus_id_lcd	"jornada720_lcd"
#define jornada720_mcu_bus_id_bl	"jornada720_bl"

int jornada720_mcu_byte(u8 byte);

#define jornada720_mcu_read() jornada720_mcu_byte(jornada720_mcu_TxDummy)

/* 
 * WARNING: remember to jornada720_mcu_end() after every
 * jornada720_mcu_start() or you will deadlock!
 */
int jornada720_mcu_start(u8 byte);

void jornada720_mcu_end(void);

extern struct bus_type jornada720_mcu_bus_type;
