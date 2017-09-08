/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "mem.h"
#include "cpu.h"
#include "ppu.h"
#include "apu.h"
#include "input.h"
#include "mbc.h"

//used externally
extern uint8_t Ext_Mem[0x20000];
static uint8_t Main_Mem[0x8000];
static uint8_t High_Mem[0x80];
static uint8_t gbs_prevValReads[8];
static uint8_t memLastVal;
static uint8_t irReq;
static uint8_t serialReg;
static uint8_t serialCtrlReg;
static uint8_t irqEnableReg;
static uint8_t irqFlagsReg;
//writable, undocumented regs
static uint8_t genericReg[4];
static uint16_t divRegVal;
static uint8_t timerReg;
static uint8_t timerRegVal;
static uint8_t timerResetVal;
static uint16_t timerRegBit;
static uint8_t sioTimerRegClock;
static uint8_t sioTimerRegTimer;
static uint8_t sioBitsTransfered;
static uint8_t cgbMainBank;
static bool cgbDmaActive;
static uint16_t cgbDmaSrc;
static uint16_t cgbDmaDst;
static uint8_t cgbDmaLen;
static uint8_t memDmaClock;
static bool cgbDmaHBlankMode;
static bool cgbBootromEnabled = false;
static bool timerRegEnable = false;
static bool sioTimerRegEnable = false;
static bool emuSaveEnabled = false;

//from main.c
extern bool gbCgbGame;
extern bool gbCgbMode;
extern bool gbCgbBootrom;
extern uint8_t *emuGBROM;

//from mbc.c
extern uint16_t cBank;
extern uint16_t extBank;
extern uint16_t bankMask;
extern uint16_t extMask;
extern uint16_t extAddrMask;
extern size_t extTotalSize;

//from cpu.c
extern bool cpuCgbSpeed;
extern bool cpuDoStopSwitch;

//from ppu.c
extern uint8_t ppuCgbBank;

//from mbc.c
extern bool extMemEnabled;
extern bool bankUsed;
extern bool extSelect;
extern bool rtcUsed;

static get8FuncT memGet8ptr[0x10000];
static set8FuncT memSet8ptr[0x10000];
static uint8_t memCGBBootrom[0x900];
static uint8_t memGetROMBank8(uint16_t addr);
static uint8_t memGetROMNoBank8(uint16_t addr);
static uint8_t memGetBootROMNoBank8(uint16_t addr);
static uint8_t memGetRAMBank8(uint16_t addr);
static uint8_t memGetRAMNoBank8(uint16_t addr);
static uint8_t memGetHiRAM8(uint16_t addr);
static uint8_t memGetGeneralReg8(uint16_t addr);
static uint8_t memGetInvalid8(uint16_t addr);
static void memSetRAMBank8(uint16_t addr, uint8_t val);
static void memSetRAMNoBank8(uint16_t addr, uint8_t val);
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
			printf("Mem: 32KB ROM allowed\n");
			bankMask = 1;
			break;
		case 1:
			printf("Mem: 64KB ROM allowed\n");
			bankMask = 3;
			break;
		case 2:
			printf("Mem: 128KB ROM allowed\n");
			bankMask = 7;
			break;
		case 3:
			printf("Mem: 256KB ROM allowed\n");
			bankMask = 15;
			break;
		case 4:
			printf("Mem: 512KB ROM allowed\n");
			bankMask = 31;
			break;
		case 5:
			printf("Mem: 1MB ROM allowed\n");
			bankMask = 63;
			break;
		case 6:
			printf("Mem: 2MB ROM allowed\n");
			bankMask = 127;
			break;
		case 7:
			printf("Mem: 4MB ROM allowed\n");
			bankMask = 255;
			break;
		case 8:
			printf("Mem: 8MB ROM allowed\n");
			bankMask = 511;
			break;
		case 0x52:
			printf("Mem: 1.1MB ROM allowed\n");
			bankMask = 71;
			break;
		case 0x53:
			printf("Mem: 1.2MB ROM allowed\n");
			bankMask = 79;
			break;
		case 0x54:
			printf("Mem: 1.5MB ROM allowed\n");
			bankMask = 95;
			break;
		default:
			printf("Mem: Unknown ROM Size, allowing 32KB ROM\n");
			bankMask = 1;
			break;
	}
}

