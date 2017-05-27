/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include "mem.h"
#include "cpu.h"
#include "ppu.h"
#include "apu.h"
#include "input.h"
#include "mbc.h"

static uint8_t Ext_Mem[0x20000];
static uint8_t Main_Mem[0x8000];
static uint8_t High_Mem[0x80];
static uint8_t gbs_prevValReads[8];
static uint8_t memLastVal;
static uint8_t irqEnableReg;
static uint8_t irqFlagsReg;

static uint8_t divRegVal;
static uint8_t divRegClock;
static uint8_t timerReg;
static uint8_t timerRegVal;
static uint8_t timerResetVal;
static uint8_t timerRegClock;
static uint8_t timerRegTimer;
static uint8_t cgbMainBank;
static bool cgbDmaActive;
static uint16_t cgbDmaSrc;
static uint16_t cgbDmaDst;
static uint8_t cgbDmaLen;
static uint8_t memDmaClock;
static bool cgbDmaHBlankMode;
static bool timerRegEnable = false;

static bool emuSaveEnabled = false;

//from main.c
extern bool allowCgbRegs;
extern uint8_t *emuGBROM;

//from mbc.c
extern uint16_t cBank;
extern uint16_t extBank;
extern uint16_t bankMask;
extern uint16_t extMask;
extern uint16_t extTotalMask;
extern size_t extTotalSize;

//from cpu.c
extern bool cpuCgbSpeed;
extern bool cpuDoStopSwitch;

//from ppu.c
extern uint8_t ppuCgbBank;

extern bool extMemUsed;
extern bool bankUsed;
extern bool extSelect;

static get8FuncT memGet8ptr[0x10000];
static set8FuncT memSet8ptr[0x10000];
static uint16_t memGetAddr;
static uint16_t memSetAddr;
static uint8_t memGetROMBank8(uint16_t addr);
static uint8_t memGetROMNoBank8(uint16_t addr);
static uint8_t memGetRAMBank8(uint16_t addr);
static uint8_t memGetRAMNoBank8(uint16_t addr);
static uint8_t memGetExtRAM8(uint16_t addr);
static uint8_t memGetHiRAM8(uint16_t addr);
static uint8_t memGetGeneralReg8(uint16_t addr);
static uint8_t memGetInvalid8(uint16_t addr);
static void memSetRAMBank8(uint16_t addr, uint8_t val);
static void memSetRAMNoBank8(uint16_t addr, uint8_t val);
static void memSetExtRAM8(uint16_t addr, uint8_t val);
static void memSetHiRAM8(uint16_t addr, uint8_t val);
static void memSetGeneralReg8(uint16_t addr, uint8_t val);
static void memSetInvalid8(uint16_t addr, uint8_t val);

static void memLoadSave();

static void memSetBankVal()
{
	bankUsed = true;
	switch(emuGBROM[0x148])
	{
		case 0:
			printf("32KB ROM allowed\n");
			bankMask = 1;
			break;
		case 1:
			printf("64KB ROM allowed\n");
			bankMask = 3;
			break;
		case 2:
			printf("128KB ROM allowed\n");
			bankMask = 7;
			break;
		case 3:
			printf("256KB ROM allowed\n");
			bankMask = 15;
			break;
		case 4:
			printf("512KB ROM allowed\n");
			bankMask = 31;
			break;
		case 5:
			printf("1MB ROM allowed\n");
			bankMask = 63;
			break;
		case 6:
			printf("2MB ROM allowed\n");
			bankMask = 127;
			break;
		case 7:
			printf("4MB ROM allowed\n");
			bankMask = 255;
			break;
		case 8:
			printf("8MB ROM allowed\n");
			bankMask = 511;
			break;
		case 0x52:
			printf("1.1MB ROM allowed\n");
			bankMask = 71;
			break;
		case 0x53:
			printf("1.2MB ROM allowed\n");
			bankMask = 79;
			break;
		case 0x54:
			printf("1.5MB ROM allowed\n");
			bankMask = 95;
			break;
		default:
			printf("Unknown ROM Size, allowing 32KB ROM\n");
			bankMask = 1;
			break;
	}
}

