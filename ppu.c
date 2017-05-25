/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include "cpu.h"
#include "ppu.h"
#include "mem.h"

//FF40
#define PPU_BG_ENABLE (1<<0)
#define PPU_SPRITE_ENABLE (1<<1)
#define PPU_SPRITE_8_16 (1<<2)
#define PPU_BG_TILEMAP_UP (1<<3)
#define PPU_BG_TILEDAT_LOW (1<<4)
#define PPU_WINDOW_ENABLE (1<<5)
#define PPU_WINDOW_TILEMAP_UP (1<<6)
#define PPU_ENABLE (1<<7)
//FF40 in CGB Mode is different!
#define PPU_BG_WINDOW_PRIO (1<<0)

//FF41
#define PPU_LINEMATCH (1<<2)
#define PPU_HBLANK_IRQ (1<<3)
#define PPU_VBLANK_IRQ (1<<4)
#define PPU_OAM_IRQ (1<<5)
#define PPU_LINEMATCH_IRQ (1<<6)

//sprite byte 3 and CGB BG
#define PPU_TILE_CGB_BANK (1<<3)
#define PPU_TILE_DMG_PAL (1<<4)
#define PPU_TILE_FLIP_X (1<<5)
#define PPU_TILE_FLIP_Y (1<<6)
#define PPU_TILE_PRIO (1<<7)

static void ppuDrawDotDMG(size_t drawPos);
static void ppuDrawDotCGB(size_t drawPos);

typedef void (*drawFunc)(size_t);
static drawFunc ppuDrawDot = NULL;

//from main.c
extern uint8_t *textureImage;
extern bool allowCgbRegs;
//used externally
uint8_t ppuCgbBank = 0;

static uint32_t ppuClock;
static uint32_t ppuTestClock;
static uint8_t ppuMode;
static uint8_t ppuDots;
static uint8_t ppuOAMpos;
static uint8_t ppuOAM2pos;
static uint8_t ppuCgbBgPalPos;
static uint8_t ppuCgbObjPalPos;
static uint8_t ppuLycReg;
static uint8_t PPU_Reg[12];
static uint8_t PPU_OAM[0xA0];
static uint8_t PPU_OAM2[0x28];
static uint8_t PPU_VRAM[0x4000];
static uint8_t PPU_CGB_BGPAL[0x40];
static uint8_t PPU_CGB_OBJPAL[0x40];
static uint8_t PPU_CGB_BGRLUT[0x18000];
static bool ppuFrameDone;
static bool ppuVBlank;
static bool ppuVBlankTriggered;
static bool ppuHBlank;
static bool ppuHBlankTriggered;

void ppuInit()
{
	ppuClock = 0;
	ppuTestClock = 0;
	ppuMode = 0;
	ppuDots = 0;
	ppuOAMpos = 0;
	ppuOAM2pos = 0;
	ppuCgbBgPalPos = 0;
	ppuCgbObjPalPos = 0;
	ppuCgbBank = 0;
	ppuLycReg = 153; //last (first) line
	ppuFrameDone = false;
	ppuVBlank = false;
	ppuVBlankTriggered = false;
	ppuHBlank = false;
	ppuHBlankTriggered = false;
	//set draw method depending on DMG or CGB Mode
	ppuDrawDot = allowCgbRegs?ppuDrawDotCGB:ppuDrawDotDMG;
	//init buffers
	memset(PPU_Reg,0,12);
	memset(PPU_OAM,0,0xA0);
	memset(PPU_OAM2,0,0x28);
	memset(PPU_VRAM,0,0x4000);
	//set CGB palettes to white
	memset(PPU_CGB_BGPAL,0xFF,0x40);
	memset(PPU_CGB_OBJPAL,0xFF,0x40);
	//generate CGB BGR32 LUT
	uint8_t r, g, b;
	uint32_t cgb_palpos = 0;
	for(b = 0; b < 0x20; b++)
	{
		for(g = 0; g < 0x20; g++)
		{
			for(r = 0; r < 0x20; r++)
			{
				//this color mixing makes it look closer
				//to what an actual GBC screen produces
				PPU_CGB_BGRLUT[cgb_palpos++] = r+(g*2)+(b*5); //Blue
				PPU_CGB_BGRLUT[cgb_palpos++] = r+(g*6)+b; //Green
				PPU_CGB_BGRLUT[cgb_palpos++] = (r*7)+g; //Red
			}
		}
	}
	//From GB Bootrom
	PPU_Reg[0] = 0x91;
	PPU_Reg[7] = 0xFC;
	PPU_Reg[8] = 0xFF;
	PPU_Reg[9] = 0xFF;
}

