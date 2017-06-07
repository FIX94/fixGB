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
extern uint32_t textureImage[0x5A00];
extern bool gbCgbMode;
//used externally
uint8_t ppuCgbBank = 0;

static uint32_t ppuClock;
static uint8_t ppuMode;
static uint8_t ppuDots;
static uint8_t ppuLines;
static uint8_t ppuLineMatch;
static uint8_t ppuOAMpos;
static uint8_t ppuOAM2pos;
static uint8_t ppuCgbBgPalPos;
static uint8_t ppuCgbObjPalPos;
static uint8_t PPU_Reg[12];
static uint8_t PPU_OAM[0xA0];
static uint8_t PPU_OAM2[0x28];
static uint8_t PPU_VRAM[0x4000];
static uint32_t PPU_BGRLUT[4];
static uint8_t PPU_CGB_BGPAL[0x40];
static uint8_t PPU_CGB_OBJPAL[0x40];
static uint32_t PPU_CGB_BGRLUT[0x8000];
static bool ppuFrameDone;
static bool ppuVBlank;
static bool ppuVBlankTriggered;
static bool ppuHBlank;
static bool ppuHBlankTriggered;

//default values when starting ROM with 0x80 and 0xC0 at 0x143
static const uint8_t defaultCGBBgPal[0x40] = {
	0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 
	0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 
	0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 
	0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 
};

static const uint8_t defaultCGBObjPal[0x40] = {
	0x00, 0x00, 0xF2, 0xAB, 0x61, 0xC2, 0xD9, 0xBA, 0x88, 0x6E, 0xDD, 0x63, 0x28, 0x27, 0xFB, 0x9F, 
	0x35, 0x42, 0xD6, 0xD4, 0x50, 0x48, 0x57, 0x5E, 0x23, 0x3E, 0x3D, 0xCA, 0x71, 0x21, 0x37, 0xC0, 
	0xC6, 0xB3, 0xFB, 0xF9, 0x08, 0x00, 0x8D, 0x29, 0xA3, 0x20, 0xDB, 0x87, 0x62, 0x05, 0x5D, 0xD4, 
	0x0E, 0x08, 0xFE, 0xAF, 0x20, 0x02, 0xD7, 0xFF, 0x07, 0x6A, 0x55, 0xEC, 0x83, 0x40, 0x0B, 0x77, 
};