static void memSetExtVal()
{
	extMemUsed = true;
	switch(emuGBROM[0x149])
	{
		case 0:
			printf("No RAM allowed\n");
			extTotalSize = 0;
			extTotalMask = 0;
			extMask = 0;
		case 1:
			printf("2KB RAM allowed\n");
			extTotalSize = 0x800;
			extTotalMask = 0x7FF;
			extMask = 1;
			break;
		case 2:
			printf("8KB RAM allowed\n");
			extTotalSize = 0x2000;
			extTotalMask = 0x1FFF;
			extMask = 1;
			break;
		case 3:
			printf("32KB RAM allowed\n");
			extTotalSize = 0x8000;
			extTotalMask = 0x1FFF;
			extMask = 3;
			break;
		case 4:
			printf("128KB RAM allowed\n");
			extTotalSize = 0x20000;
			extTotalMask = 0x1FFF;
			extMask = 15;
			break;
		case 5:
			printf("64KB RAM allowed\n");
			extTotalSize = 0x10000;
			extTotalMask = 0x1FFF;
			extMask = 7;
			break;
		default:
			printf("Unknown RAM Size, allowing 8KB RAM\n");
			extTotalMask = 0x1FFF;
			extMask = 1;
			break;
	}
}
static uint8_t curGBS = 0;
extern uint8_t gbsTracksTotal;
bool memInit(bool romcheck, bool gbs)
{
	if(romcheck)
	{
		if(gbs)
		{
			printf("GBS Mode\n");
			mbcInit(MBC_TYPE_GBS);
			bankUsed = true;
			extMemUsed = true;
			printf("8KB RAM allowed\n");
			extTotalSize = 0x2000;
			extTotalMask = 0x1FFF;
			extMask = 1;
			memset(gbs_prevValReads,0,8);
		}
		else
		{
			switch(emuGBROM[0x147])
			{
				case 0x00:
					printf("ROM Only\n");
					mbcInit(MBC_TYPE_NONE);
					bankUsed = false;
					extMemUsed = false;
					break;
				case 0x01:
					printf("ROM Only (MBC1)\n");
					mbcInit(MBC_TYPE_1);
					memSetBankVal();
					extMemUsed = false;
					break;
				case 0x02:
					printf("ROM and RAM (without save) (MBC1)\n");
					mbcInit(MBC_TYPE_1);
					memSetBankVal();
					memSetExtVal();
					break;
				case 0x03:
					printf("ROM and RAM (with save) (MBC1)\n");
					mbcInit(MBC_TYPE_1);
					memSetBankVal();
					memSetExtVal();
					memLoadSave();
					break;
				case 0x0F:
					//TODO: RTC Support
				case 0x11:
					printf("ROM Only (MBC3)\n");
					mbcInit(MBC_TYPE_3);
					memSetBankVal();
					extMemUsed = false;
					break;
				case 0x12:
					printf("ROM and RAM (without save) (MBC3)\n");
					mbcInit(MBC_TYPE_3);
					memSetBankVal();
					memSetExtVal();
					break;
				case 0x10:
					//TODO: RTC Support
				case 0x13:
					printf("ROM and RAM (with save) (MBC3)\n");
					mbcInit(MBC_TYPE_3);
					memSetBankVal();
					memSetExtVal();
					memLoadSave();
					break;
				case 0x19:
				case 0x1C:
					printf("ROM Only (MBC5)\n");
					mbcInit(MBC_TYPE_5);
					memSetBankVal();
					extMemUsed = false;
					break;
				case 0x1A:
				case 0x1D:
					printf("ROM and RAM (without save) (MBC5)\n");
					mbcInit(MBC_TYPE_5);
					memSetBankVal();
					memSetExtVal();
					break;
				case 0x1B:
				case 0x1E:
					printf("ROM and RAM (with save) (MBC5)\n");
					mbcInit(MBC_TYPE_5);
					memSetBankVal();
					memSetExtVal();
					memLoadSave();
					break;
				default:
					printf("Unsupported Type %02x!\n", emuGBROM[0x147]);
					return false;
			}
		}
	}
	memset(Main_Mem,0,0x8000);
	memset(High_Mem,0,0x80);
	memLastVal = 0;
	irqEnableReg = 0;
	irqFlagsReg = 0;
	divRegVal = 0;
	divRegClock = 1;
	timerReg = 0;
	timerRegVal = 0;
	timerResetVal = 0;
	timerRegClock = 1;
	timerRegTimer = 64; //262144 / 64 = 4096
	cgbMainBank = 1;
	cgbDmaActive = false;
	cgbDmaSrc = 0;
	cgbDmaDst = 0;
	cgbDmaLen = 0;
	memDmaClock = 1;
	cgbDmaHBlankMode = false;
	timerRegEnable = false;
	//init memGet8 and memSet8 arrays
	memGetAddr = 0;
	memSetAddr = 0;
	uint32_t addr;
	for(addr = 0; addr < 0x10000; addr++)
	{
		if(addr < 0x4000) //Cartridge ROM
		{
			memGet8ptr[addr] = memGetROMNoBank8;
			memSet8ptr[addr] = mbcSet8;
		}
		else if(addr < 0x8000) //Cartridge ROM (possibly banked)
		{
			memGet8ptr[addr] = bankUsed?memGetROMBank8:memGetROMNoBank8;
			memSet8ptr[addr] = mbcSet8;
		}
		else if(addr < 0xA000) //PPU VRAM
		{
			memGet8ptr[addr] = allowCgbRegs?ppuGetVRAMBank8:ppuGetVRAMNoBank8;
			memSet8ptr[addr] = allowCgbRegs?ppuSetVRAMBank8:ppuSetVRAMNoBank8;
		}
		else if(addr < 0xC000) //Cardridge RAM
		{
			memGet8ptr[addr] = extMemUsed?memGetExtRAM8:memGetInvalid8;
			memSet8ptr[addr] = extMemUsed?memSetExtRAM8:memSetInvalid8;
		}
		else if(addr < 0xD000) //Main RAM
		{
			memGet8ptr[addr] = memGetRAMNoBank8;
			memSet8ptr[addr] = memSetRAMNoBank8;
		}
		else if(addr < 0xE000) //Main RAM (possibly banked)
		{
			memGet8ptr[addr] = allowCgbRegs?memGetRAMBank8:memGetRAMNoBank8;
			memSet8ptr[addr] = allowCgbRegs?memSetRAMBank8:memSetRAMNoBank8;
		}
		else if(addr < 0xF000) //Echo Main RAM
		{
			memGet8ptr[addr] = memGetRAMNoBank8;
			memSet8ptr[addr] = memSetRAMNoBank8;
		}
		else if(addr < 0xFE00) //Echo Main RAM (possibly banked)
		{
			memGet8ptr[addr] = allowCgbRegs?memGetRAMBank8:memGetRAMNoBank8;
			memSet8ptr[addr] = allowCgbRegs?memSetRAMBank8:memSetRAMNoBank8;
		}
		else if(addr < 0xFEA0) //PPU OAM
		{
			memGet8ptr[addr] = ppuGetOAM8;
			memSet8ptr[addr] = ppuSetOAM8;
		}
		else if(addr < 0xFF00) //Unusable
		{
			memGet8ptr[addr] = memGetInvalid8;
			memSet8ptr[addr] = memSetInvalid8;
		}
		else if(addr == 0xFF00) //Inputs
		{
			memGet8ptr[addr] = inputGet8;
			memSet8ptr[addr] = inputSet8;
		}
		else if(addr < 0xFF10) //General Features
		{
			memGet8ptr[addr] = memGetGeneralReg8;
			memSet8ptr[addr] = memSetGeneralReg8;
		}
		else if(addr < 0xFF40) //APU Regs
		{
			memGet8ptr[addr] = apuGetReg8;
			memSet8ptr[addr] = apuSetReg8;
		}
		else if(addr < 0xFF4C) //PPU Regs
		{
			memGet8ptr[addr] = ppuGetReg8;
			memSet8ptr[addr] = ppuSetReg8;
		}
		else if(addr < 0xFF68) //General CGB Features
		{
			memGet8ptr[addr] = allowCgbRegs?memGetGeneralReg8:memGetInvalid8;
			memSet8ptr[addr] = allowCgbRegs?memSetGeneralReg8:memSetInvalid8;
		}
		else if(addr < 0xFF6C) //PPU CGB Regs
		{
			memGet8ptr[addr] = allowCgbRegs?ppuGetReg8:memGetInvalid8;
			memSet8ptr[addr] = allowCgbRegs?ppuSetReg8:memSetInvalid8;
		}
		else if(addr < 0xFF80) //General CGB Features
		{
			memGet8ptr[addr] = allowCgbRegs?memGetGeneralReg8:memGetInvalid8;
			memSet8ptr[addr] = allowCgbRegs?memSetGeneralReg8:memSetInvalid8;
		}
		else if(addr < 0xFFFF) //High RAM
		{
			memGet8ptr[addr] = memGetHiRAM8;
			memSet8ptr[addr] = memSetHiRAM8;
		}
		else if(addr == 0xFFFF) //General Features
		{
			memGet8ptr[addr] = memGetGeneralReg8;
			memSet8ptr[addr] = memSetGeneralReg8;
		}
		else //Should never happen
			printf("WARNING: Address %04x uninitialized!\n", addr);
	}
	return true;
}