static void memSetExtVal()
{
	extAddrMask = 0x1FFF;
	extMemEnabled = true;
	switch(emuGBROM[0x149])
	{
		case 0:
			if(emuGBROM[0x147] == 6)
			{
				printf("Mem: MBC2 Special RAM\n");
				extAddrMask = 0x1FF; //special case
				extTotalSize = 0x200;
				extMask = 1;
			}
			else
			{
				printf("Mem: No RAM allowed\n");
				extMemEnabled = false;
				extAddrMask = 0; //special case
				extTotalSize = 0;
				extMask = 0;
			}
			break;
		case 1:
			printf("Mem: 2KB RAM allowed\n");
			extAddrMask = 0x7FF; //special case
			extTotalSize = 0x800;
			extMask = 1;
			break;
		case 2:
			printf("Mem: 8KB RAM allowed\n");
			extTotalSize = 0x2000;
			extMask = 1;
			break;
		case 3:
			printf("Mem: 32KB RAM allowed\n");
			extTotalSize = 0x8000;
			extMask = 3;
			break;
		case 4:
			printf("Mem: 128KB RAM allowed\n");
			extTotalSize = 0x20000;
			extMask = 15;
			break;
		case 5:
			printf("Mem: 64KB RAM allowed\n");
			extTotalSize = 0x10000;
			extMask = 7;
			break;
		default:
			printf("Mem: Unknown RAM Size, allowing 8KB RAM\n");
			extTotalSize = 0x2000;
			extMask = 1;
			break;
	}
}

