/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include "mem.h"
#include "mbc.h"

uint8_t Ext_Mem[0x20000];
set8FuncT mbcSet8;
set8FuncT mbcSetRAM8;
get8FuncT mbcGetRAM8;

//multicart regs
uint16_t tBank0;
uint16_t tBank1;
uint16_t oBank;
uint16_t iBank;
uint16_t oBankAnd;
uint16_t iBankAnd;
uint8_t mcState;
bool mcLocked;

//normal regs
uint8_t rtcReg;
uint16_t cBank;
uint16_t extBank;
uint16_t bankMask;
uint16_t extMask;
uint16_t extAddrMask;
size_t extTotalSize;

bool RamIOAllowed;
bool extMemEnabled;
bool bankUsed;
bool extSelect;
bool rtcUsed;
bool rtcEnabled;

uint8_t lastRTCval;

//used in for example VBA
static struct RTCSave_t {
	int32_t secs;
	int32_t mins;
	int32_t hours;
	int32_t days;
	int32_t ctrl;
	int32_t lsecs;
	int32_t lmins;
	int32_t lhours;
	int32_t ldays;
	int32_t lctrl;
	int64_t lastTime;
} RTCSave;

static void noSet8(uint16_t addr, uint8_t val);
static void mbc1Set8(uint16_t addr, uint8_t val);
static void mbc1mcSet8(uint16_t addr, uint8_t val);
static void mbc2Set8(uint16_t addr, uint8_t val);
static void mbc3Set8(uint16_t addr, uint8_t val);
static void mbc5Set8(uint16_t addr, uint8_t val);
static void gbsSet8(uint16_t addr, uint8_t val);
static void mbcRTCUpdate();

static uint8_t mbcGetExtRAMBank8(uint16_t addr);
static void mbcSetExtRAMBank8(uint16_t addr, uint8_t val);
static uint8_t mbcGetExtRAMNoBank8(uint16_t addr);
static void mbcSetExtRAMNoBank8(uint16_t addr, uint8_t val);
static uint8_t mbcGetExtRAMRtc8(uint16_t addr);
static void mbcSetExtRAMRtc8(uint16_t addr, uint8_t val);
static uint8_t mbc2GetExtRAM8(uint16_t addr);
static void mbc2SetExtRAM8(uint16_t addr, uint8_t val);
static uint8_t mbcGetNoExtRAM8(uint16_t addr);
static void mbcSetNoExtRAM8(uint16_t addr, uint8_t val);

extern bool gbIsMulticart;
void mbcInit(uint8_t type)
{
	if(gbIsMulticart)
		mbcSet8 = mbc1mcSet8;
	else if(type == MBC_TYPE_1)
		mbcSet8 = mbc1Set8;
	else if(type == MBC_TYPE_2)
		mbcSet8 = mbc2Set8;
	else if(type == MBC_TYPE_3)
		mbcSet8 = mbc3Set8;
	else if(type == MBC_TYPE_5)
		mbcSet8 = mbc5Set8;
	else if(type == MBC_TYPE_GBS)
		mbcSet8 = gbsSet8;
	else
		mbcSet8 = noSet8;

	if(rtcUsed)
	{
		mbcGetRAM8 = mbcGetExtRAMRtc8;
		mbcSetRAM8 = mbcSetExtRAMRtc8;
		printf("MBC: Set RAM+RTC Functions\n");
	}
	else if(extMemEnabled)
	{
		if(type == MBC_TYPE_2)
		{
			mbcGetRAM8 = mbc2GetExtRAM8;
			mbcSetRAM8 = mbc2SetExtRAM8;
			printf("MBC: Set Special MBC2 RAM Functions\n");
		}
		else if(extTotalSize < 0x2000)
		{
			mbcGetRAM8 = mbcGetExtRAMNoBank8;
			mbcSetRAM8 = mbcSetExtRAMNoBank8;
			printf("MBC: Set RAM (No Bank) Functions\n");
		}
		else if(type == MBC_TYPE_GBS)
		{
			//GBS has 0x2000 bytes RAM but has no Bank or I/O Regs!
			mbcGetRAM8 = mbcGetExtRAMNoBank8;
			mbcSetRAM8 = mbcSetExtRAMNoBank8;
			printf("MBC: Set GBS RAM (No Bank) Functions\n");
		}
		else
		{
			mbcGetRAM8 = mbcGetExtRAMBank8;
			mbcSetRAM8 = mbcSetExtRAMBank8;
			printf("MBC: Set Normal RAM Functions\n");
		}
	}
	else
	{
		mbcGetRAM8 = mbcGetNoExtRAM8;
		mbcSetRAM8 = mbcSetNoExtRAM8;
		printf("MBC: No RAM Functions\n");
	}
	mbcExtRAMInit(type);
}