extern bool gbEmuGBSPlayback;
bool ppuCycle()
{
	if(gbEmuGBSPlayback)
		goto ppuIncreasePos;
	if(!(PPU_Reg[0] & PPU_ENABLE))
		return true;
	if(PPU_Reg[4] < 144)
	{
		if(ppuClock == 0)
		{
			ppuOAMpos = 0; //Reset check pos
			ppuOAM2pos = 0; //Reset array pos
			ppuMode = 2; //OAM
			ppuHBlank = false;
			if(PPU_Reg[1]&PPU_OAM_IRQ)
			{
				//printf("OAM STAT IRQ\n");
				memEnableStatIrq();
			}
		}
		if(ppuClock == 80)
		{
			ppuDots = 0; //Reset Draw Pos
			ppuMode = 3; //Main Mode
			ppuHBlank = false;
		}
		if(ppuClock == 252)
		{
			ppuMode = 0; //HBlank
			ppuHBlank = true;
			if(PPU_Reg[1]&PPU_HBLANK_IRQ)
			{
				//printf("HBlank STAT IRQ\n");
				memEnableStatIrq();
			}
		}
		//do OAM updates?
		if(ppuClock < 80 && ((ppuClock&1) == 0) && ppuOAM2pos < 10)
		{
			uint8_t OAMcYpos = PPU_OAM[(ppuOAMpos<<2)];
			if(OAMcYpos < 160)
			{
				int16_t cmpPos = ((int16_t)OAMcYpos)-16;
				uint8_t cSpriteAdd = (PPU_Reg[0] & PPU_SPRITE_8_16) ? 16 : 8;
				if(cmpPos <= PPU_Reg[4] && (cmpPos+cSpriteAdd) > PPU_Reg[4])
				{
					memcpy(PPU_OAM2+(ppuOAM2pos<<2), PPU_OAM+(ppuOAMpos<<2), 4);
					ppuOAM2pos++;
				}
			}
			ppuOAMpos++;
		}
		//draw point?
		if(ppuClock >= 80 && ppuClock < 240)
		{
			//makes it possible to draw 160x144 in here :)
			size_t drawPos = (ppuDots*4)+(PPU_Reg[4]*160*4);
			ppuDrawDot(drawPos);
			ppuDots++;
		}
	}
ppuIncreasePos:
	/* increase pos */
	ppuTestClock++;
	ppuClock++;
	if(ppuClock == 456)
	{
		ppuClock = 0;
		PPU_Reg[4]++;
		if(PPU_Reg[4]==ppuLycReg)
		{
			if(PPU_Reg[1]&PPU_LINEMATCH_IRQ)
			{
				//printf("Line STAT IRQ at %i\n",PPU_Reg[4]);
				memEnableStatIrq();
			}
		}
		if(PPU_Reg[4] == 144)
		{
			ppuMode = 1; //VBlank
			ppuHBlank = false;
			ppuVBlank = true;
			memEnableVBlankIrq();
			if(PPU_Reg[1]&PPU_VBLANK_IRQ)
			{
				//printf("VBlank STAT IRQ\n");
				memEnableStatIrq();
			}
			//printf("VBlank Start\n");
		}
		if(PPU_Reg[4] == 154)
		{
			PPU_Reg[4] = 0; //Draw Done!
			extern int testCounter;
			//printf("Draw Done %i %i\n",testCounter,ppuTestClock);
			testCounter = 0;
			ppuTestClock = 0;
			ppuFrameDone = true;
			ppuVBlank = false;
		}
	}
	return true;
}

bool ppuDrawDone()
{
	if(ppuFrameDone)
	{
		//printf("%i\n",ppuCycles);
		//ppuCycles = 0;
		ppuFrameDone = false;
		return true;
	}
	return false;
}