static uint8_t curGBS = 0;
extern uint8_t gbsTracksTotal;
extern uint32_t gbsRomSize;
bool memInit(bool romcheck, bool gbs)
{
	if(romcheck)
	{
		cBank = 1;
		bankMask = 1;
		extBank = 0;
		extMask = 0;
		extAddrMask = 0;
		extTotalSize = 0;
		bankUsed = false;
		extMemEnabled = false;
		rtcUsed = false;
		if(gbs)
		{
			bankUsed = true;
			//Get ROM Size multiple
			if(gbsRomSize <= 0x8000)
			{
				printf("Mem: 32KB ROM allowed\n");
				bankMask = 1;
			}
			else if(gbsRomSize <= 0x10000)
			{
				printf("Mem: 64KB ROM allowed\n");
				bankMask = 3;
			}
			else if(gbsRomSize <= 0x20000)
			{
				printf("Mem: 128KB ROM allowed\n");
				bankMask = 7;
			}
			else if(gbsRomSize <= 0x40000)
			{
				printf("Mem: 256KB ROM allowed\n");
				bankMask = 15;
			}
			else if(gbsRomSize <= 0x80000)
			{
				printf("Mem: 512KB ROM allowed\n");
				bankMask = 31;
			}
			else if(gbsRomSize <= 0x100000)
			{
				printf("Mem: 1MB ROM allowed\n");
				bankMask = 63;
			}
			else if(gbsRomSize <= 0x200000)
			{
				printf("Mem: 2MB ROM allowed\n");
				bankMask = 127;
			}
			else if(gbsRomSize <= 0x400000)
			{
				printf("Mem: 4MB ROM allowed\n");
				bankMask = 255;
			}
			else
			{
				printf("Mem: 8MB ROM allowed\n");
				bankMask = 511;
			}
			//Always have 8KB RAM enabled
			extMemEnabled = true;
			printf("Mem: 8KB RAM allowed\n");
			extTotalSize = 0x2000;
			extAddrMask = 0x1FFF;
			extMask = 1;
			printf("Mem: ROM and RAM (GBS)\n");
			mbcInit(MBC_TYPE_GBS);
			memset(gbs_prevValReads,0,8);
		}
		else
		{
			switch(emuGBROM[0x147])
			{
				case 0x00:
					printf("Mem: ROM Only\n");
					mbcInit(MBC_TYPE_NONE);
					break;
				case 0x01:
					memSetBankVal();
					printf("Mem: ROM Only (MBC1)\n");
					mbcInit(MBC_TYPE_1);
					break;
				case 0x02:
					memSetBankVal();
					memSetExtVal();
					printf("Mem: ROM and RAM (without save) (MBC1)\n");
					mbcInit(MBC_TYPE_1);
					break;
				case 0xFF:
					//TODO: Actually implement HuC1 functionality
				case 0x03:
					memSetBankVal();
					memSetExtVal();
					printf("Mem: ROM and RAM (with save) (MBC1)\n");
					mbcInit(MBC_TYPE_1);
					memLoadSave();
					break;
				case 0x05:
					memSetBankVal();
					printf("Mem: ROM only (MBC2)\n");
					mbcInit(MBC_TYPE_1);
					break;
				case 0x06:
					memSetBankVal();
					memSetExtVal();
					printf("Mem: ROM and RAM (with save) (MBC2)\n");
					mbcInit(MBC_TYPE_2);
					memLoadSave();
					break;
				case 0x08:
					memSetBankVal();
					memSetExtVal();
					printf("Mem: ROM and RAM (without save)\n");
					mbcInit(MBC_TYPE_NONE);
					break;
				case 0x09:
					memSetBankVal();
					memSetExtVal();
					printf("Mem: ROM and RAM (with save)\n");
					mbcInit(MBC_TYPE_NONE);
					memLoadSave();
					break;
				case 0x0F:
					memSetBankVal();
					mbcRTCInit();
					printf("Mem: ROM and RTC (MBC3)\n");
					mbcInit(MBC_TYPE_3);
					memLoadSave();
					break;
				case 0x11:
					memSetBankVal();
					printf("Mem: ROM Only (MBC3)\n");
					mbcInit(MBC_TYPE_3);
					break;
				case 0x12:
					memSetBankVal();
					memSetExtVal();
					printf("Mem: ROM and RAM (without save) (MBC3)\n");
					mbcInit(MBC_TYPE_3);
					break;
				case 0x10:
					memSetBankVal();
					memSetExtVal();
					mbcRTCInit();
					printf("Mem: ROM and RAM (with save) and RTC (MBC3)\n");
					mbcInit(MBC_TYPE_3);
					memLoadSave();
					break;
				case 0x13:
					memSetBankVal();
					memSetExtVal();
					printf("Mem: ROM and RAM (with save) (MBC3)\n");
					mbcInit(MBC_TYPE_3);
					memLoadSave();
					break;
				case 0x19:
				case 0x1C:
					memSetBankVal();
					printf("Mem: ROM Only (MBC5)\n");
					mbcInit(MBC_TYPE_5);
					break;
				case 0x1A:
				case 0x1D:
					memSetBankVal();
					memSetExtVal();
					printf("Mem: ROM and RAM (without save) (MBC5)\n");
					mbcInit(MBC_TYPE_5);
					break;
				case 0x1B:
				case 0x1E:
					memSetBankVal();
					memSetExtVal();
					printf("Mem: ROM and RAM (with save) (MBC5)\n");
					mbcInit(MBC_TYPE_5);
					memLoadSave();
					break;
				default:
					printf("Mem Error: Unsupported MBC Type %02x!\n", emuGBROM[0x147]);
					return false;
			}
		}
	}
	memset(Main_Mem,0,0x8000);
	memset(High_Mem,0,0x80);
	memset(genericReg,0,4);
	//IMPORTANT: Clear Ext RAM
	if(gbs) //On song switches
	{
		mbcExtRAMGBSClear();
		//and reset ROM Bank as well
		cBank = 1;
	}
	memLastVal = 0;
	irReq = 0;
	serialReg = 0;
	serialCtrlReg = 0;
	irqEnableReg = 0;
	irqFlagsReg = 0;
	divRegVal = 0;
	timerReg = 0;
	timerRegVal = 0;
	timerResetVal = 0;
	timerRegBit = (1<<9); //Freq 0
	sioTimerRegClock = 1;
	sioTimerRegTimer = 32;
	sioBitsTransfered = 0;
	cgbMainBank = 1;
	cgbDmaActive = false;
	cgbDmaSrc = 0;
	cgbDmaDst = 0;
	cgbDmaLen = 0;
	memDmaClock = 1;
	cgbDmaHBlankMode = false;
	timerRegEnable = false;
	sioTimerRegEnable = false;
	memInitGetSetPointers();
	return true;
}

