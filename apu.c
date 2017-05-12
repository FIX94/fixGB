/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <malloc.h>
#include "apu.h"
#include "audio.h"
#include "mem.h"
#include "cpu.h"

#define P1_ENABLE (1<<0)
#define P2_ENABLE (1<<1)
#define TRI_ENABLE (1<<2)
#define NOISE_ENABLE (1<<3)
#define DMC_ENABLE (1<<4)

#define PULSE_CONST_V (1<<4)
#define PULSE_HALT_LOOP (1<<5)

#define TRI_HALT_LOOP (1<<7)

#define DMC_HALT_LOOP (1<<6)
#define DMC_IRQ_ENABLE (1<<7)

static uint8_t APU_IO_Reg[0x40];

static float lpVal;
static float hpVal;
static float *apuOutBuf;
static uint32_t apuBufSize;
static uint32_t apuBufSizeBytes;
static uint32_t curBufPos;
static uint32_t apuFrequency;
static uint16_t freq1;
static uint16_t freq2;
static uint16_t triFreq;
static uint16_t noiseFreq;
static uint16_t noiseShiftReg;
static uint16_t dmcFreq;
static uint16_t dmcAddr, dmcLen, dmcSampleBuf;
static uint16_t dmcCurAddr, dmcCurLen;
static uint8_t p1LengthCtr, p2LengthCtr, noiseLengthCtr;
static uint8_t triLinearCtr, triCurLinearCtr;
static uint16_t triLengthCtr;
static uint8_t dmcVol, dmcCurVol;
static uint8_t dmcSampleRemain;
static uint8_t triVolShift;
static uint16_t modeCurCtr = 0;
static uint16_t p1freqCtr, p2freqCtr, triFreqCtr, noiseFreqCtr;
static uint8_t p1Cycle, p2Cycle, triCycle;
static uint8_t modePos = 0;
static bool p1haltloop, p2haltloop, trihaltloop, noisehaltloop;
static bool dmcstart;
static bool dmcirqenable;
static bool trireload;
static bool noiseMode1;
static bool apu_enable_irq;

static envelope_t p1Env, p2Env, noiseEnv;

typedef struct _sweep_t {
	bool enabled;
	bool start;
	bool negative;
	bool mute;
	bool chan1;
	uint8_t period;
	uint8_t divider;
	uint8_t shift;
} sweep_t;

static sweep_t p1Sweep, p2Sweep;

static float pulseLookupTbl[32];
static float tndLookupTbl[204];