void mbcResetRegs()
{
	//multicart regs
	tBank0 = 0, tBank1 = 1;
	oBank = 0, iBank = 1;
	oBankAnd = 0x20, iBankAnd = 0x3F;
	mcState = 0, mcLocked = false;
	//normal regs
	rtcReg = 0;
	cBank = 1;
	bankMask = 1;
	extBank = 0;
	extMask = 0;
	extAddrMask = 0;
	extTotalSize = 0;
	RamIOAllowed = false;
	extMemEnabled = false;
	bankUsed = false;
	extSelect = false;
	rtcUsed = false;
	rtcEnabled = false;
	lastRTCval = 0;
}

static void noSet8(uint16_t addr, uint8_t val)
{
	(void)addr;
	(void)val;
}

static void mbc1Set8(uint16_t addr, uint8_t val)
{
	if(addr < 0x2000)
		RamIOAllowed = ((val&0xF) == 0xA);
	else if(addr >= 0x2000 && addr < 0x4000)
	{
		if(bankUsed)
		{
			//printf("%02x\n",val);
			cBank &= ~0x1F;
			cBank |= val&0x1F;
			if((cBank&0x1F) == 0)
				cBank |= 1;
			cBank &= bankMask;
		}
	}
	else if(addr >= 0x4000 && addr < 0x6000)
	{
		if(extSelect)
		{
			if(extMemEnabled)
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
				if((cBank&0x1F) == 0)
					cBank |= 1;
				cBank &= bankMask;
			}
		}
	}
	else if(addr >= 0x6000 && addr < 0x8000)
		extSelect = !!val;
}

static void mbc1mcSet8(uint16_t addr, uint8_t val)
{
	if(addr >= 0x2000 && addr < 0x4000)
	{
		iBank = val&0x3F;
		if(iBank == 0)
			iBank |= 1;
		tBank1 = ((oBank&oBankAnd)<<1)+(iBank&iBankAnd);
	}
	else if(addr >= 0x6000 && addr < 0x8000 && !mcLocked)
	{
		if(mcState == 0)
		{
			oBank = val&0x3F;
			tBank0 = ((oBank&oBankAnd)<<1);
			tBank1 = ((oBank&oBankAnd)<<1)+(iBank&iBankAnd);
		}
		else
		{
			iBankAnd=(~(val<<1))&0x3F;
			oBankAnd=0x20|(val&0x1F);
			tBank0 = ((oBank&oBankAnd)<<1);
			tBank1 = ((oBank&oBankAnd)<<1)+(iBank&iBankAnd);
			mcLocked = !!(val&0x20);
		}
		mcState^=1;
	}
}

static void mbc2Set8(uint16_t addr, uint8_t val)
{
	if(addr < 0x2000 && ((addr&0x100) == 0))
		RamIOAllowed = ((val&0xF) == 0xA);
	else if(addr >= 0x2000 && addr < 0x4000 && ((addr&0x100) == 0x100))
	{
		if(bankUsed)
		{
			cBank = val&0x7F;
			if(cBank == 0)
				cBank |= 1;
			cBank &= bankMask;
		}
	}
}