void memInitGetSetPointers()
{
	//init memGet8 and memSet8 arrays
	uint32_t addr;
	for(addr = 0; addr < 0x10000; addr++)
	{
		if(addr < 0x4000) //0x0000 - 0x3FFF = Cartridge ROM
		{
			memGet8ptr[addr] = cgbBootromEnabled?memGetBootROMNoBank8:memGetROMNoBank8;
			memSet8ptr[addr] = mbcSet8;
		}
		else if(addr < 0x8000) //0x4000 - 0x7FFF = Cartridge ROM (possibly banked)
		{
			memGet8ptr[addr] = bankUsed?memGetROMBank8:memGetROMNoBank8;
			memSet8ptr[addr] = mbcSet8;
		}
		else if(addr < 0xA000) //0x8000 - 0x9FFF = PPU VRAM
		{
			memGet8ptr[addr] = gbCgbMode?ppuGetVRAMBank8:ppuGetVRAMNoBank8;
			memSet8ptr[addr] = gbCgbMode?ppuSetVRAMBank8:ppuSetVRAMNoBank8;
		}
		else if(addr < 0xC000) //0xA000 - 0xBFFF = Cardridge RAM
		{
			memGet8ptr[addr] = mbcGetRAM8;
			memSet8ptr[addr] = mbcSetRAM8;
		}
		else if(addr < 0xD000) //0xC000 - 0xCFFF = Main RAM
		{
			memGet8ptr[addr] = memGetRAMNoBank8;
			memSet8ptr[addr] = memSetRAMNoBank8;
		}
		else if(addr < 0xE000) //0xD000 - 0xDFFF = Main RAM (possibly banked)
		{
			memGet8ptr[addr] = gbCgbMode?memGetRAMBank8:memGetRAMNoBank8;
			memSet8ptr[addr] = gbCgbMode?memSetRAMBank8:memSetRAMNoBank8;
		}
		else if(addr < 0xF000) //0xE000 - 0xEFFF = Echo Main RAM
		{
			memGet8ptr[addr] = memGetRAMNoBank8;
			memSet8ptr[addr] = memSetRAMNoBank8;
		}
		else if(addr < 0xFE00) //0xF000 - 0xFCFF = Echo Main RAM (possibly banked)
		{
			memGet8ptr[addr] = gbCgbMode?memGetRAMBank8:memGetRAMNoBank8;
			memSet8ptr[addr] = gbCgbMode?memSetRAMBank8:memSetRAMNoBank8;
		}
		else if(addr < 0xFEA0) //0xFE00 - 0xFE9F = PPU OAM
		{
			memGet8ptr[addr] = ppuGetOAM8;
			memSet8ptr[addr] = ppuSetOAM8;
		}
		else if(addr < 0xFF00) //0xFEA0 - 0xFEFF = Unusable
		{
			memGet8ptr[addr] = memGetInvalid8;
			memSet8ptr[addr] = memSetInvalid8;
		}
		else if(addr == 0xFF00) //FF00 = Inputs
		{
			memGet8ptr[addr] = inputGet8;
			memSet8ptr[addr] = inputSet8;
		}
		else if(addr < 0xFF10) //0xFF01 - 0xFF0F = General Features
		{
			memGet8ptr[addr] = memGetGeneralReg8;
			memSet8ptr[addr] = memSetGeneralReg8;
		}
		else if(addr < 0xFF40) //0xFF10 - 0xFF3F = APU Regs
		{
			memGet8ptr[addr] = apuGetReg8;
			memSet8ptr[addr] = apuSetReg8;
		}
		else if(addr < 0xFF4C) //0xFF40 - 0xFF4B = PPU Regs
		{
			memGet8ptr[addr] = ppuGetReg8;
			memSet8ptr[addr] = ppuSetReg8;
		}
		else if(addr < 0xFF68) //0xFF4C - 0xFF67 = General CGB Features
		{
			memGet8ptr[addr] = gbCgbMode?memGetGeneralReg8:memGetInvalid8;
			memSet8ptr[addr] = gbCgbMode?memSetGeneralReg8:memSetInvalid8;
		}
		else if(addr < 0xFF6C) //0xFF68 - 0xFF6B = PPU CGB Regs
		{
			memGet8ptr[addr] = gbCgbMode?ppuGetReg8:memGetInvalid8;
			memSet8ptr[addr] = gbCgbMode?ppuSetReg8:memSetInvalid8;
		}
		else if(addr < 0xFF80) //0xFF6C - 0xFF7F = General CGB Features
		{
			memGet8ptr[addr] = memGetGeneralReg8;
			memSet8ptr[addr] = memSetGeneralReg8;
		}
		else if(addr < 0xFFFF) //0xFF80 - 0xFFFE = High RAM
		{
			memGet8ptr[addr] = memGetHiRAM8;
			memSet8ptr[addr] = memSetHiRAM8;
		}
		else if(addr == 0xFFFF) //FFFF = General Features
		{
			memGet8ptr[addr] = memGetGeneralReg8;
			memSet8ptr[addr] = memSetGeneralReg8;
		}
		else //Should never happen
			printf("Mem Warning: Address %04x uninitialized!\n", addr);
	}
}