uint8_t ppuGet8(uint16_t addr)
{
	uint8_t val = 0;
	if(addr >= 0x8000 && addr < 0xA000)
	{
		if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode != 3))
		{
			if(!allowCgbRegs)
				val = PPU_VRAM[addr&0x1FFF];
			else
				val = PPU_VRAM[(ppuCgbBank<<13)|(addr&0x1FFF)];
		}
		else
			val = 0xFF;
	}
	else if(addr >= 0xFE00 && addr < 0xFEA0)
	{
		if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode == 0) || (ppuMode == 1))
			val = PPU_OAM[addr&0xFF];
		else
			val = 0xFF;
	}
	else if(addr >= 0xFF40 && addr < 0xFF4C)
	{
		if(addr == 0xFF41)
		{
			if(!(PPU_Reg[0] & PPU_ENABLE))
				val = 0; //This is not all that clear anywhere...
			else
				val = (PPU_Reg[addr&0xF]&(~7))|(ppuMode&3)|((PPU_Reg[4]==ppuLycReg)?PPU_LINEMATCH:0);
		}
		else
			val = PPU_Reg[addr&0xF];
		//if(addr != 0xFF44)
		//	printf("at instr %04x:ppuGet8(%04x, %02x)\n",cpuCurPC(),addr,val);
	}
	else if(addr >= 0xFF68 && addr < 0xFF6C)
	{
		if(addr == 0xFF68)
			val = ppuCgbBgPalPos;
		else if(addr == 0xFF69)
			val = PPU_CGB_BGPAL[ppuCgbBgPalPos&0x3F];
		else if(addr == 0xFF6A)
			val = ppuCgbObjPalPos;
		else if(addr == 0xFF6B)
			val = PPU_CGB_OBJPAL[ppuCgbObjPalPos&0x3F];
	}
	return val;
}

void ppuSet8(uint16_t addr, uint8_t val)
{
	if(addr >= 0x8000 && addr < 0xA000)
	{
		if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode != 3))
		{
			if(!allowCgbRegs)
				PPU_VRAM[addr&0x1FFF] = val;
			else
				PPU_VRAM[(ppuCgbBank<<13)|(addr&0x1FFF)] = val;
		}
	}
	else if(addr >= 0xFE00 && addr < 0xFEA0)
	{
		if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode == 0) || (ppuMode == 1))
			PPU_OAM[addr&0xFF] = val;
	}
	else if(addr >= 0xFF40 && addr < 0xFF4C)
	{
		if(addr == 0xFF46) //OAM DMA
		{
			if(val < 0xFE)
			{
				uint8_t i;
				for(i = 0; i < 0xA0; i++)
					PPU_OAM[i] = memGet8((val<<8) | i);
			}
		}
		else if(addr != 0xFF44) //line count fully RO
		{
			if(addr == 0xFF41) //STAT RO Regs
				PPU_Reg[addr&0xF] = (val&(~7));
			else //other R/W regs
				PPU_Reg[addr&0xF] = val;
			if(addr == 0xFF40 && !(val&PPU_ENABLE))
			{
				PPU_Reg[4] = 0;
				ppuClock = 0;
				ppuMode = 2;
			}
			else if(addr == 0xFF45)
				ppuLycReg = ((val == 0 || val > 153) ? 153 : val);
			//	printf("ppuSet8(%04x, %02x)\n",addr,val);
		}
	}
	else if(addr >= 0xFF68 && addr < 0xFF6C)
	{
		if(addr == 0xFF68)
			ppuCgbBgPalPos = val;
		else if(addr == 0xFF69)
		{
			//printf("BG Write %02x to %02x\n", val, ppuCgbBgPalPos&0x3F);
			PPU_CGB_BGPAL[ppuCgbBgPalPos&0x3F] = val;
			if(ppuCgbBgPalPos&0x80) //auto-increment
				ppuCgbBgPalPos = ((ppuCgbBgPalPos+1)&0x3F)|0x80;
		}
		else if(addr == 0xFF6A)
			ppuCgbObjPalPos = val;
		else if(addr == 0xFF6B)
		{
			//printf("OBJ Write %02x to %02x\n", val, ppuCgbObjPalPos&0x3F);
			PPU_CGB_OBJPAL[ppuCgbObjPalPos&0x3F] = val;
			if(ppuCgbObjPalPos&0x80) //auto-increment
				ppuCgbObjPalPos = ((ppuCgbObjPalPos+1)&0x3F)|0x80;
		}
	}
}

