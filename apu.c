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
#define WAV_ENABLE (1<<2)
#define NOISE_ENABLE (1<<3)

static uint8_t APU_IO_Reg[0x50];

static float lpVal;
static float hpVal;
static float *apuOutBuf;
static uint32_t apuBufSize;
static uint32_t apuBufSizeBytes;
static uint32_t curBufPos;
static uint32_t apuFrequency;
static uint16_t freq1;
static uint16_t freq2;
static uint16_t wavFreq;
static uint16_t noiseFreq;
static uint16_t noiseShiftReg;
static uint8_t p1LengthCtr, p2LengthCtr, noiseLengthCtr;
static uint8_t wavLinearCtr;
static uint16_t wavLengthCtr;
static uint8_t wavVolShift;
static uint16_t modeCurCtr = 0;
static uint16_t p1freqCtr, p2freqCtr, wavFreqCtr, noiseFreqCtr;
static uint8_t p1Cycle, p2Cycle, wavCycle;
static uint8_t modePos = 0;
static bool p1haltloop, p2haltloop, wavhaltloop, noisehaltloop;
static bool p1dacenable, p2dacenable, wavdacenable, noisedacenable;
static bool p1enable, p2enable, wavenable, noiseenable;
static bool soundEnabled;
static bool noiseMode1;

static envelope_t p1Env, p2Env, noiseEnv;

typedef struct _sweep_t {
	bool enabled;
	bool negative;
	uint8_t period;
	uint8_t divider;
	uint8_t shift;
} sweep_t;

static sweep_t p1Sweep;

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

#define M_2_PI 6.28318530717958647692

void apuInitBufs()
{
	noisePeriod = noisePeriodNtsc;
	//effective frequency for 60.000Hz Video out
	apuFrequency = 526680;
	double dt = 1.0/((double)apuFrequency);
	//LP at 22kHz
	double rc = 1.0/(M_2_PI * 22000.0);
	lpVal = dt / (rc + dt);
	//HP at 40Hz
	rc = 1.0/(M_2_PI * 40.0);
	hpVal = rc / (rc + dt);

	apuBufSize = apuFrequency/60*2;
	apuBufSizeBytes = apuBufSize*sizeof(float);

	apuOutBuf = (float*)malloc(apuBufSizeBytes);
}

void apuDeinitBufs()
{
	if(apuOutBuf)
		free(apuOutBuf);
	apuOutBuf = NULL;
}