static void mbc3Set8(uint16_t addr, uint8_t val)
{
	if(addr < 0x2000)
		RamIOAllowed = ((val&0xF) == 0xA);
	else if(addr >= 0x2000 && addr < 0x4000)
	{
		if(bankUsed)
		{
			//printf("%02x\n",val);
			cBank = val&0x7F;
			if(cBank == 0)
				cBank |= 1;
			cBank &= bankMask;
		}
	}
	else if(addr >= 0x4000 && addr < 0x6000)
	{
		if((val & 0xC) == 0)
		{
			if(extMemEnabled)
			{
				extBank = val&3;
				extBank &= extMask;
			}
			rtcEnabled = false;
		}
		else if(rtcUsed)
		{
			rtcReg = val&0xF;
			rtcEnabled = true;
		}
	}
	else if(addr >= 0x6000 && addr < 0x8000)
	{
		//update latched regs with current vals
		if(lastRTCval == 0 && val == 1)
		{
			mbcRTCUpdate();
			RTCSave.lsecs = RTCSave.secs;
			RTCSave.lmins = RTCSave.mins;
			RTCSave.lhours = RTCSave.hours;
			RTCSave.ldays = RTCSave.days;
			RTCSave.lctrl = RTCSave.ctrl;
		}
		lastRTCval = val;
	}
}

static void mbc5Set8(uint16_t addr, uint8_t val)
{
	if(addr < 0x2000)
		RamIOAllowed = ((val&0xF) == 0xA);
	else if(addr >= 0x2000 && addr < 0x3000)
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
		if(extMemEnabled)
		{
			extBank = val&0xF;
			extBank &= extMask;
		}
	}
}

static void gbsSet8(uint16_t addr, uint8_t val)
{
	if(addr >= 0x2000 && addr < 0x3000)
	{
		//Some GBS files seem to follow VERY strange behaviour
		//similar to the MBC1 but mixing in bank mask
		cBank = (val&bankMask);
		if(cBank == 0)
			cBank |= 1;
	}
}

void mbcExtRAMInit(uint8_t type)
{
	if(extTotalSize == 0)
		printf("MBC: No RAM Cleared\n");
	else if(type == MBC_TYPE_2)
	{
		printf("MBC: Cleared MBC2 RAM\n");
		memset(Ext_Mem,0xF0,0x200);
	}
	else
	{
		printf("MBC: Cleared Normal RAM\n");
		memset(Ext_Mem,0,extTotalSize);
	}
}

void mbcExtRAMGBSClear()
{
	memset(Ext_Mem,0,0x2000);
}

void mbcExtRAMLoad(FILE *f)
{
	fread(Ext_Mem,1,extTotalSize,f);
	printf("MBC: Read in saved game\n");
}

void mbcExtRAMStore(FILE *f)
{
	printf("MBC: Saved game\n");
	fwrite(Ext_Mem,1,extTotalSize,f);
}

//Regular RAM for regular Controllers
uint8_t mbcGetExtRAMBank8(uint16_t addr)
{
	if(RamIOAllowed)
		return Ext_Mem[((extBank<<13)|(addr&extAddrMask))];
	return 0xFF;
}

void mbcSetExtRAMBank8(uint16_t addr, uint8_t val)
{
	if(RamIOAllowed)
		Ext_Mem[((extBank<<13)|(addr&extAddrMask))] = val;
}

//Allow Only 4 Bits to read/write
static uint8_t mbc2GetExtRAM8(uint16_t addr)
{
	if(RamIOAllowed)
		return Ext_Mem[addr&0x1FF] | 0xF0;
	return 0xFF;
}

static void mbc2SetExtRAM8(uint16_t addr, uint8_t val)
{
	if(RamIOAllowed)
		Ext_Mem[addr&0x1FF] = val | 0xF0;
}

//No Banks and No RAM IO Regs to be set
static uint8_t mbcGetExtRAMNoBank8(uint16_t addr)
{
	return Ext_Mem[addr&extAddrMask];
}

static void mbcSetExtRAMNoBank8(uint16_t addr, uint8_t val)
{
	Ext_Mem[addr&extAddrMask] = val;
}

//No RAM, just dummy functions
static uint8_t mbcGetNoExtRAM8(uint16_t addr)
{
	(void)addr;
	return 0xFF;
}

static void mbcSetNoExtRAM8(uint16_t addr, uint8_t val)
{
	(void)addr;
	(void)val;
}