void ppuDumpMem()
{
	FILE *f = fopen("PPU_VRAM.bin","wb");
	if(f)
	{
		fwrite(PPU_VRAM,1,allowCgbRegs?0x4000:0x2000,f);
		fclose(f);
	}
	f = fopen("PPU_OAM.bin","wb");
	if(f)
	{
		fwrite(PPU_OAM,1,0xA0,f);
		fclose(f);
	}
	/*f = fopen("PPU_Sprites.bin","wb");
	if(f)
	{
		fwrite(PPU_Sprites,1,0x20,f);
		fclose(f);
	}*/
}

bool ppuInVBlank()
{
	if(ppuVBlank)
	{
		if(ppuVBlankTriggered == false)
		{
			ppuVBlankTriggered = true;
			return true;
		}
		else
			return false;
	}
	ppuVBlankTriggered = false;
	return false;
}

bool ppuInHBlank()
{
	if(ppuHBlank)
	{
		if(ppuHBlankTriggered == false)
		{
			ppuHBlankTriggered = true;
			return true;
		}
		else
			return false;
	}
	ppuHBlankTriggered = false;
	return false;
}

static uint8_t ppuDoSpritesDMG(uint8_t color, uint8_t tCol)
{
	uint8_t i;
	uint8_t cSpriteAnd = (PPU_Reg[0] & PPU_SPRITE_8_16) ? 15 : 7;
	uint8_t cPrioSpriteX = 0xFF;
	uint8_t ChrRegA = 0, ChrRegB = 0;
	for(i = 0; i < ppuOAM2pos; i++)
	{
		uint8_t OAMcXpos = PPU_OAM2[(i<<2)+1];
		if(OAMcXpos >= 168)
			continue;
		int16_t cmpPos = ((int16_t)OAMcXpos)-8;
		if(cmpPos <= ppuDots && (cmpPos+8) > ppuDots)
		{
			uint8_t cSpriteByte3 = PPU_OAM2[(i<<2)+3];
			uint8_t tVal = PPU_OAM2[(i<<2)+2];
			uint16_t tPos = tVal*16;

			uint8_t OAMcYpos = PPU_OAM2[(i<<2)];
			uint8_t cmpYPos = OAMcYpos-16;
			uint8_t cSpriteY = (PPU_Reg[4] - cmpYPos)&cSpriteAnd;
			uint8_t cSpriteAdd = 0; //used to select which 8 by 16 tile
			if(cSpriteY > 7) //8 by 16 select
			{
				cSpriteAdd = 16;
				cSpriteY &= 7;
			}
			if(cSpriteByte3 & PPU_TILE_FLIP_Y)
			{
				cSpriteY ^= 7;
				if(PPU_Reg[0] & PPU_SPRITE_8_16)
					cSpriteAdd ^= 16; //8 by 16 select
			}
			tPos+=(cSpriteY)*2;

			ChrRegA = PPU_VRAM[(tPos+cSpriteAdd)&0x1FFF];
			ChrRegB = PPU_VRAM[(tPos+cSpriteAdd+1)&0x1FFF];

			uint8_t cSpriteX = (ppuDots - OAMcXpos)&7;
			if(cSpriteByte3 & PPU_TILE_FLIP_X)
				cSpriteX ^= 7;
			uint8_t sprCol = 0;
			if(ChrRegA & (0x80>>cSpriteX))
				sprCol |= 1;
			if(ChrRegB & (0x80>>cSpriteX))
				sprCol |= 2;

			//found possible candidate to display
			if(sprCol != 0)
			{
				//there already was a sprite set with lower X
				if(cPrioSpriteX < OAMcXpos)
					continue;
				//sprite has highest priority, return sprite
				if((cSpriteByte3 & PPU_TILE_PRIO) == 0)
				{
					//sprite so far has highest prio so set color
					if(cSpriteByte3 & PPU_TILE_DMG_PAL)
						tCol = (~(PPU_Reg[9]>>(sprCol*2)))&3;
					else
						tCol = (~(PPU_Reg[8]>>(sprCol*2)))&3;
					//keep looking if there is a lower X
					cPrioSpriteX = OAMcXpos;
					continue;
				} //sprite has low priority and BG is not 0, keep BG for now
				else if((color&3) != 0)
					continue;
				//background is 0 so set color
				if(cSpriteByte3 & PPU_TILE_DMG_PAL)
					tCol = (~(PPU_Reg[9]>>(sprCol*2)))&3;
				else
					tCol = (~(PPU_Reg[8]>>(sprCol*2)))&3;
				//keep looking if there is a lower X
				cPrioSpriteX = OAMcXpos;
				continue;
			}
			//Sprite is 0, keep looking for sprites
		}
	}
	return tCol;
}