bool memInitCGBBootrom()
{
	FILE *f = fopen("gbc_bios.bin","rb");
	if(!f) return false;
	fseek(f,0,SEEK_END);
	if(ftell(f) < 0x900)
	{
		fclose(f);
		return false;
	}
	fseek(f,0,SEEK_SET);
	fread(memCGBBootrom,1,0x900,f);
	fclose(f);
	cgbBootromEnabled = true;
	return true;
}

void memDisableCGBBootrom(uint8_t val)
{
	if(val == 0x11 && cgbBootromEnabled)
	{
		cgbBootromEnabled = false;
		//Update CGB/DMG Mode if needed
		if(!gbCgbGame) gbCgbMode = false;
		//Memory Map changes
		memInitGetSetPointers();
		ppuInitDrawPointer();
	}
}

void memStartGBS()
{
	curGBS = 1;
	//printf("Track %i/%i         ", curGBS, gbsTracksTotal);
	ppuDrawGBSTrackNum(curGBS, gbsTracksTotal);
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
	//printf("VBlank IRQ\n");
	irqFlagsReg |= 1;
}

void memEnableStatIrq()
{
	//printf("STAT IRQ\n");
	irqFlagsReg |= 2;
}

uint8_t memGet8(uint16_t addr)
{
	return memGet8ptr[addr](addr);
}

static uint8_t memGetROMBank8(uint16_t addr)
{
	return emuGBROM[(cBank<<14)|(addr&0x3FFF)];
}

static uint8_t memGetROMNoBank8(uint16_t addr)
{
	return emuGBROM[addr&0x7FFF];
}

