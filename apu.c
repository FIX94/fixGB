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
#include <string.h>
#include "apu.h"
#include "audio.h"
#include "mem.h"
#include "cpu.h"

//Upper bits of FF25 are Left
#define P1_ENABLE_LEFT (1<<4)
#define P2_ENABLE_LEFT (1<<5)
#define WAV_ENABLE_LEFT (1<<6)
#define NOISE_ENABLE_LEFT (1<<7)

//Lower bits of FF25 are Right
#define P1_ENABLE_RIGHT (1<<0)
#define P2_ENABLE_RIGHT (1<<1)
#define WAV_ENABLE_RIGHT (1<<2)
#define NOISE_ENABLE_RIGHT (1<<3)

static uint8_t APU_IO_Reg[0x50];
#if AUDIO_FLOAT
static float lpVal;
static float hpVal;
static const float volLevel[8] = {
	0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f,
};
static float *apuOutBuf;
#else
static int32_t lpVal;
static int32_t hpVal;
static int16_t *apuOutBuf;
#endif
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
static uint16_t modeCurCtr;
static uint8_t modePos;
static uint16_t p1freqCtr, p2freqCtr, wavFreqCtr, noiseFreqCtr;
static uint8_t p1Cycle, p2Cycle, wavCycle;
static bool p1haltloop, p2haltloop, wavhaltloop, noisehaltloop;
static bool p1dacenable, p2dacenable, wavdacenable, noisedacenable;
static bool p1enable, p2enable, wavenable, noiseenable;
static bool soundEnabled;
static bool noiseMode1;
static bool wavEqual;

static envelope_t p1Env, p2Env, noiseEnv;