void apuInit()
{
	memset(APU_IO_Reg,0,0x50);
	memset(apuOutBuf, 0, apuBufSizeBytes);
	curBufPos = 0;

	freq1 = 0; freq2 = 0; wavFreq = 0; noiseFreq = 0;
	noiseShiftReg = 1;
	p1LengthCtr = 0; p2LengthCtr = 0;
	noiseLengthCtr = 0;	wavLengthCtr = 0;
	wavLinearCtr = 0;
	p1freqCtr = 0; p2freqCtr = 0; wavFreqCtr = 0, noiseFreqCtr = 0;
	p1Cycle = 0; p2Cycle = 0; wavCycle = 0;
	wavVolShift = 0;

	memset(&p1Env,0,sizeof(envelope_t));
	memset(&p2Env,0,sizeof(envelope_t));
	memset(&noiseEnv,0,sizeof(envelope_t));

	memset(&p1Sweep,0,sizeof(sweep_t));

	p1haltloop = false;	p2haltloop = false;
	wavhaltloop = false; noisehaltloop = false;
	p1enable = false; p2enable = false;
	wavenable = false; noiseenable = false;
	p1dacenable = false; p2dacenable = false;
	wavdacenable = false; noisedacenable = false;
	noiseMode1 = false;
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

	if(wavFreqCtr)
		wavFreqCtr--;
	if(wavFreqCtr == 0)
	{
		wavFreqCtr = (2048-wavFreq)*2;
		wavCycle++;
		if(wavCycle >= 32)
			wavCycle = 0;
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

static float lastHPOut[2] = { 0, 0 }, lastLPOut[2] = { 0, 0 };
static uint8_t lastP1Out[2] = { 0, 0 }, lastP2Out[2] = { 0, 0 }, lastwavOut[2] = { 0, 0 }, lastNoiseOut[2] = { 0, 0 };
static uint8_t apuCurChan = 0;
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

	uint8_t p1Out = lastP1Out[apuCurChan], p2Out = lastP2Out[apuCurChan], 
		wavOut = lastwavOut[apuCurChan], noiseOut = lastNoiseOut[apuCurChan];
	uint8_t apuEnableReg = (apuCurChan==0)?(APU_IO_Reg[0x25]>>4):(APU_IO_Reg[0x25]&0xF);
	if(p1enable && p1dacenable && (apuEnableReg & P1_ENABLE))
	{
		if(p1seq[p1Cycle] && freq1 > 0 && freq1 <= 0x7FF)
			lastP1Out[apuCurChan] = p1Out = p1Env.vol;
		else
			p1Out = 0;
	}
	if(p2enable && p2dacenable && (apuEnableReg & P2_ENABLE))
	{
		if(p2seq[p2Cycle] && freq2 > 0 && freq2 <= 0x7FF)
			lastP2Out[apuCurChan] = p2Out = p2Env.vol;
		else
			p2Out = 0;
	}
	if(wavenable && wavdacenable && (apuEnableReg & WAV_ENABLE))
	{
		uint8_t v = APU_IO_Reg[0x30+(wavCycle>>1)];
		if((wavCycle&1)==0)
			v >>= 4; 
		else
			v &= 0xF;
		v>>=wavVolShift;
		if(v)// && wavFreq >= 2)
			lastwavOut[apuCurChan] = wavOut = v;
		else
			wavOut = 0;
	}
	if(noiseenable && noisedacenable && (apuEnableReg & NOISE_ENABLE))
	{
		if((noiseShiftReg&1) == 0 && noiseFreq > 0)
			lastNoiseOut[apuCurChan] = noiseOut = noiseEnv.vol;
		else
			noiseOut = 0;
	}
	//should be 60.f at max but that'd be a tad too loud after LP and HP
	float curIn = ((float)(p1Out + p2Out + wavOut + noiseOut))/90.f;
	float curLPout = lastLPOut[apuCurChan]+(lpVal*(curIn-lastLPOut[apuCurChan]));
	float curHPOut = hpVal*(lastHPOut[apuCurChan]+curLPout-curIn);
	//set output
	apuOutBuf[curBufPos] = soundEnabled?(-curHPOut):0;
	lastLPOut[apuCurChan] = curLPout;
	lastHPOut[apuCurChan] = curHPOut;
	curBufPos++;

	apuCurChan^=1;

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
	//printf("%i\n", *freq);
	uint16_t inFreq = *freq;
	uint16_t shiftVal = (inFreq >> sw->shift);
	//if(sw->shift > 0)
	{
		if(sw->negative)
			inFreq -= shiftVal;
		else
			inFreq += shiftVal;
	}
	if(inFreq <= 0x7FF)
	{
		if(sw->enabled && sw->shift && sw->period)
			*freq = inFreq;
	}
	else
	{
		//printf("Freq disabled\n");
		p1enable = false;
	}
}

void doSweepLogic(sweep_t *sw, uint16_t *freq)
{
	if(sw->divider == 0)
	{
		if(sw->period)
		{
			sweepUpdateFreq(sw, freq);
			//gameboy checks a SECOND time after updating...
			uint16_t inFreq = *freq;
			uint16_t shiftVal = (inFreq >> sw->shift);
			if(sw->negative)
				inFreq -= shiftVal;
			else
				inFreq += shiftVal;
			if(inFreq > 0x7FF)
			{
				//printf("Freq disabled\n");
				p1enable = false;
			}
		}
		sw->divider = sw->period;
	}
	else
		sw->divider--;
}

void apuClockA()
{
	//printf("Len clock\n");
	if(p1LengthCtr && !p1haltloop)
	{
		p1LengthCtr--;
		if(p1LengthCtr == 0)
		{
			//printf("Len ran out\n");
			p1enable = false;
		}
	}
	if(p2LengthCtr && !p2haltloop)
	{
		p2LengthCtr--;
		if(p2LengthCtr == 0)
			p2enable = false;
	}
	if(wavLengthCtr && !wavhaltloop)
	{
		wavLengthCtr--;
		if(wavLengthCtr == 0)
			wavenable = false;
	}
	if(noiseLengthCtr && !noisehaltloop)
	{
		noiseLengthCtr--;
		if(noiseLengthCtr == 0)
			noiseenable = false;
	}
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
		modePos++;
		if(modePos&1)
			apuClockA();
		if(modePos == 3 || modePos == 7)
		{
			//printf("sweep clock\n");
			if(p1LengthCtr)
				doSweepLogic(&p1Sweep, &freq1);
		}
		if(modePos >= 8)
		{
			apuClockB();
			modePos = 0;
		}
		modeCurCtr = 2048;
	}
}