static uint8_t memGetBootROMNoBank8(uint16_t addr)
{
	if(addr < 0x100 || (addr >= 0x200 && addr < 0x900))
		return memCGBBootrom[addr];
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

static uint8_t memGetHiRAM8(uint16_t addr)
{
	return High_Mem[addr&0x7F];
}

extern uint8_t curP1Out, curP2Out, curWavOut, curNoiseOut;
static uint8_t memGetGeneralReg8(uint16_t addr)
{
	switch(addr&0xFF)
	{
		case 0x01:
			return serialReg;
		case 0x02:
			return serialCtrlReg | (gbCgbMode ? 0x7C : 0x7E);
		case 0x04:
			return (divRegVal>>8);
		case 0x05:
			return timerRegVal;
		case 0x06:
			return timerResetVal;
		case 0x07:
			return timerReg|0xF8;
		case 0x0F:
			return irqFlagsReg|0xE0;
		case 0x4D:
			return (cpuDoStopSwitch | (cpuCgbSpeed<<7)) | 0x7E;
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
		case 0x56:
			return irReq|0x3C;
		case 0x6C:
			return (!(gbCgbMode))|0xFE;
		case 0x70:
			return (gbCgbMode)?(cgbMainBank|0xF8):0xFF;
		case 0x72:
			return genericReg[0];
		case 0x73:
			return genericReg[1];
		case 0x74:
			return (gbCgbMode)?genericReg[2]:0xFF;
		case 0x75:
			return genericReg[3]|0x8F;
		case 0x76:
			return curP1Out|(curP2Out<<4);
		case 0x77:
			return curWavOut|(curNoiseOut<<4);
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

static void memSetHiRAM8(uint16_t addr, uint8_t val)
{
	High_Mem[addr&0x7F] = val;
}

static void memSetGeneralReg8(uint16_t addr, uint8_t val)
{
	switch(addr&0xFF)
	{
		case 0x01:
			serialReg = val;
			break;
		case 0x02:
			serialCtrlReg = val&(gbCgbMode ? 0x83 : 0x81);
			sioTimerRegTimer = (serialCtrlReg&2) ? 1 : 32;
			sioTimerRegEnable = (serialCtrlReg&0x81) == 0x81;
			sioBitsTransfered = 0;
			sioTimerRegClock = 1;
			break;
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
				timerRegBit = (1<<9);
			else if((val&3)==1) //1 for 262144 Hz
				timerRegBit = (1<<3);
			else if((val&3)==2) //2 for 65536 Hz
				timerRegBit = (1<<5);
			else if((val&3)==3) //3 for 16384 Hz
				timerRegBit = (1<<7);
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
		case 0x50:
			memDisableCGBBootrom(val);
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
		case 0x56:
			irReq = val;
			break;
		case 0x70:
			if(gbCgbMode)
			{
				cgbMainBank = (val&7);
				if(cgbMainBank == 0)
					cgbMainBank = 1;
			}
			break;
		case 0x72:
			genericReg[0] = val;
			break;
		case 0x73:
			genericReg[1] = val;
			break;
		case 0x74:
			if(gbCgbMode)
				genericReg[2] = val;
			break;
		case 0x75:
			genericReg[3] = val;
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
		fwrite(Main_Mem,1,gbCgbMode?0x8000:0x2000,f);
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

extern char emuSaveName[1024];
void memLoadSave()
{
	if(emuSaveName[0] && (extTotalSize || rtcUsed))
	{
		emuSaveEnabled = true;
		FILE *save = fopen(emuSaveName, "rb");
		if(save)
		{
			fseek(save,0,SEEK_END);
			size_t saveSize = ftell(save);
			if(extTotalSize && (saveSize >= extTotalSize))
			{
				rewind(save);
				mbcExtRAMLoad(save);
				saveSize -= extTotalSize;
			}
			if(rtcUsed && (saveSize >= mbcRTCSize()))
				mbcRTCLoad(save);
			printf("Mem: Done reading %s\n", emuSaveName);
			fclose(save);
		}
	}
}

void memSaveGame()
{
	if(emuSaveName[0] && ((emuSaveEnabled && extTotalSize) || rtcUsed))
	{
		FILE *save = fopen(emuSaveName, "wb");
		if(save)
		{
			if(emuSaveEnabled && extTotalSize)
				mbcExtRAMStore(save);
			if(rtcUsed)
				mbcRTCStore(save);
			printf("Mem: Done writing %s\n", emuSaveName);
			fclose(save);
		}
	}
}

extern bool gbEmuGBSPlayback;
extern bool gbsTimerMode;
extern uint8_t inValReads[8];
static bool timerPrevTicked = false;
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
			//printf("\rTrack %i/%i         ", curGBS, gbsTracksTotal);
			ppuDrawGBSTrackNum(curGBS, gbsTracksTotal);
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
			//printf("\rTrack %i/%i         ", curGBS, gbsTracksTotal);
			ppuDrawGBSTrackNum(curGBS, gbsTracksTotal);
			cpuLoadGBS(curGBS-1);
		}
		else if(!inValReads[BUTTON_LEFT])
			gbs_prevValReads[BUTTON_LEFT] = 0;
	}

	if(sioTimerRegEnable)
	{
		//clocked at specified rate
		if(sioTimerRegClock == sioTimerRegTimer)
		{
			sioTimerRegClock = 1;
			serialReg <<= 1;
			serialReg |= 1; //no serial cable=bit set
			sioBitsTransfered++;
			if(sioBitsTransfered == 8)
			{
				//printf("SIO interrupt\n");
				sioBitsTransfered = 0;
				irqFlagsReg |= 8;
				sioTimerRegEnable = false;
				serialCtrlReg &= 3;
			}
		}
		else
			sioTimerRegClock++;
	}
}

extern bool cpuDmaHalt;
extern uint8_t cpuAddSpeed;
//clocked at 131072 Hz
void memDmaClockTimers()
{
	divRegVal += cpuAddSpeed;
	//clocked at specified rate
	if((timerRegBit&divRegVal) && timerRegEnable)
			timerPrevTicked = true;
	else if(timerPrevTicked)
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
		timerPrevTicked = false;
	}

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