typedef struct _sweep_t {
	bool enabled;
	bool negative;
	bool inNegative;
	uint8_t period;
	uint8_t divider;
	uint8_t shift;
	uint16_t pfreq;
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

//wav values from http://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware#Power_Control
static const uint8_t startWavSetDMG[0x10] = {
	0x84, 0x40, 0x43, 0xAA, 0x2D, 0x78, 0x92, 0x3C,
	0x60, 0x59, 0x59, 0xB0, 0x34, 0xB8, 0x2E, 0xDA,
};

static const uint8_t startWavSetCGB[0x10] = {
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
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
	//apuFrequency = 526680;
	//effective frequency for original LCD Video out
	apuFrequency = 262144;
	double dt = 1.0/((double)apuFrequency);

	//LP at 20kHz
	double rc = 1.0/(M_2_PI * 20000.0);
#if AUDIO_FLOAT
	lpVal = dt / (rc + dt);
#else
	//convert to 32bit int for calcs later
	lpVal = (int32_t)((dt / (rc + dt))*32768.0);
#endif
	//HP at 20Hz for GB/GBA (150Hz for GBC)
	rc = 1.0/(M_2_PI * 20.0);
#if AUDIO_FLOAT
	hpVal = rc / (rc + dt);
#else
	//convert to 32bit int for calcs later
	hpVal = (int32_t)((rc / (rc + dt))*32768.0);
#endif
	//keep exactly 1 frame buffer
	apuBufSize = 70224/16*2;
#if AUDIO_FLOAT
	apuBufSizeBytes = apuBufSize*sizeof(float);
	apuOutBuf = (float*)malloc(apuBufSizeBytes);
	printf("Audio: 32-bit Float Output\n"); 
#else
	apuBufSizeBytes = apuBufSize*sizeof(int16_t);
	apuOutBuf = (int16_t*)malloc(apuBufSizeBytes);
	printf("Audio: 16-bit Short Output\n");
#endif
}

void apuDeinitBufs()
{
	if(apuOutBuf)
		free(apuOutBuf);
	apuOutBuf = NULL;
}

#if AUDIO_FLOAT
static float lastHPOutLeft, lastHPOutRight, lastLPOutLeft, lastLPOutRight;
#else
static int32_t lastHPOutLeft, lastHPOutRight, lastLPOutLeft, lastLPOutRight;
#endif
//used externally
uint8_t curP1Out, curP2Out, curWavOut, curNoiseOut;

extern bool emuSkipVsync, emuSkipFrame;

extern bool gbCgbMode;
extern bool gbCgbBootrom;
void apuInit()
{
	memset(APU_IO_Reg,0,0x50);
	if(gbCgbMode) //essentially 50% duty pulse on CGB
		memcpy(APU_IO_Reg+0x30,startWavSetCGB,0x10);
	else //relatively random audio pattern on DMG
		memcpy(APU_IO_Reg+0x30,startWavSetDMG,0x10);
	memset(apuOutBuf, 0, apuBufSizeBytes);
	curBufPos = 0;

	modeCurCtr = 0;
	modePos = 0;

	freq1 = 0; freq2 = 0; wavFreq = 0; noiseFreq = 0;
	noiseShiftReg = 0;
	p1LengthCtr = 0; p2LengthCtr = 0;
	noiseLengthCtr = 0;	wavLengthCtr = 0;
	wavLinearCtr = 0;
	p1freqCtr = 0; p2freqCtr = 0; wavFreqCtr = 0, noiseFreqCtr = 0;
	p1Cycle = 0; p2Cycle = 0; wavCycle = 0;
	wavVolShift = 4; //default

	memset(&p1Env,0,sizeof(envelope_t));
	memset(&p2Env,0,sizeof(envelope_t));
	memset(&noiseEnv,0,sizeof(envelope_t));

	memset(&p1Sweep,0,sizeof(sweep_t));

	p1haltloop = false; p2haltloop = false;
	wavhaltloop = false; noisehaltloop = false;
	p1enable = false; p2enable = false;
	wavenable = false; noiseenable = false;
	p1dacenable = false; p2dacenable = false;
	wavdacenable = false; noisedacenable = false;
	noiseMode1 = false;
	wavEqual = false;
	if(gbCgbBootrom)
		soundEnabled = false;
	else //GB Bootrom
	{
		soundEnabled = true;
		APU_IO_Reg[0x24] = 0x77;
		APU_IO_Reg[0x25] = 0xF3;
	}

	lastHPOutLeft = 0, lastHPOutRight = 0, lastLPOutLeft = 0, lastLPOutRight = 0;
	curP1Out = 0, curP2Out = 0, curWavOut = 0, curNoiseOut = 0;
}

bool apuCycle()
{
	if(curBufPos == apuBufSize)
	{
#ifndef __LIBRETRO__
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
#endif
		curBufPos = 0;
	}
	int8_t p1Out = 0, p2Out = 0, noiseOut = 0, wavOut = 0;
	int8_t p1OutLeft = 0, p2OutLeft = 0, 
		wavOutLeft = 0, noiseOutLeft = 0;
	int8_t p1OutRight = 0, p2OutRight = 0, 
		wavOutRight = 0, noiseOutRight = 0;
	int8_t apuMasterVolLeft = ((APU_IO_Reg[0x24]>>4)&7), apuMasterVolRight = (APU_IO_Reg[0x24]&7);
	if(p1enable && p1dacenable)
	{
		if(p1seq[p1Cycle])
			curP1Out = p1Env.curVol;
		else
			curP1Out = 0;
		//actually audible output
		if(freq1 > 0 && freq1 < 0x7FF)
		{
			//GB/GBC Behavior
			//p1Out = (curP1Out<<1)-15;
			//GBA Behavior
			p1Out = (curP1Out<<1)-p1Env.curVol;
		}
	}
	else
		curP1Out = 0;
	if(APU_IO_Reg[0x25] & P1_ENABLE_LEFT)
		p1OutLeft = p1Out;
	if(APU_IO_Reg[0x25] & P1_ENABLE_RIGHT)
		p1OutRight = p1Out;
	if(p2enable && p2dacenable)
	{
		if(p2seq[p2Cycle])
			curP2Out = p2Env.curVol;
		else
			curP2Out = 0;
		//actually audible output
		if(freq2 > 0 && freq2 < 0x7FF)
		{
			//GB/GBC Behavior
			//p2Out = (curP2Out<<1)-15;
			//GBA Behavior
			p2Out = (curP2Out<<1)-p2Env.curVol;
		}
	}
	else
		curP2Out = 0;
	if(APU_IO_Reg[0x25] & P2_ENABLE_LEFT)
		p2OutLeft = p2Out;
	if(APU_IO_Reg[0x25] & P2_ENABLE_RIGHT)
		p2OutRight = p2Out;
	if(wavenable && wavdacenable)
	{
		curWavOut = APU_IO_Reg[0x30+(wavCycle>>1)];
		if((wavCycle&1)==0)
			curWavOut >>= 4; 
		else
			curWavOut &= 0xF;
		curWavOut >>= wavVolShift;
		//actually audible output
		if((wavFreq > 0 && wavFreq < 0x7FF) || wavEqual)
			wavOut = (curWavOut<<1)-15;
	}
	else
		curWavOut = 0;
	if(APU_IO_Reg[0x25] & WAV_ENABLE_LEFT)
		wavOutLeft = wavOut;
	if(APU_IO_Reg[0x25] & WAV_ENABLE_RIGHT)
		wavOutRight = wavOut;
	if(noiseenable && noisedacenable)
	{
		if((noiseShiftReg&1) == 0)
			curNoiseOut = noiseEnv.curVol;
		else
			curNoiseOut = 0;
		//actually audible output
		if(noiseFreq > 0)
		{
			//GB/GBC Behavior
			//noiseOut = (curNoiseOut<<1)-15;
			//GBA Behavior
			noiseOut = (curNoiseOut<<1)-noiseEnv.curVol;
		}
	}
	else
		curNoiseOut = 0;
	if(APU_IO_Reg[0x25] & NOISE_ENABLE_LEFT)
		noiseOutLeft = noiseOut;
	if(APU_IO_Reg[0x25] & NOISE_ENABLE_RIGHT)
		noiseOutRight = noiseOut;
#if AUDIO_FLOAT
	//gen output Left
	float curInLeft = ((float)(p1OutLeft + p2OutLeft + wavOutLeft + noiseOutLeft))*volLevel[apuMasterVolLeft]/85.333333f;
	float curLPOutLeft = lastLPOutLeft+(lpVal*(curInLeft-lastLPOutLeft));
	float curHPOutLeft = hpVal*(lastHPOutLeft+lastLPOutLeft-curLPOutLeft);
	//gen output Right
	float curInRight = ((float)(p1OutRight + p2OutRight + wavOutRight + noiseOutRight))*volLevel[apuMasterVolRight]/85.333333f;
	float curLPOutRight = lastLPOutRight+(lpVal*(curInRight-lastLPOutRight));
	float curHPOutRight = hpVal*(lastHPOutRight+lastLPOutRight-curLPOutRight);
	//set output Left
	apuOutBuf[curBufPos++] = ((soundEnabled)?curHPOutLeft:0);
	//set output Right
	apuOutBuf[curBufPos++] = ((soundEnabled)?curHPOutRight:0);
	//save HP and LP Left
	lastLPOutLeft = curLPOutLeft;
	lastHPOutLeft = curHPOutLeft;
	//save HP and LP Right
	lastLPOutRight = curLPOutRight;
	lastHPOutRight = curHPOutRight;
#else
	int32_t curIn, curOut;
	//gen output Left
	curIn = (p1OutLeft + p2OutLeft + wavOutLeft + noiseOutLeft)*(apuMasterVolLeft+1)*48;
	curOut = lastLPOutLeft+((lpVal*(curIn-lastLPOutLeft))>>15); //Set Left Lowpass Output
	curIn = (lastHPOutLeft+lastLPOutLeft-curOut); //Set Left Highpass Input
	curIn += (curIn>>31)&1; //Add Sign Bit for proper Downshift later
	lastLPOutLeft = curOut; //Save Left Lowpass Output
	curOut = (hpVal*curIn)>>15; //Set Left Highpass Output
	lastHPOutLeft = curOut; //Save Left Highpass Output
	//Save Clipped Left Highpass Output
	apuOutBuf[curBufPos++] = ((soundEnabled)?((curOut > 32767)?(32767):((curOut < -32768)?(-32768):curOut)):0);
	//gen output Right
	curIn = (p1OutRight + p2OutRight + wavOutRight + noiseOutRight)*(apuMasterVolRight+1)*48;
	curOut = lastLPOutRight+((lpVal*(curIn-lastLPOutRight))>>15); //Set Right Lowpass Output
	curIn = (lastHPOutRight+lastLPOutRight-curOut); //Set Right Highpass Input
	curIn += (curIn>>31)&1; //Add Sign Bit for proper Downshift later
	lastLPOutRight = curOut; //Save Right Lowpass Output
	curOut = (hpVal*curIn)>>15; //Set Right Highpass Output
	lastHPOutRight = curOut; //Save Right Highpass Output
	//Save Clipped Right Highpass Output
	apuOutBuf[curBufPos++] = ((soundEnabled)?((curOut > 32767)?(32767):((curOut < -32768)?(-32768):curOut)):0);
#endif
	return true;
}

#ifdef __LIBRETRO__
void audioFrameEnd(int samples);
void apuFrameEnd()
{
	audioFrameEnd(curBufPos>>1);
	curBufPos = 0;
}
#endif

void doEnvelopeLogic(envelope_t *env)
{
	if(env->divider == 0)
	{
		if(env->period)
		{
			if(env->modeadd)
			{
				if(env->curVol < 15)
					env->curVol++;
			}
			else
			{
				if(env->curVol > 0)
					env->curVol--;
			}
		}
		//period 0 is actually period 8!
		env->divider = (env->period-1)&7;
	}
	else
		env->divider--;
}

void sweepUpdateFreq(sweep_t *sw, uint16_t *freq, bool update)
{
	if(!sw->enabled)
		return;
	//printf("%i\n", *freq);
	uint16_t inFreq = sw->pfreq;
	uint16_t shiftVal = (inFreq >> sw->shift);

	if(sw->negative)
	{
		sw->inNegative = true;
		inFreq -= shiftVal;
	}
	else
		inFreq += shiftVal;

	if(inFreq <= 0x7FF)
	{
		if(sw->enabled && sw->shift && sw->period && update)
		{
			*freq = inFreq;
			sw->pfreq = inFreq;
		}
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
		//printf("Divider 0\n");
		if(sw->period)
		{
			sweepUpdateFreq(sw, freq, true);
			//gameboy checks a SECOND time after updating...
			uint16_t inFreq = sw->pfreq;
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
		//period 0 is actually period 8!
		sw->divider = (sw->period-1)&7;
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
			p1enable = false;
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

void apuClockTimers()
{
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
		modeCurCtr = 8192;
	}
	if(modeCurCtr)
		modeCurCtr--;

	if(p1freqCtr == 0)
	{
		if(freq1)
			p1freqCtr = (2048-freq1)*4;
		p1Cycle++;
		if(p1Cycle >= 8)
			p1Cycle = 0;
	}
	if(p1freqCtr)
		p1freqCtr--;

	if(p2freqCtr == 0)
	{
		if(freq2)
			p2freqCtr = (2048-freq2)*4;
		p2Cycle++;
		if(p2Cycle >= 8)
			p2Cycle = 0;
	}
	if(p2freqCtr)
		p2freqCtr--;

	if(wavFreqCtr == 0)
	{
		wavFreqCtr = (2048-wavFreq)*2;
		wavCycle++;
		if(wavCycle >= 32)
			wavCycle = 0;
	}
	if(wavFreqCtr)
		wavFreqCtr--;

	if(noiseFreqCtr == 0)
	{
		noiseFreqCtr = noiseFreq;
		uint8_t cmpRes = (noiseShiftReg&1)^((noiseShiftReg>>1)&1);
		noiseShiftReg >>= 1;
		noiseShiftReg |= cmpRes << (noiseMode1 ? 6 : 14);
	}
	if(noiseFreqCtr)
		noiseFreqCtr--;
}

void apuSetReg8(uint16_t addr, uint8_t val)
{
	uint8_t reg = addr&0xFF;
	//printf("APU set %02x %02x\n", reg, val);
	if(reg == 0x26)
	{
		bool wasEnabled = soundEnabled;
		soundEnabled = (val&0x80)!=0;
		if(!soundEnabled)
		{
			// FULL reset of nearly every reg
			memset(APU_IO_Reg,0,0x30);
			// except for the wav buffer
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
			wavVolShift = 4; //default
		}
		else
		{
			APU_IO_Reg[0x26] = val;
			//on sound powerup, reset frame sequencer
			if(!wasEnabled)
			{
				modeCurCtr = 8192;
				modePos = 0;
			}
		}
		return;
	}
	//even if sound off, still update wav buffer
	else if(reg >= 0x30 && reg < 0x40)
	{
		if(wavenable)
			APU_IO_Reg[0x30+(wavCycle>>1)] = val;
		else
			APU_IO_Reg[reg] = val;
		//allow for manual wav inputs if all wav inputs are equal
		wavEqual = ((*(uint32_t*)(APU_IO_Reg+0x30) == *(uint32_t*)(APU_IO_Reg+0x34)) &&
					(*(uint32_t*)(APU_IO_Reg+0x34) == *(uint32_t*)(APU_IO_Reg+0x38)) &&
					(*(uint32_t*)(APU_IO_Reg+0x38) == *(uint32_t*)(APU_IO_Reg+0x3C)) );
		return;
	}
	//dont even bother with the switch if sound is off
	else if(!soundEnabled)
		return;
	bool p1prevhaltloop, p2prevhaltloop,
		wavprevhaltloop, noiseprevhaltloop;
	APU_IO_Reg[reg] = val;
	switch(reg)
	{
		case 0x10:
			//printf("P1 sweep %02x\n", val);
			p1Sweep.shift = val&7;
			p1Sweep.period = (val>>4)&7;
			p1Sweep.negative = ((val&0x8) != 0);
			if(p1Sweep.inNegative && !p1Sweep.negative)
				p1enable = false;
			break;
		case 0x11:
			p1seq = pulseSeqs[val>>6];
			p1LengthCtr = 64-(val&0x3F);
			break;
		case 0x12:
			p1Env.vol = (val>>4)&0xF;
			p1Env.modeadd = (val&8)!=0;
			if(p1Env.modeadd && p1Env.period == 0 && (val&7) == 0)
			{
				//"Zombie" Mode
				p1Env.curVol++;
				p1Env.curVol &= 0xF;
			}
			p1dacenable = (p1Env.modeadd || p1Env.vol);
			if(!p1dacenable)
				p1enable = false;
			p1Env.period = val&7;
			break;
		case 0x13:
			freq1 = ((freq1&~0xFF) | val);
			//printf("P1 new freq %04x\n", freq1);
			break;
		case 0x14:
			p1prevhaltloop = p1haltloop;
			p1haltloop = ((val&(1<<6)) == 0);
			freq1 = (freq1&0xFF) | ((val&7)<<8);
			//if length was previously frozen and we are in
			//an odd frame sequence, clock length right now
			if(p1prevhaltloop && !p1haltloop && p1LengthCtr && (modePos&1))
			{
				p1LengthCtr--;
				//disable channel immediately if length
				//reached 0 from this extra clock
				if(p1LengthCtr == 0)
					p1enable = false;
			}
			if(val&(1<<7))
			{
				if(p1dacenable)
					p1enable = true;
				if(p1LengthCtr == 0)
				{
					p1LengthCtr = 64;
					//if length enabled and we are in an odd frame
					//sequence, subtract one from newly set clock length
					if(!p1haltloop && (modePos&1))
						p1LengthCtr--;
				}
				//trigger reloads frequency timers
				p1Cycle = 0;
				if(freq1)
					p1freqCtr = (2048-freq1)*4;
				//trigger resets env volume
				p1Env.curVol = p1Env.vol;
				//period 0 is actually period 8!
				p1Env.divider = (p1Env.period-1)&7;
				//trigger used to enable/disable sweep
				if(p1Sweep.period || p1Sweep.shift)
					p1Sweep.enabled = true;
				else
					p1Sweep.enabled = false;
				//trigger also resets divider, neg mode and frequency
				p1Sweep.inNegative = false;
				p1Sweep.pfreq = freq1;
				//period 0 is actually period 8!
				p1Sweep.divider = (p1Sweep.period-1)&7;
				//if sweep shift>0, pre-calc frequency
				if(p1Sweep.shift)
					sweepUpdateFreq(&p1Sweep, &freq1, false);
			}
			//printf("P1 new freq %04x\n", freq1);
			break;
		case 0x16:
			p2seq = pulseSeqs[val>>6];
			p2LengthCtr = 64-(val&0x3F);
			break;
		case 0x17:
			p2Env.vol = (val>>4)&0xF;
			p2Env.modeadd = (val&8)!=0;
			if(p2Env.modeadd && p2Env.period == 0 && (val&7) == 0)
			{
				//"Zombie" Mode
				p2Env.curVol++;
				p2Env.curVol &= 0xF;
			}
			p2dacenable = (p2Env.modeadd || p2Env.vol);
			if(!p2dacenable)
				p2enable = false;
			p2Env.period = val&7;
			break;
		case 0x18:
			freq2 = ((freq2&~0xFF) | val);
			//printf("P2 new freq %04x\n", freq2);
			break;
		case 0x19:
			p2prevhaltloop = p2haltloop;
			p2haltloop = ((val&(1<<6)) == 0);
			freq2 = (freq2&0xFF) | ((val&7)<<8);
			//if length was previously frozen and we are in
			//an odd frame sequence, clock length right now
			if(p2prevhaltloop && !p2haltloop && p2LengthCtr && (modePos&1))
			{
				p2LengthCtr--;
				//disable channel immediately if length
				//reached 0 from this extra clock
				if(p2LengthCtr == 0)
					p2enable = false;
			}
			if(val&(1<<7))
			{
				if(p2dacenable)
					p2enable = true;
				if(p2LengthCtr == 0)
				{
					p2LengthCtr = 64;
					//if length enabled and we are in an odd frame
					//sequence, subtract one from newly set clock length
					if(!p2haltloop && (modePos&1))
						p2LengthCtr--;
				}
				//trigger reloads frequency timers
				p2Cycle = 0;
				if(freq2)
					p2freqCtr = (2048-freq2)*4;
				//trigger resets env volume
				p2Env.curVol = p2Env.vol;
				//period 0 is actually period 8!
				p2Env.divider = (p2Env.period-1)&7;
			}
			//printf("P2 new freq %04x\n", freq2);
			break;
		case 0x1A:
			wavdacenable = ((val&0x80)!=0);
			if(!wavdacenable)
				wavenable = false;
			break;
		case 0x1B:
			wavLengthCtr = 256-val;
			break;
		case 0x1C:
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
			break;
		case 0x1D:
			wavFreq = ((wavFreq&~0xFF) | val);
			//printf("wav new freq %04x\n", wavFreq);
			break;
		case 0x1E:
			wavprevhaltloop = wavhaltloop;
			wavhaltloop = ((val&(1<<6)) == 0);
			wavFreq = (wavFreq&0xFF) | ((val&7)<<8);
			//if length was previously frozen and we are in
			//an odd frame sequence, clock length right now
			if(wavprevhaltloop && !wavhaltloop && wavLengthCtr && (modePos&1))
			{
				wavLengthCtr--;
				//disable channel immediately if length
				//reached 0 from this extra clock
				if(wavLengthCtr == 0)
					wavenable = false;
			}
			if(val&(1<<7))
			{
				if(wavdacenable)
					wavenable = true;
				if(wavLengthCtr == 0)
				{
					wavLengthCtr = 256;
					//if length enabled and we are in an odd frame
					//sequence, subtract one from newly set clock length
					if(!wavhaltloop && (modePos&1))
						wavLengthCtr--;
				}
				//trigger reloads frequency timers
				wavCycle = 0;
				//not sure why +4 needed to sync initally,
				//probably because of sample buffer byte
				wavFreqCtr = ((2048-wavFreq)*2)+4;
			}
			//printf("wav new freq %04x\n", wavFreq);
			break;
		case 0x20:
			noiseLengthCtr = 64-(val&0x3F);
			break;
		case 0x21:
			noiseEnv.vol = (val>>4)&0xF;
			noiseEnv.modeadd = (val&8)!=0;
			if(noiseEnv.modeadd && noiseEnv.period == 0 && (val&7) == 0)
			{
				//"Zombie" Mode
				noiseEnv.curVol++;
				noiseEnv.curVol &= 0xF;
			}
			noisedacenable = (noiseEnv.modeadd || noiseEnv.vol);
			if(!noisedacenable)
				noiseenable = false;
			noiseEnv.period=val&7;
			break;
		case 0x22:
			if((val>>4)<14)
				noiseFreq = noisePeriod[val&0x7]<<(val>>4);
			else
				noiseFreq = 0;
			noiseMode1 = ((val&0x8) != 0);
			break;
		case 0x23:
			noiseprevhaltloop = noisehaltloop;
			noisehaltloop = ((val&(1<<6)) == 0);
			//if length was previously frozen and we are in
			//an odd frame sequence, clock length right now
			if(noiseprevhaltloop && !noisehaltloop && noiseLengthCtr && (modePos&1))
			{
				noiseLengthCtr--;
				//disable channel immediately if length
				//reached 0 from this extra clock
				if(noiseLengthCtr == 0)
					noiseenable = false;
			}
			if(val&(1<<7))
			{
				if(noisedacenable)
					noiseenable = true;
				if(noiseLengthCtr == 0)
				{
					noiseLengthCtr = 64;
					//if length enabled and we are in an odd frame
					//sequence, subtract one from newly set clock length
					if(!noisehaltloop && (modePos&1))
						noiseLengthCtr--;
				}
				//trigger reloads frequency timers
				noiseFreqCtr = noiseFreq;
				//trigger resets env volume
				noiseEnv.curVol = noiseEnv.vol;
				//period 0 is actually period 8!
				noiseEnv.divider = (noiseEnv.period-1)&7;
				//trigger sets all shift reg bits
				noiseShiftReg = 0x7FFF;
			}
			break;
		default:
			break;
	}
}

//write-only bits are always set on reads by the cpu
static const uint8_t apuReadMask[0x20] =
{
	0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF, 
	0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

uint8_t apuGetReg8(uint16_t addr)
{
	uint8_t reg = addr&0xFF;
	//printf("APU get %02x\n", reg);
	switch(reg)
	{
		case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: /*case 0x26:*/ case 0x27:
		case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			return APU_IO_Reg[reg]|apuReadMask[reg-0x10];
		case 0x26:
			return soundEnabled?((p1enable) | ((p2enable)<<1) | ((wavenable)<<2) | ((noiseenable)<<3)|0xF0):0x70;
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
		case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
			if(wavenable)
				return APU_IO_Reg[0x30+(wavCycle>>1)];
			return APU_IO_Reg[reg];
		default:
			break;
	}
	return 0xFF;
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