size_t mbcRTCSize()
{
	return sizeof(RTCSave);
}

void mbcRTCInit()
{
	rtcUsed = true;
	//Set default values already in
	//case no save exists
	time_t curtime;
	time(&curtime);
	struct tm *lt = localtime(&curtime);
	RTCSave.secs = lt->tm_sec;
	RTCSave.mins = lt->tm_min;
	RTCSave.hours = lt->tm_hour;
	RTCSave.days = lt->tm_yday & 255;
	RTCSave.ctrl = (lt->tm_yday > 255 ? 1: 0);
	RTCSave.lastTime = curtime;
	printf("MBC: RTC allowed\n");
}

static void mbcRTCUpdate()
{
	if(RTCSave.ctrl & 0x40) //Halted!
		return;

	time_t curtime;
	time(&curtime);
	time_t diff = curtime - ((time_t)RTCSave.lastTime);
	if(diff == 0) //No Time Diff!
		return;

	RTCSave.secs += diff % 60;
	if(RTCSave.secs > 59)
	{
		RTCSave.secs -= 60;
		RTCSave.mins++;
	}

	diff /= 60;

	RTCSave.mins += diff % 60;
	if(RTCSave.mins > 60)
	{
		RTCSave.mins -= 60;
		RTCSave.hours++;
	}

	diff /= 60;

	RTCSave.hours += diff % 24;
	if(RTCSave.hours > 24)
	{
		RTCSave.hours -= 24;
		RTCSave.days++;
	}
	diff /= 24;

	RTCSave.days += diff;
	if(RTCSave.days > 511)
	{
		RTCSave.days %= 512;
		RTCSave.ctrl |= (RTCSave.ctrl&0x7E)|0x80|(RTCSave.days > 255 ? 1 : 0);
	}
	RTCSave.lastTime = curtime;
}

void mbcRTCLoad(FILE *f)
{
	fread(&RTCSave,1,mbcRTCSize(),f);
	printf("MBC: Read in RTC Save\n");
	//refresh timestamps
	mbcRTCUpdate();
}

void mbcRTCStore(FILE *f)
{
	//update regs one last time before saving
	mbcRTCUpdate();
	fwrite(&RTCSave,1,mbcRTCSize(),f);
	printf("MBC: Saved RTC\n");
}

uint8_t mbcGetExtRAMRtc8(uint16_t addr)
{
	if(!RamIOAllowed)
		return 0xFF;
	if(rtcEnabled)
	{
		uint8_t ret = 0xFF;
		//return currently latched regs
		switch(rtcReg)
		{
			case 0x8:
				ret = RTCSave.lsecs;
				break;
			case 0x9:
				ret = RTCSave.lmins;
				break;
			case 0xA:
				ret = RTCSave.lhours;
				break;
			case 0xB:
				ret = RTCSave.ldays;
				break;
			case 0xC:
				ret = RTCSave.lctrl;
				break;
			default:
				break;
		}
		return ret;
	}
	else if(extMemEnabled)
	{
		uint8_t ret = Ext_Mem[((extBank<<13)|(addr&extAddrMask))];
		return ret;
	}
	return 0xFF;
}

void mbcSetExtRAMRtc8(uint16_t addr, uint8_t val)
{
	if(!RamIOAllowed)
		return;
	if(rtcEnabled)
	{
		//refresh time to set time of write
		time_t curtime;
		time(&curtime);
		RTCSave.lastTime = curtime;
		//write into rtc regs
		switch(rtcReg)
		{
			case 0x08:
				RTCSave.secs = val;
				break;
			case 0x09:
				RTCSave.mins = val;
				break;
			case 0x0A:
				RTCSave.hours = val;
				break;
			case 0x0B:
				RTCSave.days = (RTCSave.days&0x100)|val;
				break;
			case 0x0C:
				RTCSave.ctrl = val;
				RTCSave.days = (RTCSave.days&0xFF)|((val&1)<<8);
				break;
			default:
				break;
		}
	}
	else if(extMemEnabled)
		Ext_Mem[((extBank<<13)|(addr&extAddrMask))] = val;
}
