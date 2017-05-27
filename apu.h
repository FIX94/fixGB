/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef _apu_h_
#define _apu_h_

#define NUM_BUFFERS 10

void apuInitBufs();
void apuDeinitBufs();
void apuInit();
bool apuCycle();
void apuClockTimers();
uint8_t *apuGetBuf();
uint32_t apuGetBufSize();
uint32_t apuGetFrequency();
void apuSetReg8(uint16_t addr, uint8_t val);
uint8_t apuGetReg8(uint16_t addr);


typedef struct _envelope_t {
	bool modeadd;
	uint8_t vol;
	uint8_t curVol;
	uint8_t period;
	uint8_t divider;
} envelope_t;

void doEnvelopeLogic(envelope_t *env);

#endif