void apuSet8(uint8_t reg, uint8_t val)
{
	//printf("APU set %02x %02x\n", reg, val);
	if(reg == 0x26)
	{
		soundEnabled = (val&0x80)!=0;
		if(!soundEnabled)
		{
			// FULL reset of nearly every reg
			memset(APU_IO_Reg,0,0x30);
			memset(APU_IO_Reg+0x40,0,0x10);
			memset(&p1Env,0,sizeof(envelope_t));
			memset(&p2Env,0,sizeof(envelope_t));
			memset(&noiseEnv,0,sizeof(envelope_t));
			memset(&p1Sweep,0,sizeof(sweep_t));
			p1LengthCtr = 0; p2LengthCtr = 0;
			wavLengthCtr = 0; noiseLengthCtr = 0;
			p1enable = false; p2enable = false;
			wavenable = false; noiseenable = false;
			p1dacenable = false; p2dacenable = false;
			wavdacenable = false; noisedacenable = false;
			freq1 = 0; freq2 = 0; wavFreq = 0; noiseFreq = 0;
		}
	}
	if(!soundEnabled)
		return;
	APU_IO_Reg[reg] = val;
	if(reg == 0x10)
	{
		//printf("P1 sweep %02x\n", val);
		p1Sweep.shift = val&7;
		p1Sweep.period = (val>>4)&7;
		p1Sweep.negative = ((val&0x8) != 0);
		//enabled by trigger
		if(p1Sweep.enabled)
		{
			p1Sweep.divider = 0;
			sweepUpdateFreq(&p1Sweep, &freq1);
		}
	}
	else if(reg == 0x11)
	{
		p1seq = pulseSeqs[val>>6];
		p1LengthCtr = 64-(val&0x3F);
	}
	else if(reg == 0x12)
	{
		p1Env.vol = (val>>4)&0xF;
		p1Env.modeadd = (val&8)!=0;
		p1dacenable = (p1Env.modeadd || p1Env.vol);
		if(!p1dacenable)
			p1enable = false;
		p1Env.period = val&7;
		//if(p1Env.period==0)
		//	p1Env.period=8;
		p1Env.divider = p1Env.period;
	}
	else if(reg == 0x13)
	{
		freq1 = ((freq1&~0xFF) | val);
	}
	else if(reg == 0x14)
	{
		p1haltloop = ((val&(1<<6)) == 0);
		freq1 = (freq1&0xFF) | ((val&7)<<8);
		if(val&(1<<7))
		{
			if(p1dacenable)
				p1enable = true;
			if(p1LengthCtr == 0)
				p1LengthCtr = 64;
			p1Cycle = 0;
			//trigger used to enable/disable sweep
			if(p1Sweep.period || p1Sweep.shift)
			{
			
				p1Sweep.divider = 0;
				p1Sweep.enabled = true;
			}
			else
				p1Sweep.enabled = false;
			if(p1Sweep.shift)
				sweepUpdateFreq(&p1Sweep, &freq1);
		}
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
		p2dacenable = (p2Env.modeadd || p2Env.vol);
		if(!p2dacenable)
			p2enable = false;
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
		freq2 = (freq2&0xFF) | ((val&7)<<8);
		if(val&(1<<7))
		{
			if(p2dacenable)
				p2enable = true;
			if(p2LengthCtr == 0)
				p2LengthCtr = 64;
			p2Cycle = 0;
		}
		//printf("P2 new freq %04x\n", freq2);
	}
	else if(reg == 0x1A)
	{
		wavdacenable = ((val&0x80)!=0);
		if(!wavdacenable)
			wavenable = false;
	}
	else if(reg == 0x1B)
		wavLengthCtr = 256-val;
	else if(reg == 0x1C)
	{
		//printf("wavVolShift %i\n", (val>>5)&3);
		switch((val>>5)&3)
		{
			case 0:
				wavVolShift=4;
				break;
			case 1:
				wavVolShift=0;
				break;
			case 2:
				wavVolShift=1;
				break;
			case 3:
				wavVolShift=2;
				break;
		}
	}
	else if(reg == 0x1D)
	{
		//printf("wav time low %02x\n", val);
		wavFreq = ((wavFreq&~0xFF) | val);
	}
	else if(reg == 0x1E)
	{
		wavhaltloop = ((val&(1<<6)) == 0);
		wavFreq = (wavFreq&0xFF) | ((val&7)<<8);
		if(val&(1<<7))
		{
			if(wavdacenable)
				wavenable = true;
			if(wavLengthCtr == 0)
				wavLengthCtr = 256;
			wavCycle = 0;
		}
		//printf("wav new freq %04x\n", wavFreq);
	}
	else if(reg == 0x20)
	{
		noiseLengthCtr = 64-(val&0x3F);
	}
	else if(reg == 0x21)
	{
		noiseEnv.vol = (val>>4)&0xF;
		noiseEnv.modeadd = (val&8)!=0;
		noisedacenable = (noiseEnv.modeadd || noiseEnv.vol);
		if(!noisedacenable)
			noiseenable = false;
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
			if(noisedacenable)
				noiseenable = true;
			if(noiseLengthCtr == 0)
				noiseLengthCtr = 64;
		}
	}
}

//write-only bits are always set on reads by the cpu
static const uint8_t apuReadMask[0x20] =
{
	0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF, 
	0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

uint8_t apuGet8(uint8_t reg)
{
	if(reg == 0x26)
	{
		//uint8_t intrflags = ((apu_interrupt<<6) | (dmc_interrupt<<7));
		uint8_t apuretval = soundEnabled?((p1enable) | ((p2enable)<<1) | ((wavenable)<<2) | ((noiseenable)<<3)|0xF0):0x70;
		//printf("Get 0x26 %02x\n",apuretval);
		//apu_interrupt = false;
		return apuretval;
	}
	uint8_t val;
	if(reg >= 0x10 && reg < 0x30)
		val = APU_IO_Reg[reg]|apuReadMask[reg-0x10];
	else
		val = APU_IO_Reg[reg];
	//printf("APU get %02x %02x\n", reg, val);
	return val;
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