void memStartGBS()
{
	curGBS = 1;
	printf("Track %i/%i         ", curGBS, gbsTracksTotal);
	cpuLoadGBS(curGBS-1);
}

uint8_t memGetCurIrqList()
{
	return (irqEnableReg & irqFlagsReg);
}

void memClearCurIrqList(uint8_t num)
{
	irqFlagsReg &= ~num;
}

void memEnableVBlankIrq()
{
	irqFlagsReg |= 1;
}

void memEnableStatIrq()
{
	irqFlagsReg |= 2;
}

uint8_t memGet8(uint16_t addr)
{
	return memGet8ptr[addr](addr);
}

static uint8_t memGetROMBank8(uint16_t addr)
{
	return emuGBROM[(cBank<<14)+(addr&0x3FFF)];
}

static uint8_t memGetROMNoBank8(uint16_t addr)
{
	return emuGBROM[addr&0x7FFF];
}

static uint8_t memGetRAMBank8(uint16_t addr)
{
	return Main_Mem[(cgbMainBank<<12)|(addr&0xFFF)];
}

static uint8_t memGetRAMNoBank8(uint16_t addr)
{
	return Main_Mem[addr&0x1FFF];
}

static uint8_t memGetExtRAM8(uint16_t addr)
{
	return Ext_Mem[((extBank<<13)+(addr&0x1FFF))&extTotalMask];
}