static void ppuDrawDotDMG(size_t drawPos)
{
	uint8_t ChrRegA = 0, ChrRegB = 0, color = 0, tCol = 0;
	if(PPU_Reg[0]&PPU_BG_ENABLE)
	{
		uint8_t bgXPos = ppuDots+PPU_Reg[3];
		uint8_t bgYPos = PPU_Reg[4]+PPU_Reg[2];
		uint16_t vramTilePos = ((PPU_Reg[0]&PPU_BG_TILEMAP_UP)?0x1C00:0x1800)+(((bgXPos/8)+(bgYPos/8*32))&0x3FF);
		if(PPU_Reg[0]&PPU_BG_TILEDAT_LOW)
		{
			uint8_t tVal = PPU_VRAM[vramTilePos&0x1FFF];
			uint16_t tPos = tVal*16;
			tPos+=(bgYPos&7)*2;
			ChrRegA = PPU_VRAM[(tPos)&0x1FFF];
			ChrRegB = PPU_VRAM[(tPos+1)&0x1FFF];
		}
		else
		{
			int8_t tVal = (int8_t)PPU_VRAM[vramTilePos&0x1FFF];
			int16_t tPos = tVal*16;
			tPos+=(bgYPos&7)*2;
			ChrRegA = PPU_VRAM[(0x1000+tPos)&0x1FFF];
			ChrRegB = PPU_VRAM[(0x1000+tPos+1)&0x1FFF];
		}
		if(ChrRegA & (0x80>>(bgXPos&7)))
			color |= 1;
		if(ChrRegB & (0x80>>(bgXPos&7)))
			color |= 2;
		//again, not sure
		tCol = (~(PPU_Reg[7]>>(color*2)))&3;
	}
	if(PPU_Reg[0]&PPU_WINDOW_ENABLE && (PPU_Reg[0xB]) <= ppuDots+7 && PPU_Reg[0xA] <= PPU_Reg[4])
	{
		uint8_t windowXPos = ppuDots+7-PPU_Reg[0xB];
		uint8_t windowYPos = PPU_Reg[4]-PPU_Reg[0xA];
		uint16_t vramTilePos = ((PPU_Reg[0]&PPU_WINDOW_TILEMAP_UP)?0x1C00:0x1800)+(((windowXPos/8)+(windowYPos/8*32))&0x3FF);
		if(PPU_Reg[0]&PPU_BG_TILEDAT_LOW)
		{
			uint8_t tVal = PPU_VRAM[vramTilePos&0x1FFF];
			uint16_t tPos = tVal*16;
			tPos+=(windowYPos&7)*2;
			ChrRegA = PPU_VRAM[(tPos)&0x1FFF];
			ChrRegB = PPU_VRAM[(tPos+1)&0x1FFF];
		}
		else
		{
			int8_t tVal = (int8_t)PPU_VRAM[vramTilePos&0x1FFF];
			int16_t tPos = tVal*16;
			tPos+=(windowYPos&7)*2;
			ChrRegA = PPU_VRAM[(0x1000+tPos)&0x1FFF];
			ChrRegB = PPU_VRAM[(0x1000+tPos+1)&0x1FFF];
		}
		color = 0;
		if(ChrRegA & (0x80>>(windowXPos&7)))
			color |= 1;
		if(ChrRegB & (0x80>>(windowXPos&7)))
			color |= 2;
		//again, not sure
		tCol = (~(PPU_Reg[7]>>(color*2)))&3;
	}
	if(PPU_Reg[0]&PPU_SPRITE_ENABLE)
		tCol = ppuDoSpritesDMG(color, tCol);
	uint8_t draw = (tCol == 0) ? 0 : (tCol == 1) ? 0x55 : (tCol == 2) ? 0xAA : 0xFF;
	textureImage[drawPos] = draw;
	textureImage[drawPos+1] = draw;
	textureImage[drawPos+2] = draw;
}

