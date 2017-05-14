/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include "mbc.h"

set8FuncT mbcSet8;

uint16_t cBank = 1;
uint16_t extBank = 0;
uint16_t bankMask = 0;
uint16_t extMask = 0;
uint16_t extTotalMask = 0;
size_t extTotalSize = 0;

bool extMemUsed = false;
bool bankUsed = false;
bool extSelect = false;

static void noSet8(uint16_t addr, uint8_t val);
static void mbc1Set8(uint16_t addr, uint8_t val);
static void mbc3Set8(uint16_t addr, uint8_t val);
static void mbc5Set8(uint16_t addr, uint8_t val);

void mbcInit(uint8_t type)
{
	if(type == MBC_TYPE_1)
		mbcSet8 = mbc1Set8;
	else if(type == MBC_TYPE_3)
		mbcSet8 = mbc3Set8;
	else if(type == MBC_TYPE_5)
		mbcSet8 = mbc5Set8;
	else
		mbcSet8 = noSet8;
}

static void noSet8(uint16_t addr, uint8_t val)
{
	(void)addr;
	(void)val;
}

static void mbc1Set8(uint16_t addr, uint8_t val)
{
	if(addr >= 0x2000 && addr < 0x4000)
	{
		if(bankUsed)
		{
			//printf("%02x\n",val);
			cBank &= ~0x1F;
			cBank |= val&0x1F;
			cBank &= bankMask;
			if((cBank&0x1F) == 0)
				cBank |= 1;
			cBank &= bankMask;
		}
	}
	else if(addr >= 0x4000 && addr < 0x6000)
	{
		if(extSelect)
		{
			if(extMemUsed)
			{
				extBank = val&3;
				extBank &= extMask;
			}
		}
		else
		{
			if(bankUsed)
			{
				//printf("%02x\n",val);
				cBank &= 0x1F;
				cBank |= ((val&3)<<5);
				cBank &= bankMask;
				if((cBank&0x1F) == 0)
					cBank |= 1;
			}
		}
	}
	else if(addr >= 0x6000 && addr < 0x8000)
		extSelect = !!val;
}

static void mbc3Set8(uint16_t addr, uint8_t val)
{
	if(addr >= 0x2000 && addr < 0x4000)
	{
		if(bankUsed)
		{
			//printf("%02x\n",val);
			cBank = val&0x7F;
			cBank &= bankMask;
			if(cBank == 0)
				cBank |= 1;
			cBank &= bankMask;
		}
	}
	else if(addr >= 0x4000 && addr < 0x6000)
	{
		if(extMemUsed)
		{
			extBank = val&3;
			extBank &= extMask;
		}
		//TODO:RTC Support
	}
}

static void mbc5Set8(uint16_t addr, uint8_t val)
{
	if(addr >= 0x2000 && addr < 0x3000)
	{
		if(bankUsed)
		{
			cBank &= ~0xFF;
			cBank |= val;
			cBank &= bankMask;
		}
	}
	else if(addr >= 0x3000 && addr < 0x4000)
	{
		if(bankUsed)
		{
			cBank &= 0xFF;
			cBank |= (val&1)<<8;
			cBank &= bankMask;
		}
	}
	else if(addr >= 0x4000 && addr < 0x6000)
	{
		if(extMemUsed)
		{
			extBank = val&0xF;
			extBank &= extMask;
		}
	}
}