static uint8_t memGetHiRAM8(uint16_t addr)
{
	return High_Mem[addr&0x7F];
}

static uint8_t memGetGeneralReg8(uint16_t addr)
{
	switch(addr&0xFF)
	{
		case 0x04:
			return divRegVal;
		case 0x05:
			return timerRegVal;
		case 0x06:
			return timerResetVal;
		case 0x07:
			return timerReg;
		case 0x0F:
			return irqFlagsReg|0xE0;
		case 0x4D:
			return (cpuDoStopSwitch | (cpuCgbSpeed<<7));
		case 0x4F:
			return ppuCgbBank;
		case 0x51:
			return cgbDmaSrc>>8;
		case 0x52:
			return (cgbDmaSrc&0xFF);
		case 0x53:
			return (cgbDmaDst>>8)&0x1F;
		case 0x54:
			return (cgbDmaDst&0xFF);
		case 0x55:
			//bit 7 = 1 means NOT active
			if(!cgbDmaActive)
				return (0x80|(cgbDmaLen-1));
			else
				return cgbDmaLen-1;
		case 0x70:
			return cgbMainBank;
		case 0xFF:
			return irqEnableReg|0xE0;
		default:
			break;
	}
	return 0xFF;
}

static uint8_t memGetInvalid8(uint16_t addr)
{
	(void)addr;
	return 0xFF;
}