static uint16_t ppuDoSpritesCGB(uint8_t color, uint16_t cgbRGB)
{
	uint8_t i;
	uint8_t cSpriteAnd = (PPU_Reg[0] & PPU_SPRITE_8_16) ? 15 : 7;
	uint8_t ChrRegA = 0, ChrRegB = 0;
	for(i = 0; i < ppuOAM2pos; i++)
	{
		uint8_t OAMcXpos = PPU_OAM2[(i<<2)+1];
		if(OAMcXpos >= 168)
			continue;
		int16_t cmpPos = ((int16_t)OAMcXpos)-8;
		if(cmpPos <= ppuDots && (cmpPos+8) > ppuDots)
		{
			uint8_t cSpriteByte3 = PPU_OAM2[(i<<2)+3];
			uint16_t tCgbBank = (cSpriteByte3&PPU_TILE_CGB_BANK)?0x2000:0x0;
			uint8_t tVal = PPU_OAM2[(i<<2)+2];
			uint16_t tPos = tVal*16;

			uint8_t OAMcYpos = PPU_OAM2[(i<<2)];
			uint8_t cmpYPos = OAMcYpos-16;
			uint8_t cSpriteY = (PPU_Reg[4] - cmpYPos)&cSpriteAnd;
			uint8_t cSpriteAdd = 0; //used to select which 8 by 16 tile
			if(cSpriteY > 7) //8 by 16 select
			{
				cSpriteAdd = 16;
				cSpriteY &= 7;
			}
			if(cSpriteByte3 & PPU_TILE_FLIP_Y)
			{
				cSpriteY ^= 7;
				if(PPU_Reg[0] & PPU_SPRITE_8_16)
					cSpriteAdd ^= 16; //8 by 16 select
			}
			tPos+=(cSpriteY)*2;

			ChrRegA = PPU_VRAM[tCgbBank|((tPos+cSpriteAdd)&0x1FFF)];
			ChrRegB = PPU_VRAM[tCgbBank|((tPos+cSpriteAdd+1)&0x1FFF)];

			uint8_t cSpriteX = (ppuDots - OAMcXpos)&7;
			if(cSpriteByte3 & PPU_TILE_FLIP_X)
				cSpriteX ^= 7;
			uint8_t sprCol = 0;
			if(ChrRegA & (0x80>>cSpriteX))
				sprCol |= 1;
			if(ChrRegB & (0x80>>cSpriteX))
				sprCol |= 2;

			//found possible candidate to display
			if(sprCol != 0)
			{
				//BG Master Disable, return sprite
				if((PPU_Reg[0] & PPU_BG_WINDOW_PRIO) == 0)
				{
					uint8_t pByte = ((cSpriteByte3&7)<<3)|(sprCol<<1);
					cgbRGB = (PPU_CGB_OBJPAL[pByte])|(PPU_CGB_OBJPAL[pByte+1]<<8);
					break;
				}
				//sprite has highest priority, return sprite
				if((cSpriteByte3 & PPU_TILE_PRIO) == 0)
				{
					uint8_t pByte = ((cSpriteByte3&7)<<3)|(sprCol<<1);
					cgbRGB = (PPU_CGB_OBJPAL[pByte])|(PPU_CGB_OBJPAL[pByte+1]<<8);
					break;
				} //sprite has low priority and BG is not 0, keep BG for now
				else if((color&3) != 0)
					continue;
				//background is 0 so set color
				uint8_t pByte = ((cSpriteByte3&7)<<3)|(sprCol<<1);
				cgbRGB = (PPU_CGB_OBJPAL[pByte])|(PPU_CGB_OBJPAL[pByte+1]<<8);
				break;
			}
			//Sprite is 0, keep looking for sprites
		}
	}
	return cgbRGB;
}