void ppuInit()
{
	//Set start line
	if(gbCgbMode)
	{
		ppuClock = 170;
		ppuMode = 1;
		ppuLines = 144;
	}
	else
	{
		ppuClock = 400;
		ppuMode = 1;
		ppuLines = 153;
	}
	ppuDots = 0;
	ppuLineMatch = 0;
	ppuOAMpos = 0;
	ppuOAM2pos = 0;
	ppuCgbBgPalPos = 0;
	ppuCgbObjPalPos = 0;
	ppuCgbBank = 0;
	ppuFrameDone = false;
	ppuVBlank = false;
	ppuVBlankTriggered = false;
	ppuHBlank = false;
	ppuHBlankTriggered = false;
	//set draw method depending on DMG or CGB Mode
	ppuDrawDot = gbCgbMode?ppuDrawDotCGB:ppuDrawDotDMG;
	//init buffers
	memset(PPU_Reg,0,12);
	memset(PPU_OAM,0,0xA0);
	memset(PPU_OAM2,0,0x28);
	memset(PPU_VRAM,0,0x4000);
	PPU_Reg[4] = ppuLines;
	//set DMG BGR32 LUT
	PPU_BGRLUT[0] = 0xFFFFFFFF; //White
	PPU_BGRLUT[1] = 0xFFAAAAAA; //Light Gray
	PPU_BGRLUT[2] = 0xFF555555; //Dark Gray
	PPU_BGRLUT[3] = 0xFF000000; //Black
	//from GBC Bootrom
	memcpy(PPU_CGB_BGPAL,defaultCGBBgPal,0x40);
	memcpy(PPU_CGB_OBJPAL,defaultCGBObjPal,0x40);
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
				PPU_CGB_BGRLUT[cgb_palpos++] = 
					(r+(g*2)+(b*5)) //Blue
					| ((r+(g*6)+b)<<8) //Green
					| (((r*7)+g)<<16) //Red
					| (0xFF<<24); //Alpha
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
void ppuCycle()
{
	if(gbEmuGBSPlayback)
		goto ppuIncreasePos;
	if(!(PPU_Reg[0] & PPU_ENABLE))
		return;
	//Update line match stat
	ppuLineMatch = ((PPU_Reg[4] == PPU_Reg[5]) ? PPU_LINEMATCH : 0);
	//check for line IRQ on first clock
	if(ppuClock == 0)
	{
		//DONT check line 0 here, already done further below
		if((ppuLines > 0) && ppuLineMatch)
		{
			if(PPU_Reg[1]&PPU_LINEMATCH_IRQ)
			{
				//printf("Line STAT IRQ at %i\n",ppuLines);
				memEnableStatIrq();
			}
		}
	}
	if(ppuLines < 144)
	{
		//do OAM updates
		if(ppuClock < 80)
		{
			if(ppuClock == 0) //set OAM mode
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
			if(((ppuClock&1) == 0) && ppuOAM2pos < 10)
			{
				uint8_t OAMcYpos = PPU_OAM[(ppuOAMpos<<2)];
				if(OAMcYpos < 160)
				{
					int16_t cmpPos = ((int16_t)OAMcYpos)-16;
					uint8_t cSpriteAdd = (PPU_Reg[0] & PPU_SPRITE_8_16) ? 16 : 8;
					if(cmpPos <= ppuLines && (cmpPos+cSpriteAdd) > ppuLines)
					{
						memcpy(PPU_OAM2+(ppuOAM2pos<<2), PPU_OAM+(ppuOAMpos<<2), 4);
						ppuOAM2pos++;
					}
				}
				ppuOAMpos++;
			}
		} //enter main mode
		else if(ppuClock == 80)
		{
			ppuDots = 0; //Reset Draw Pos
			ppuMode = 3; //Main Mode
			ppuHBlank = false;
		} //draw point
		else if(ppuClock >= 92 && ppuClock < 252)
		{
			//makes it possible to draw 160x144 in here :)
			size_t drawPos = (ppuDots)+(ppuLines*160);
			//heavy cpu load from this
			ppuDrawDot(drawPos);
			ppuDots++;
		}
		else if(ppuClock == 252)
		{
			ppuMode = 0; //HBlank
			ppuHBlank = true;
			if(PPU_Reg[1]&PPU_HBLANK_IRQ)
			{
				//printf("HBlank STAT IRQ\n");
				memEnableStatIrq();
			}
		}
	} //VERY important, reg 4 to the outside gets set back to 0 early!
	else if(ppuLines == 153 && ppuClock == 4)
	{
		PPU_Reg[4] = 0;
		//check for linematch and IRQ early too
		ppuLineMatch = ((PPU_Reg[4] == PPU_Reg[5]) ? PPU_LINEMATCH : 0);
		if(ppuLineMatch)
		{
			if(PPU_Reg[1]&PPU_LINEMATCH_IRQ)
			{
				//printf("Line STAT IRQ at %i\n",PPU_Reg[4]);
				memEnableStatIrq();
			}
		}
	}
ppuIncreasePos:
	/* increase pos */
	ppuClock++;
	if(ppuClock == 456)
	{
		ppuClock = 0;
		ppuLines++;
		//check for vblank or draw done
		if(ppuLines == 144)
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
			if(PPU_Reg[1]&PPU_OAM_IRQ)
			{
				//printf("OAM STAT IRQ\n");
				memEnableStatIrq();
			}
			//printf("VBlank Start\n");
		}
		else if(ppuLines == 154)
		{
			ppuLines = 0; //Draw Done!
			ppuFrameDone = true;
			ppuVBlank = false;
		}
		//copy our line val into public reg
		PPU_Reg[4] = ppuLines;
	}
	return;
}

bool ppuDrawDone()
{
	if(ppuFrameDone)
	{
		ppuFrameDone = false;
		return true;
	}
	return false;
}