void memSet8(uint16_t addr, uint8_t val)
{
	memSet8ptr[addr](addr,val);
}

static void memSetRAMBank8(uint16_t addr, uint8_t val)
{
	Main_Mem[(cgbMainBank<<12)|(addr&0xFFF)] = val;
}

static void memSetRAMNoBank8(uint16_t addr, uint8_t val)
{
	Main_Mem[addr&0x1FFF] = val;
}

static void memSetExtRAM8(uint16_t addr, uint8_t val)
{
	Ext_Mem[((extBank<<13)+(addr&0x1FFF))&extTotalMask] = val;
}

static void memSetHiRAM8(uint16_t addr, uint8_t val)
{
	High_Mem[addr&0x7F] = val;
}

static void memSetGeneralReg8(uint16_t addr, uint8_t val)
{
	switch(addr&0xFF)
	{
		case 0x04:
			divRegVal = 0; //writing any val resets to 0
			break;
		case 0x05:
			timerRegVal = val;
			break;
		case 0x06:
			timerResetVal = val;
			break;
		case 0x07:
			//if(val != 0)
			//	printf("memSet8 %04x %02x\n", addr, val);
			timerReg = val; //for readback
			timerRegEnable = ((val&4)!=0);
			if((val&3)==0) //0 for 4096 Hz
				timerRegTimer = 64; //262144 / 64 = 4096
			else if((val&3)==1) //1 for 262144 Hz
				timerRegTimer = 1; //262144 / 1 = 262144
			else if((val&3)==2) //2 for 65536 Hz
				timerRegTimer = 4; //262144 / 4 = 65536
			else if((val&3)==3) //3 for 16384 Hz
				timerRegTimer = 16; //262144 / 16 = 16384
			break;
		case 0x0F:
			irqFlagsReg = val&0x1F;
			break;
		case 0x4D:
			cpuDoStopSwitch = !!(val&1);
			break;
		case 0x4F:
			ppuCgbBank = (val&1);
			break;
		case 0x51:
			cgbDmaSrc = (cgbDmaSrc&0x00FF)|(val<<8);
			break;
		case 0x52:
			cgbDmaSrc = (cgbDmaSrc&0xFF00)|(val&~0xF);
			break;
		case 0x53:
			cgbDmaDst = (cgbDmaDst&0x00FF)|((val&0x1F)<<8)|0x8000;
			break;
		case 0x54:
			cgbDmaDst = (cgbDmaDst&0xFF00)|(val&~0xF);
			break;
		case 0x55:
			//disabling ongoing HBlank DMA when disabling HBlank mode
			if(cgbDmaActive && cgbDmaHBlankMode && !(val&0x80))
				cgbDmaActive = false;
			else //enable DMA in all other cases
			{
				cgbDmaActive = true;
				cgbDmaLen = (val&0x7F)+1;
				cgbDmaHBlankMode = !!(val&0x80);
				//trigger immediately
				memDmaClock = 16;
				memDmaClockTimers();
			}
			break;
		case 0x70:
			cgbMainBank = (val&7);
			if(cgbMainBank == 0)
				cgbMainBank = 1;
			break;
		case 0xFF:
			irqEnableReg = val&0x1F;
			break;
		default:
			break;
	}
}

static void memSetInvalid8(uint16_t addr, uint8_t val)
{
	(void)addr;
	(void)val;
}

#define DEBUG_MEM_DUMP 0

void memDumpMainMem()
{
	#if DEBUG_MEM_DUMP
	FILE *f = fopen("MainMem.bin","wb");
	if(f)
	{
		fwrite(Main_Mem,1,allowCgbRegs?0x8000:0x2000,f);
		fclose(f);
	}
	f = fopen("HighMem.bin","wb");
	if(f)
	{
		fwrite(High_Mem,1,0x80,f);
		fclose(f);
	}
	ppuDumpMem();
	#endif
}