static void ppuDrawDotCGB(size_t drawPos)
{
	uint8_t ChrRegA = 0, ChrRegB = 0, color = 0;
	uint8_t bgXPos = ppuDots+PPU_Reg[3];
	uint8_t bgYPos = PPU_Reg[4]+PPU_Reg[2];
	uint16_t vramTilePos = ((PPU_Reg[0]&PPU_BG_TILEMAP_UP)?0x1C00:0x1800)+(((bgXPos/8)+(bgYPos/8*32))&0x3FF);
	uint8_t tCgbVal = PPU_VRAM[0x2000|(vramTilePos&0x1FFF)];
	uint16_t tCgbBank = (tCgbVal&PPU_TILE_CGB_BANK)?0x2000:0x0;
	if(tCgbVal & PPU_TILE_FLIP_Y)
		bgYPos ^= 7;
	if(PPU_Reg[0]&PPU_BG_TILEDAT_LOW)
	{
		uint8_t tVal = PPU_VRAM[vramTilePos&0x1FFF];
		uint16_t tPos = tVal*16;
		tPos+=(bgYPos&7)*2;
		ChrRegA = PPU_VRAM[tCgbBank|((tPos)&0x1FFF)];
		ChrRegB = PPU_VRAM[tCgbBank|((tPos+1)&0x1FFF)];
	}
	else
	{
		int8_t tVal = (int8_t)PPU_VRAM[vramTilePos&0x1FFF];
		int16_t tPos = tVal*16;
		tPos+=(bgYPos&7)*2;
		ChrRegA = PPU_VRAM[tCgbBank|((0x1000+tPos)&0x1FFF)];
		ChrRegB = PPU_VRAM[tCgbBank|((0x1000+tPos+1)&0x1FFF)];
	}
	if(tCgbVal & PPU_TILE_FLIP_X)
		bgXPos ^= 7;
	if(ChrRegA & (0x80>>(bgXPos&7)))
		color |= 1;
	if(ChrRegB & (0x80>>(bgXPos&7)))
		color |= 2;
	bool bgHighestPrio = (color && (tCgbVal & PPU_TILE_PRIO) && (PPU_Reg[0] & PPU_BG_WINDOW_PRIO));
	uint8_t pByte = ((tCgbVal&7)<<3)|(color<<1);
	uint16_t cgbRGB = (PPU_CGB_BGPAL[pByte])|(PPU_CGB_BGPAL[pByte+1]<<8);
	if(PPU_Reg[0]&PPU_WINDOW_ENABLE && (PPU_Reg[0xB]) <= ppuDots+7 && PPU_Reg[0xA] <= PPU_Reg[4])
	{
		uint8_t windowXPos = ppuDots+7-PPU_Reg[0xB];
		uint8_t windowYPos = PPU_Reg[4]-PPU_Reg[0xA];
		vramTilePos = ((PPU_Reg[0]&PPU_WINDOW_TILEMAP_UP)?0x1C00:0x1800)+(((windowXPos/8)+(windowYPos/8*32))&0x3FF);
		tCgbVal = PPU_VRAM[0x2000|(vramTilePos&0x1FFF)];
		tCgbBank = (tCgbVal&PPU_TILE_CGB_BANK)?0x2000:0x0;
		bgHighestPrio = (color && (tCgbVal & PPU_TILE_PRIO) && (PPU_Reg[0] & PPU_BG_WINDOW_PRIO));
		if(tCgbVal & PPU_TILE_FLIP_Y)
			windowYPos ^= 7;
		if(PPU_Reg[0]&PPU_BG_TILEDAT_LOW)
		{
			uint8_t tVal = PPU_VRAM[vramTilePos&0x1FFF];
			uint16_t tPos = tVal*16;
			tPos+=(windowYPos&7)*2;
			ChrRegA = PPU_VRAM[tCgbBank|((tPos)&0x1FFF)];
			ChrRegB = PPU_VRAM[tCgbBank|((tPos+1)&0x1FFF)];
		}
		else
		{
			int8_t tVal = (int8_t)PPU_VRAM[vramTilePos&0x1FFF];
			int16_t tPos = tVal*16;
			tPos+=(windowYPos&7)*2;
			ChrRegA = PPU_VRAM[tCgbBank|((0x1000+tPos)&0x1FFF)];
			ChrRegB = PPU_VRAM[tCgbBank|((0x1000+tPos+1)&0x1FFF)];
		}
		if(tCgbVal & PPU_TILE_FLIP_X)
			windowXPos ^= 7;
		color = 0;
		if(ChrRegA & (0x80>>(windowXPos&7)))
			color |= 1;
		if(ChrRegB & (0x80>>(windowXPos&7)))
			color |= 2;
		pByte = ((tCgbVal&7)<<3)|(color<<1);
		cgbRGB = (PPU_CGB_BGPAL[pByte])|(PPU_CGB_BGPAL[pByte+1]<<8);
	}
	if(!bgHighestPrio && PPU_Reg[0]&PPU_SPRITE_ENABLE)
		cgbRGB = ppuDoSpritesCGB(color, cgbRGB);
	uint32_t cgb_palpos = (cgbRGB&0x7FFF)*3;
	//copy color value from BGR32 LUT
	memcpy(textureImage+drawPos, PPU_CGB_BGRLUT+cgb_palpos, 3);
}