uint8_t ppuGetVRAMBank8(uint16_t addr)
{
	if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode != 3))
		return PPU_VRAM[(ppuCgbBank<<13)|(addr&0x1FFF)];
	return 0xFF;
}

uint8_t ppuGetVRAMNoBank8(uint16_t addr)
{
	if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode != 3))
		return PPU_VRAM[addr&0x1FFF];
	return 0xFF;
}

uint8_t ppuGetOAM8(uint16_t addr)
{
	if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode == 0) || (ppuMode == 1))
		return PPU_OAM[addr&0xFF];
	return 0xFF;
}

uint8_t ppuGetReg8(uint16_t addr)
{
	uint8_t reg = addr&0x3F;
	switch(reg)
	{
		case 0x0: /*case 0x1:*/ case 0x2: case 0x3: case 0x4: case 0x5:
		case 0x6: case 0x7: case 0x8: case 0x9: case 0xA: case 0xB:
			return PPU_Reg[reg];
		case 0x1:
			if(!(PPU_Reg[0] & PPU_ENABLE))
				return 0x80;
			else
				return (PPU_Reg[1]&(~7))|(ppuMode&3)|(ppuLineMatch&4)|0x80;
		case 0x28: //FF68
			return ppuCgbBgPalPos|0x40;
		case 0x29: //FF69
			if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode != 3))
				return PPU_CGB_BGPAL[ppuCgbBgPalPos&0x3F];
			return 0xFF;
		case 0x2A: //FF6A
			return ppuCgbObjPalPos|0x40;
		case 0x2B: //FF6B
			if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode != 3))
				return PPU_CGB_OBJPAL[ppuCgbObjPalPos&0x3F];
			return 0xFF;
		default:
			break;
	}
	return 0xFF;
}

void ppuSetVRAMBank8(uint16_t addr, uint8_t val)
{
	if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode != 3))
		PPU_VRAM[(ppuCgbBank<<13)|(addr&0x1FFF)] = val;
}

void ppuSetVRAMNoBank8(uint16_t addr, uint8_t val)
{
	if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode != 3))
		PPU_VRAM[addr&0x1FFF] = val;
}

void ppuSetOAM8(uint16_t addr, uint8_t val)
{
	if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode == 0) || (ppuMode == 1))
		PPU_OAM[addr&0xFF] = val;
}

void ppuSetReg8(uint16_t addr, uint8_t val)
{
	uint8_t reg = addr&0x3F;
	switch(reg)
	{
		/*case 0x0: case 0x1:*/ case 0x2: case 0x3: /*case 0x4:*/ case 0x5:
		/*case 0x6:*/ case 0x7: case 0x8: case 0x9: case 0xA: case 0xB:
			PPU_Reg[reg] = val;
			break;
		case 0x0: //Control reg
			PPU_Reg[0] = val;
			if(!(val&PPU_ENABLE))
			{
				ppuLines = 0;
				PPU_Reg[4] = 0;
				//since it resets a bit into the screen
				//it wont get through OAM fully
				ppuClock = 4;
				ppuOAMpos = 0; //Reset check pos
				ppuOAM2pos = 0; //Reset array pos
				ppuMode = 2; //OAM
				ppuHBlank = false;
			}
			break;
		case 0x1: //STAT RO Regs
			PPU_Reg[1] = (val&(~7));
			break;
		case 0x6: //OAM DMA
			PPU_Reg[6] = val;
			if(val < 0xFE)
			{
				uint8_t i;
				for(i = 0; i < 0xA0; i++)
					PPU_OAM[i] = memGet8((val<<8) | i);
			}
			break;
		case 0x28: //FF68
			ppuCgbBgPalPos = val;
			break;
		case 0x29: //FF69
			if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode != 3))
			{
				//printf("BG Write %02x to %02x\n", val, ppuCgbBgPalPos&0x3F);
				PPU_CGB_BGPAL[ppuCgbBgPalPos&0x3F] = val;
				if(ppuCgbBgPalPos&0x80) //auto-increment
					ppuCgbBgPalPos = ((ppuCgbBgPalPos+1)&0x3F)|0x80;
			}
			break;
		case 0x2A: //FF6A
			ppuCgbObjPalPos = val;
			break;
		case 0x2B: //FF6B
			if(!(PPU_Reg[0] & PPU_ENABLE) || (ppuMode != 3))
			{
				//printf("OBJ Write %02x to %02x\n", val, ppuCgbObjPalPos&0x3F);
				PPU_CGB_OBJPAL[ppuCgbObjPalPos&0x3F] = val;
				if(ppuCgbObjPalPos&0x80) //auto-increment
					ppuCgbObjPalPos = ((ppuCgbObjPalPos+1)&0x3F)|0x80;
			}
			break;
		default:
			break;
	}
}