extern char *emuSaveName;
void memLoadSave()
{
	if(emuSaveName && extTotalSize)
	{
		emuSaveEnabled = true;
		FILE *save = fopen(emuSaveName, "rb");
		if(save)
		{
			fseek(save,0,SEEK_END);
			size_t saveSize = ftell(save);
			if(saveSize == extTotalSize)
			{
				rewind(save);
				fread(Ext_Mem,1,saveSize,save);
			}
			else
				printf("Save file ignored\n");
			fclose(save);
		}
	}
}

void memSaveGame()
{
	if(emuSaveName && extMask && emuSaveEnabled)
	{
		FILE *save = fopen(emuSaveName, "wb");
		if(save)
		{
			fwrite(Ext_Mem,1,extTotalSize,save);
			fclose(save);
		}
	}
}

extern bool gbEmuGBSPlayback;
extern bool gbsTimerMode;
extern uint8_t inValReads[8];

//clocked at 262144 Hz (or 2x that in CGB Mode)
void memClockTimers()
{
	if(gbEmuGBSPlayback)
	{
		if(inValReads[BUTTON_RIGHT] && !gbs_prevValReads[BUTTON_RIGHT])
		{
			gbs_prevValReads[BUTTON_RIGHT] = inValReads[BUTTON_RIGHT];
			curGBS++;
			if(curGBS > gbsTracksTotal)
				curGBS = 1;
			printf("\rTrack %i/%i         ", curGBS, gbsTracksTotal);
			cpuLoadGBS(curGBS-1);
		}
		else if(!inValReads[BUTTON_RIGHT])
			gbs_prevValReads[BUTTON_RIGHT] = 0;
		
		if(inValReads[BUTTON_LEFT] && !gbs_prevValReads[BUTTON_LEFT])
		{
			gbs_prevValReads[BUTTON_LEFT] = inValReads[BUTTON_LEFT];
			curGBS--;
			if(curGBS < 1)
				curGBS = gbsTracksTotal;
			printf("\rTrack %i/%i         ", curGBS, gbsTracksTotal);
			cpuLoadGBS(curGBS-1);
		}
		else if(!inValReads[BUTTON_LEFT])
			gbs_prevValReads[BUTTON_LEFT] = 0;
	}

	//clocked at 16384 Hz (262144 / 16 = 16384)
	if(divRegClock == 16)
	{
		divRegVal++;
		divRegClock = 1;
	}
	else
		divRegClock++;

	if(!timerRegEnable)
		return;

	//clocked at specified rate
	if(timerRegClock == timerRegTimer)
	{
		timerRegVal++;
		if(timerRegVal == 0) //set on overflow
		{
			//printf("Timer interrupt\n");
			timerRegVal = timerResetVal;
			if(!gbEmuGBSPlayback)
				irqFlagsReg |= 4;
			else if(gbsTimerMode)
				cpuPlayGBS();
		}
		timerRegClock = 1;
	}
	else
		timerRegClock++;
}

extern bool cpuDmaHalt;

//clocked at 131072 Hz
void memDmaClockTimers()
{
	if(memDmaClock >= 16)
	{
		cpuDmaHalt = false;
		if(!cgbDmaActive)
			return;
		//printf("%04x %04x %02x\n", cgbDmaSrc, cgbDmaDst, cgbDmaLen);
		if(cgbDmaLen && ((cgbDmaSrc < 0x8000) || (cgbDmaSrc >= 0xA000 && cgbDmaSrc < 0xE000)) && (cgbDmaDst >= 0x8000 && cgbDmaDst < 0xA000))
		{
			if(!cgbDmaHBlankMode || (cgbDmaHBlankMode && ppuInHBlank()))
			{
				uint8_t i;
				for(i = 0; i < 0x10; i++)
					memSet8(cgbDmaDst+i, memGet8(cgbDmaSrc+i));
				cgbDmaLen--;
				if(cgbDmaLen == 0)
					cgbDmaActive = false;
				cgbDmaSrc += 0x10;
				cgbDmaDst += 0x10;
				cpuDmaHalt = true;
			}
		}
		else
			cgbDmaActive = false;
		memDmaClock = 1;
	}
	else
		memDmaClock++;
}
