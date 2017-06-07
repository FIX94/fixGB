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
#include "input.h"

//used externally by main.c
uint8_t inValReads[8];
uint8_t modeSelect = 3;

#define DEBUG_INPUT 0

void inputInit()
{
	memset(inValReads, 0, 8);
	modeSelect = 3;
}

void inputSet8(uint16_t addr, uint8_t in)
{
	(void)addr;
	modeSelect = ((in)>>4)&0x3;
	#if DEBUG_INPUT
	printf("Input: Set %02x->%02x\n",in,modeSelect);
	#endif
}

uint8_t inputGet8(uint16_t addr)
{
	(void)addr;
	uint8_t outVal = 0;
	if(modeSelect == 1)
	{
		if(inValReads[BUTTON_A])
			outVal |= 1;
		if(inValReads[BUTTON_B])
			outVal |= 2;
		if(inValReads[BUTTON_SELECT])
			outVal |= 4;
		if(inValReads[BUTTON_START])
			outVal |= 8;
	}
	else if(modeSelect == 2)
	{
		if(inValReads[BUTTON_RIGHT])
			outVal |= 1;
		if(inValReads[BUTTON_LEFT])
			outVal |= 2;
		if(inValReads[BUTTON_UP])
			outVal |= 4;
		if(inValReads[BUTTON_DOWN])
			outVal |= 8;
	}
	return (~(outVal|(modeSelect<<4)));
}

bool inputAny()
{
	return !!(inValReads[BUTTON_A]|inValReads[BUTTON_B]|inValReads[BUTTON_SELECT]|inValReads[BUTTON_START]
		|inValReads[BUTTON_RIGHT]|inValReads[BUTTON_LEFT]|inValReads[BUTTON_UP]|inValReads[BUTTON_DOWN]);
}