void ppuDumpMem()
{
	FILE *f = fopen("PPU_VRAM.bin","wb");
	if(f)
	{
		fwrite(PPU_VRAM,1,gbCgbMode?0x4000:0x2000,f);
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

			uint8_t OAMcYpos = PPU_OAM2[(i<<2)];
			uint8_t cmpYPos = OAMcYpos-16;
			uint8_t cSpriteY = (ppuLines - cmpYPos)&cSpriteAnd;
			uint8_t cSpriteAdd = 0; //used to select which 8 by 16 tile
			if(cSpriteY > 7) //8 by 16 select
			{
				cSpriteAdd = 16;
				cSpriteY &= 7;
			}
			if(PPU_Reg[0] & PPU_SPRITE_8_16)
				tVal &= ~1; //clear low bit since its ALL 8 by 16 (2x the space)
			if(cSpriteByte3 & PPU_TILE_FLIP_Y)
			{
				cSpriteY ^= 7;
				if(PPU_Reg[0] & PPU_SPRITE_8_16)
					cSpriteAdd ^= 16; //8 by 16 select
			}
			uint16_t tPos = tVal<<4;
			tPos+=(cSpriteY)<<1;

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
						tCol = (PPU_Reg[9]>>(sprCol<<1));
					else
						tCol = (PPU_Reg[8]>>(sprCol<<1));
					//keep looking if there is a lower X
					cPrioSpriteX = OAMcXpos;
					continue;
				} //sprite has low priority and BG is not 0, keep BG for now
				else if((color&3) != 0)
					continue;
				//background is 0 so set color
				if(cSpriteByte3 & PPU_TILE_DMG_PAL)
					tCol = (PPU_Reg[9]>>(sprCol<<1));
				else
					tCol = (PPU_Reg[8]>>(sprCol<<1));
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
		uint8_t bgYPos = ppuLines+PPU_Reg[2];
		uint16_t vramTilePos = ((PPU_Reg[0]&PPU_BG_TILEMAP_UP)?0x1C00:0x1800)|(((bgXPos>>3)+((bgYPos>>3)<<5))&0x3FF);
		if(PPU_Reg[0]&PPU_BG_TILEDAT_LOW)
		{
			uint8_t tVal = PPU_VRAM[vramTilePos&0x1FFF];
			uint16_t tPos = tVal<<4;
			tPos+=(bgYPos&7)<<1;
			ChrRegA = PPU_VRAM[(tPos)&0x1FFF];
			ChrRegB = PPU_VRAM[(tPos+1)&0x1FFF];
		}
		else
		{
			int8_t tVal = (int8_t)PPU_VRAM[vramTilePos&0x1FFF];
			int16_t tPos = tVal<<4;
			tPos+=(bgYPos&7)<<1;
			ChrRegA = PPU_VRAM[(0x1000+tPos)&0x1FFF];
			ChrRegB = PPU_VRAM[(0x1000+tPos+1)&0x1FFF];
		}
		if(ChrRegA & (0x80>>(bgXPos&7)))
			color |= 1;
		if(ChrRegB & (0x80>>(bgXPos&7)))
			color |= 2;
		tCol = (PPU_Reg[7]>>(color<<1));
	}
	if(PPU_Reg[0]&PPU_WINDOW_ENABLE && (PPU_Reg[0xB]) <= ppuDots+7 && PPU_Reg[0xA] <= ppuLines)
	{
		uint8_t windowXPos = ppuDots+7-PPU_Reg[0xB];
		uint8_t windowYPos = ppuLines-PPU_Reg[0xA];
		uint16_t vramTilePos = ((PPU_Reg[0]&PPU_WINDOW_TILEMAP_UP)?0x1C00:0x1800)|(((windowXPos>>3)+((windowYPos>>3)<<5))&0x3FF);
		if(PPU_Reg[0]&PPU_BG_TILEDAT_LOW)
		{
			uint8_t tVal = PPU_VRAM[vramTilePos&0x1FFF];
			uint16_t tPos = tVal<<4;
			tPos+=(windowYPos&7)<<1;
			ChrRegA = PPU_VRAM[(tPos)&0x1FFF];
			ChrRegB = PPU_VRAM[(tPos+1)&0x1FFF];
		}
		else
		{
			int8_t tVal = (int8_t)PPU_VRAM[vramTilePos&0x1FFF];
			int16_t tPos = tVal<<4;
			tPos+=(windowYPos&7)<<1;
			ChrRegA = PPU_VRAM[(0x1000+tPos)&0x1FFF];
			ChrRegB = PPU_VRAM[(0x1000+tPos+1)&0x1FFF];
		}
		color = 0;
		if(ChrRegA & (0x80>>(windowXPos&7)))
			color |= 1;
		if(ChrRegB & (0x80>>(windowXPos&7)))
			color |= 2;
		tCol = (PPU_Reg[7]>>(color<<1));
	}
	if(PPU_Reg[0]&PPU_SPRITE_ENABLE)
		tCol = ppuDoSpritesDMG(color, tCol);
	//copy grayscale value from BGR32 LUT
	textureImage[drawPos] = PPU_BGRLUT[tCol&3];
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

			uint8_t OAMcYpos = PPU_OAM2[(i<<2)];
			uint8_t cmpYPos = OAMcYpos-16;
			uint8_t cSpriteY = (ppuLines - cmpYPos)&cSpriteAnd;
			uint8_t cSpriteAdd = 0; //used to select which 8 by 16 tile
			if(cSpriteY > 7) //8 by 16 select
			{
				cSpriteAdd = 16;
				cSpriteY &= 7;
			}
			if(PPU_Reg[0] & PPU_SPRITE_8_16)
				tVal &= ~1; //clear low bit since its ALL 8 by 16 (2x the space)
			if(cSpriteByte3 & PPU_TILE_FLIP_Y)
			{
				cSpriteY ^= 7;
				if(PPU_Reg[0] & PPU_SPRITE_8_16)
					cSpriteAdd ^= 16; //8 by 16 select
			}
			uint16_t tPos = tVal<<4;
			tPos+=(cSpriteY)<<1;

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
	uint8_t bgYPos = ppuLines+PPU_Reg[2];
	uint16_t vramTilePos = ((PPU_Reg[0]&PPU_BG_TILEMAP_UP)?0x1C00:0x1800)|(((bgXPos>>3)+((bgYPos>>3)<<5))&0x3FF);
	uint8_t tCgbVal = PPU_VRAM[0x2000|(vramTilePos&0x1FFF)];
	uint16_t tCgbBank = (tCgbVal&PPU_TILE_CGB_BANK)?0x2000:0x0;
	if(tCgbVal & PPU_TILE_FLIP_Y)
		bgYPos ^= 7;
	if(PPU_Reg[0]&PPU_BG_TILEDAT_LOW)
	{
		uint8_t tVal = PPU_VRAM[vramTilePos&0x1FFF];
		uint16_t tPos = tVal<<4;
		tPos+=(bgYPos&7)<<1;
		ChrRegA = PPU_VRAM[tCgbBank|((tPos)&0x1FFF)];
		ChrRegB = PPU_VRAM[tCgbBank|((tPos+1)&0x1FFF)];
	}
	else
	{
		int8_t tVal = (int8_t)PPU_VRAM[vramTilePos&0x1FFF];
		int16_t tPos = tVal<<4;
		tPos+=(bgYPos&7)<<1;
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
	if(PPU_Reg[0]&PPU_WINDOW_ENABLE && (PPU_Reg[0xB]) <= ppuDots+7 && PPU_Reg[0xA] <= ppuLines)
	{
		uint8_t windowXPos = ppuDots+7-PPU_Reg[0xB];
		uint8_t windowYPos = ppuLines-PPU_Reg[0xA];
		vramTilePos = ((PPU_Reg[0]&PPU_WINDOW_TILEMAP_UP)?0x1C00:0x1800)|(((windowXPos>>3)+((windowYPos>>3)<<5))&0x3FF);
		tCgbVal = PPU_VRAM[0x2000|(vramTilePos&0x1FFF)];
		tCgbBank = (tCgbVal&PPU_TILE_CGB_BANK)?0x2000:0x0;
		bgHighestPrio = (color && (tCgbVal & PPU_TILE_PRIO) && (PPU_Reg[0] & PPU_BG_WINDOW_PRIO));
		if(tCgbVal & PPU_TILE_FLIP_Y)
			windowYPos ^= 7;
		if(PPU_Reg[0]&PPU_BG_TILEDAT_LOW)
		{
			uint8_t tVal = PPU_VRAM[vramTilePos&0x1FFF];
			uint16_t tPos = tVal<<4;
			tPos+=(windowYPos&7)<<1;
			ChrRegA = PPU_VRAM[tCgbBank|((tPos)&0x1FFF)];
			ChrRegB = PPU_VRAM[tCgbBank|((tPos+1)&0x1FFF)];
		}
		else
		{
			int8_t tVal = (int8_t)PPU_VRAM[vramTilePos&0x1FFF];
			int16_t tPos = tVal<<4;
			tPos+=(windowYPos&7)<<1;
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
	//copy color value from BGR32 LUT
	textureImage[drawPos] = PPU_CGB_BGRLUT[cgbRGB&0x7FFF];
}

//64x12 1BPP "Track"
static const uint8_t ppuGBSTextTrack[96] =
{
	0x0C, 0x1C, 0x03, 0xD8, 0x7C, 0x71, 0xC0, 0x00, 0x0C, 0x1C, 0x07, 0xF8, 0xFE, 0x73, 0x80, 0x00, 
	0x0C, 0x1C, 0x06, 0x39, 0xE2, 0x77, 0x00, 0x00, 0x0C, 0x1C, 0x07, 0x39, 0xC0, 0x7E, 0x00, 0x00, 
	0x0C, 0x1C, 0x07, 0xF9, 0xC0, 0x7C, 0x00, 0x00, 0x0C, 0x1C, 0x01, 0xF9, 0xC0, 0x7C, 0x00, 0x00, 
	0x0C, 0x1E, 0x60, 0x38, 0xE2, 0x7E, 0x00, 0x00, 0x0C, 0x1F, 0xE3, 0xF8, 0xFE, 0x77, 0x00, 0x00, 
	0x0C, 0x1D, 0xC3, 0xF0, 0x3C, 0x73, 0xC0, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 
	0xFF, 0xC0, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0xFF, 0xC0, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 
};

//128x12 1BPP "0123456789/"
static const uint8_t ppuGbsTextRest[192] =
{
	0x0E, 0x1F, 0xF7, 0xF8, 0xF8, 0x03, 0x9F, 0x01, 0xF0, 0x60, 0x1F, 0x0F, 0x83, 0x00, 0x00, 0x00, 
	0x3F, 0x9F, 0xF7, 0xF9, 0xFC, 0x03, 0x9F, 0xC3, 0xF8, 0x70, 0x3F, 0x8F, 0xC3, 0x00, 0x00, 0x00, 
	0x3B, 0x83, 0x83, 0x80, 0x0E, 0x7F, 0xC1, 0xE7, 0x1C, 0x70, 0x71, 0xC0, 0xE1, 0x80, 0x00, 0x00, 
	0x71, 0xC3, 0x81, 0xC0, 0x0E, 0x7F, 0xC0, 0xE7, 0x1C, 0x30, 0x71, 0xC0, 0x71, 0x80, 0x00, 0x00, 
	0x79, 0xC3, 0x80, 0xE0, 0x0E, 0x63, 0x80, 0xE7, 0x1C, 0x38, 0x71, 0xC7, 0x70, 0xC0, 0x00, 0x00, 
	0x7D, 0xC3, 0x80, 0x70, 0x7E, 0x33, 0x81, 0xE7, 0x1C, 0x18, 0x3F, 0x8F, 0xF0, 0xC0, 0x00, 0x00, 
	0x77, 0xC3, 0x80, 0x70, 0x7C, 0x13, 0x9F, 0xC7, 0xF8, 0x1C, 0x1F, 0x1C, 0x70, 0x60, 0x00, 0x00, 
	0x73, 0xC3, 0x80, 0x38, 0x0E, 0x1B, 0x9F, 0x87, 0x70, 0x1C, 0x31, 0x9C, 0x70, 0x60, 0x00, 0x00, 
	0x71, 0xC3, 0x80, 0x38, 0x0E, 0x0B, 0x9C, 0x07, 0x00, 0x0C, 0x71, 0xDC, 0x70, 0x30, 0x00, 0x00, 
	0x3B, 0x9F, 0x80, 0x38, 0x0E, 0x0F, 0x9C, 0x03, 0x80, 0x0E, 0x71, 0xDC, 0x70, 0x30, 0x00, 0x00, 
	0x3F, 0x8F, 0x83, 0xF1, 0xFC, 0x07, 0x9F, 0xC1, 0xF9, 0xFE, 0x3F, 0x8F, 0xE0, 0x18, 0x00, 0x00, 
	0x0E, 0x03, 0x81, 0xE0, 0xF8, 0x03, 0x9F, 0xC0, 0xF9, 0xFE, 0x1F, 0x07, 0xC0, 0x18, 0x00, 0x00, 
};

static void ppuDrawRest(uint8_t curX, uint8_t sym)
{
	uint8_t i, j;
	for(i = 0; i < 12; i++)
	{
		for(j = 0; j < 10; j++)
		{
			size_t drawPos = (j+curX)+((i+4)*160);
			uint8_t xSel = (j+(sym*10));
			if(ppuGbsTextRest[((11-i)<<4)+(xSel>>3)]&(0x80>>(xSel&7)))
				textureImage[drawPos] = 0xFFFFFFFF; //White
			else
				textureImage[drawPos] = 0xFF000000; //Black
		}
	}
}

void ppuDrawGBSTrackNum(uint8_t cTrack, uint8_t trackTotal)
{
	memset(textureImage,0,0x16800);
	uint8_t curX = 4;
	//draw "Track"
	uint8_t i, j;
	for(i = 0; i < 12; i++)
	{
		for(j = 0; j < 50; j++)
		{
			size_t drawPos = (j+curX)+((i+4)*160);
			if(ppuGBSTextTrack[((11-i)<<3)+(j>>3)]&(0x80>>(j&7)))
				textureImage[drawPos] = 0xFFFFFFFF; //White
			else
				textureImage[drawPos] = 0xFF000000; //Black
		}
	}
	//"Track" len+space
	curX+=60;
	//draw current num
	if(cTrack > 99)
	{
		ppuDrawRest(curX, (cTrack/100)%10);
		curX+=10;
	}
	if(cTrack > 9)
	{
		ppuDrawRest(curX, (cTrack/10)%10);
		curX+=10;
	}
	ppuDrawRest(curX, cTrack%10);
	curX+=10;
	//draw the "/"
	ppuDrawRest(curX, 10);
	curX+=10;
	//draw total num
	if(trackTotal > 99)
	{
		ppuDrawRest(curX, (trackTotal/100)%10);
		curX+=10;
	}
	if(trackTotal > 9)
	{
		ppuDrawRest(curX, (trackTotal/10)%10);
		curX+=10;
	}
	ppuDrawRest(curX, trackTotal%10);
	curX+=10;
}