//used externally
const uint8_t lengthLookupTbl[0x20] = {
	10,254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
	12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

//used externally
const uint8_t pulseSeqs[4][8] = {
	{ 0, 1, 0, 0, 0, 0, 0, 0 },
	{ 0, 1, 1, 0, 0, 0, 0, 0 },
	{ 0, 1, 1, 1, 1, 0, 0, 0 },
	{ 1, 0, 0, 1, 1, 1, 1, 1 },
};

static const uint16_t noisePeriodNtsc[8] = {
	8, 16, 32, 48, 64, 80, 96, 112,
};

//used externally
const uint16_t *noisePeriod;

static const uint8_t *p1seq = pulseSeqs[0], 
					*p2seq = pulseSeqs[1];
//extern bool dmc_interrupt;

#define M_2_PI 6.28318530717958647692

void apuInitBufs()
{
	noisePeriod = noisePeriodNtsc;
	//effective frequency for 60.000Hz Video out
	apuFrequency = 1053360;
	double dt = 1.0/((double)apuFrequency);
	//LP at 22kHz
	double rc = 1.0/(M_2_PI * 22000.0);
	lpVal = dt / (rc + dt);
	//HP at 40Hz
	rc = 1.0/(M_2_PI * 40.0);
	hpVal = rc / (rc + dt);

	apuBufSize = apuFrequency/60;
	apuBufSizeBytes = apuBufSize*sizeof(float);

	apuOutBuf = (float*)malloc(apuBufSizeBytes);

	/* https://wiki.nesdev.com/w/index.php/APU_Mixer#Lookup_Table */
	uint8_t i;
	for(i = 0; i < 32; i++)
		pulseLookupTbl[i] = 95.52 / ((8128.0 / i) + 100);
	for(i = 0; i < 204; i++)
		tndLookupTbl[i] = 163.67 / ((24329.0 / i) + 100);
}

void apuDeinitBufs()
{
	if(apuOutBuf)
		free(apuOutBuf);
	apuOutBuf = NULL;
}

void apuInit()
{
	memset(APU_IO_Reg,0,0x40);
	memset(apuOutBuf, 0, apuBufSizeBytes);
	curBufPos = 0;

	freq1 = 0; freq2 = 0; triFreq = 0; noiseFreq = 0, dmcFreq = 0;
	noiseShiftReg = 1;
	p1LengthCtr = 0; p2LengthCtr = 0;
	noiseLengthCtr = 0;	triLengthCtr = 0;
	triLinearCtr = 0; triCurLinearCtr = 0;
	dmcAddr = 0, dmcLen = 0, dmcVol = 0; dmcSampleBuf = 0;
	dmcCurAddr = 0, dmcCurLen = 0; dmcCurVol = 0;
	dmcSampleRemain = 0;
	p1freqCtr = 0; p2freqCtr = 0; triFreqCtr = 0, noiseFreqCtr = 0;
	p1Cycle = 0; p2Cycle = 0; triCycle = 0;
	triVolShift = 0;

	memset(&p1Env,0,sizeof(envelope_t));
	memset(&p2Env,0,sizeof(envelope_t));
	memset(&noiseEnv,0,sizeof(envelope_t));

	memset(&p1Sweep,0,sizeof(sweep_t));
	p1Sweep.chan1 = true; //for negative sweep
	memset(&p2Sweep,0,sizeof(sweep_t));
	p2Sweep.chan1 = false;

	p1haltloop = false;	p2haltloop = false;
	trihaltloop = false; noisehaltloop = false;
	dmcstart = false;
	dmcirqenable = false;
	trireload = false;
	noiseMode1 = false;
	//4017 starts out as 0, so enable
	apu_enable_irq = true;
}

extern uint32_t cpu_oam_dma;
void apuClockTimers()
{
	if(p1freqCtr)
		p1freqCtr--;
	if(p1freqCtr == 0)
	{
		if(freq1)
			p1freqCtr = (2048-freq1)*4;
		p1Cycle++;
		if(p1Cycle >= 8)
			p1Cycle = 0;
	}

	if(p2freqCtr)
		p2freqCtr--;
	if(p2freqCtr == 0)
	{
		if(freq2)
			p2freqCtr = (2048-freq2)*4;
		p2Cycle++;
		if(p2Cycle >= 8)
			p2Cycle = 0;
	}

	if(triFreqCtr)
		triFreqCtr--;
	if(triFreqCtr == 0)
	{
		triFreqCtr = (2048-triFreq)*2;
		triCycle++;
		if(triCycle >= 32)
			triCycle = 0;
	}

	if(noiseFreqCtr)
		noiseFreqCtr--;
	if(noiseFreqCtr == 0)
	{
		noiseFreqCtr = noiseFreq;
		uint8_t cmpBit = noiseMode1 ? (noiseShiftReg>>6)&1 : (noiseShiftReg>>1)&1;
		uint8_t cmpRes = (noiseShiftReg&1)^cmpBit;
		noiseShiftReg >>= 1;
		noiseShiftReg |= cmpRes<<14;
	}
}

static float lastHPOut = 0, lastLPOut = 0;
static uint8_t lastP1Out = 0, lastP2Out = 0, lastTriOut = 0, lastNoiseOut = 0;

extern bool emuSkipVsync, emuSkipFrame;

bool apuCycle()
{
	if(curBufPos == apuBufSize)
	{
		int updateRes = audioUpdate();
		if(updateRes == 0)
		{
			emuSkipFrame = false;
			emuSkipVsync = false;
			return false;
		}
		if(updateRes > 6)
		{
			emuSkipVsync = true;
			emuSkipFrame = true;
		}
		else
		{
			emuSkipFrame = false;
			if(updateRes > 2)
				emuSkipVsync = true;
			else
				emuSkipVsync = false;
		}
		curBufPos = 0;
	}
	uint8_t p1Out = lastP1Out, p2Out = lastP2Out, 
		triOut = lastTriOut, noiseOut = lastNoiseOut;
	if(p1LengthCtr && ((APU_IO_Reg[0x25]|(APU_IO_Reg[0x25]>>4)) & P1_ENABLE))
	{
		if(p1seq[p1Cycle] && !p1Sweep.mute && freq1 >= 0x100 && freq1 < 0x7FF)
			lastP1Out = p1Out = p1Env.vol;
		else
			p1Out = 0;
	}
	if(p2LengthCtr && ((APU_IO_Reg[0x25]|(APU_IO_Reg[0x25]>>4)) & P2_ENABLE))
	{
		if(p2seq[p2Cycle] && freq2 >= 0x100 && freq2 < 0x7FF)
			lastP2Out = p2Out = p2Env.vol;
		else
			p2Out = 0;
	}
	if(triLengthCtr && triCurLinearCtr && ((APU_IO_Reg[0x25]|(APU_IO_Reg[0x25]>>4)) & TRI_ENABLE))
	{
		uint8_t v = APU_IO_Reg[0x30+(triCycle>>1)];
		if((triCycle&1)==0)
			v >>= 4; 
		else
			v &= 0xF;
		v>>=triVolShift;
		if(v)// && triFreq >= 2)
			lastTriOut = triOut = v;
		else
			triOut = 0;
	}
	if(noiseLengthCtr && ((APU_IO_Reg[0x25]|(APU_IO_Reg[0x25]>>4)) & NOISE_ENABLE))
	{
		if((noiseShiftReg&1) == 0 && noiseFreq > 0)
			lastNoiseOut = noiseOut = noiseEnv.vol;
		else
			noiseOut = 0;
	}
	float curIn = pulseLookupTbl[p1Out + p2Out] + tndLookupTbl[(3*triOut) + (2*noiseOut) + dmcVol];
	float curLPout = lastLPOut+(lpVal*(curIn-lastLPOut));
	float curHPOut = hpVal*(lastHPOut+curLPout-curIn);
	//set output
	apuOutBuf[curBufPos] = -curHPOut;
	lastLPOut = curLPout;
	lastHPOut = curHPOut;
	curBufPos++;

	return true;
}

void doEnvelopeLogic(envelope_t *env)
{
	if(env->divider == 0)
	{
		if(env->period)
		{
			if(env->modeadd)
			{
				if(env->vol < 15)
					env->vol++;
			}
			else
			{
				if(env->vol > 0)
					env->vol--;
			}
		}
		env->divider = env->period;
	}
	else
		env->divider--;
	//too slow on its own?
	//env->envelope = (env->constant ? env->vol : env->decay);
}

void sweepUpdateFreq(sweep_t *sw, uint16_t *freq)
{
	uint16_t inFreq = *freq;
	if(sw->shift > 0)
	{
		if(sw->negative)
		{
			inFreq -= (inFreq >> sw->shift);
			if(sw->chan1 == true) inFreq--;
		}
		else
			inFreq += (inFreq >> sw->shift);
	}
	if(inFreq > 0x100 && (inFreq < 0x7FF))
	{
		sw->mute = false;
		if(sw->enabled && sw->shift)
			*freq = inFreq;
	}
	else
		sw->mute = true;
}

void doSweepLogic(sweep_t *sw, uint16_t *freq)
{
	if(sw->start)
	{
		uint8_t prevDiv = sw->divider;
		sw->divider = sw->period;
		sw->start = false;
		if(prevDiv == 0)
			sweepUpdateFreq(sw, freq);
	}
	else
	{
		if(sw->divider == 0)
		{
			sweepUpdateFreq(sw, freq);
			sw->divider = sw->period;
		}
		else
			sw->divider--;
	}
	//gets clocked too little on its own?
	/*if(inFreq < 8 || (inFreq >= 0x7FF))
		sw->mute = true;
	else
		sw->mute = false;*/
}

void apuClockA()
{
	if(p1LengthCtr)
	{
		doSweepLogic(&p1Sweep, &freq1);
		if(!p1haltloop)
			p1LengthCtr--;
	}
	if(p2LengthCtr && !p2haltloop)
		p2LengthCtr--;
	if(triLengthCtr && !trihaltloop)
		triLengthCtr--;
	if(noiseLengthCtr && !noisehaltloop)
		noiseLengthCtr--;
}

void apuClockB()
{
	if(p1LengthCtr)
		doEnvelopeLogic(&p1Env);
	if(p2LengthCtr)
		doEnvelopeLogic(&p2Env);
	if(noiseLengthCtr)
		doEnvelopeLogic(&noiseEnv);
}

//extern bool apu_interrupt;

void apuLenCycle()
{
	if(modeCurCtr)
		modeCurCtr--;
	if(modeCurCtr == 0)
	{
		apuClockA();
		modePos++;
		if(modePos >= 4)
		{
			apuClockB();
			modePos = 0;
		}
		modeCurCtr = 4096;
	}
}

void apuSet8(uint8_t reg, uint8_t val)
{
	//printf("APU %02x %02x\n", reg, val);
	APU_IO_Reg[reg] = val;
	if(reg == 0x10)
	{
		//printf("P1 sweep %02x\n", val);
		p1Sweep.enabled = true;//((val&0x80) != 0);
		p1Sweep.shift = val&7;
		p1Sweep.period = (val>>4)&7;
		//if(p1Sweep.period == 0)
		//	p1Sweep.period = 8;
		p1Sweep.negative = ((val&0x8) != 0);
		p1Sweep.start = true;
		if(freq1 > 0x100 && (freq1 < 0x7FF))
			p1Sweep.mute = false; //to be safe
		doSweepLogic(&p1Sweep, &freq1);
	}
	else if(reg == 0x11)
	{
		p1seq = pulseSeqs[val>>6];
		p1LengthCtr = 64-(val&0x3F);
		if(freq1 > 0x100 && (freq1 < 0x7FF))
			p1Sweep.mute = false; //to be safe
	}
	else if(reg == 0x12)
	{
		p1Env.vol = (val>>4)&0xF;
		p1Env.modeadd = (val&8)!=0;
		p1Env.period = val&7;
		//if(p1Env.period==0)
		//	p1Env.period=8;
		p1Env.divider = p1Env.period;
		if(freq1 > 0x100 && (freq1 < 0x7FF))
			p1Sweep.mute = false; //to be safe
	}
	else if(reg == 0x13)
	{
		freq1 = ((freq1&~0xFF) | val);
		if(freq1 > 0x100 && (freq1 < 0x7FF))
			p1Sweep.mute = false; //to be safe
	}
	else if(reg == 0x14)
	{
		p1haltloop = ((val&(1<<6)) == 0);
		if(val&(1<<7))
		{
			if(p1LengthCtr == 0)
				p1LengthCtr = 64;
			if(freq1 > 0x100 && (freq1 < 0x7FF))
				p1Sweep.mute = false; //to be safe
			doSweepLogic(&p1Sweep, &freq1);
		}
		freq1 = (freq1&0xFF) | ((val&7)<<8);
		//printf("P1 new freq %04x\n", freq1);
	}
	else if(reg == 0x16)
	{
		p2seq = pulseSeqs[val>>6];
		p2LengthCtr = 64-(val&0x3F);
	}
	else if(reg == 0x17)
	{
		p2Env.vol = (val>>4)&0xF;
		p2Env.modeadd = (val&8)!=0;
		p2Env.period = val&7;
		//if(p2Env.period==0)
		//	p2Env.period=8;
		p2Env.divider = p2Env.period;
	}
	else if(reg == 0x18)
	{
		freq2 = ((freq2&~0xFF) | val);
	}
	else if(reg == 0x19)
	{
		p2haltloop = ((val&(1<<6)) == 0);
		if(val&(1<<7))
		{
			if(p2LengthCtr == 0)
				p2LengthCtr = 64;
		}
		freq2 = (freq2&0xFF) | ((val&7)<<8);
		//printf("P2 new freq %04x\n", freq2);
	}
	else if(reg == 0x1A)
		triCurLinearCtr = ((val&0x80)!=0);
	else if(reg == 0x1B)
		triLengthCtr = 256-val;
	else if(reg == 0x1C)
	{
		//printf("TRIVolShift %i\n", (val>>5)&3);
		switch((val>>5)&3)
		{
			case 0:
				triVolShift=4;
				break;
			case 1:
				triVolShift=0;
				break;
			case 2:
				triVolShift=1;
				break;
			case 3:
				triVolShift=2;
				break;
		}
	}
	else if(reg == 0x1D)
	{
		//printf("TRI time low %02x\n", val);
		triFreq = ((triFreq&~0xFF) | val);
	}
	else if(reg == 0x1E)
	{
		trihaltloop = ((val&(1<<6)) == 0);
		if(val&(1<<7))
		{
			if(triLengthCtr == 0)
				triLengthCtr = 256;
		}
		triFreq = (triFreq&0xFF) | ((val&7)<<8);
		//printf("TRI new freq %04x\n", triFreq);
	}
	else if(reg == 0x20)
	{
		noiseLengthCtr = 64-(val&0x3F);
	}
	else if(reg == 0x21)
	{
		noiseEnv.vol = (val>>4)&0xF;
		noiseEnv.modeadd = (val&8)!=0;
		noiseEnv.period=val&7;
		//if(noiseEnv.period==0)
		//	noiseEnv.period=8;
		noiseEnv.divider = noiseEnv.period;
	}
	else if(reg == 0x22)
	{
		if((val>>4)<14)
			noiseFreq = noisePeriod[val&0x7]<<(val>>4);
		else
			noiseFreq = 0;
		noiseMode1 = ((val&0x8) != 0);
	}
	else if(reg == 0x23)
	{
		noisehaltloop = ((val&(1<<6)) == 0);
		if(val&(1<<7))
		{
			if(noiseLengthCtr == 0)
				noiseLengthCtr = 64;
		}
	}
	else if(reg == 0x25)
	{
		
	}
}

uint8_t apuGet8(uint8_t reg)
{
	//printf("%08x\n", reg);
	/*if(reg == 0x15)
	{
		//uint8_t intrflags = ((apu_interrupt<<6) | (dmc_interrupt<<7));
		//uint8_t apuretval = ((p1LengthCtr > 0) | ((p2LengthCtr > 0)<<1) | ((triLengthCtr > 0)<<2) | ((noiseLengthCtr > 0)<<3) | ((dmcCurLen > 0)<<4) | intrflags);
		//printf("Get 0x15 %02x\n",apuretval);
		//apu_interrupt = false;
		return 0;//apuretval;
	}*/
	return APU_IO_Reg[reg];
}

uint8_t *apuGetBuf()
{
	return (uint8_t*)apuOutBuf;
}

uint32_t apuGetBufSize()
{
	return apuBufSizeBytes;
}

uint32_t apuGetFrequency()
{
	return apuFrequency;
}
