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
#include "apu.h"
#include "ppu.h"
#include "mem.h"
#include "input.h"

#define P_FLAG_C (1<<4)
#define P_FLAG_H (1<<5)
#define P_FLAG_N (1<<6)
#define P_FLAG_Z (1<<7)

//from main.c
extern bool gbEmuGBSPlayback;
extern uint16_t gbsLoadAddr;
extern uint16_t gbsInitAddr;
extern uint16_t gbsPlayAddr;
extern uint16_t gbsSP;
extern uint8_t cpuTimer;
extern bool allowCgbRegs;

//used externally
bool cpuDoStopSwitch = false;
bool cpuCgbSpeed = false;
void cpuSetupActionArr();

static uint16_t sp, pc, cpuTmp16;
static uint8_t a,b,c,d,e,f,h,l,cpuTmp;

//gbs stuff
static bool gbsInitRet, gbsPlayRet;

static uint8_t sub_in_val;
static bool irqEnable;
static bool cpuHaltLoop,cpuStopLoop,cpuHaltBug;
void cpuInit()
{
	sub_in_val=0,cpuTmp=0,cpuTmp16=0;
	if(allowCgbRegs) //From GBC Bootrom
		a=0x11,b=0,c=0,d=0,e=0x08,f=0x80,h=0,l=0x7C;
	else //From GB Bootrom
		a=0x01,b=0,c=0x13,d=0,e=0xD8,f=0xB0,h=1,l=0x4D;
	sp = 0xFFFE; //Boot Stack Pointer
	pc = 0x0100; //hardcoded ROM entrypoint
	irqEnable = false;
	cpuHaltLoop = false;
	cpuStopLoop = false;
	cpuHaltBug = false;
	cpuCgbSpeed = false;
	cpuSetupActionArr();
	//gbs stuff
	gbsInitRet = false; //for first init
	gbsPlayRet = true; //for first play call
}

static void setAImmRegStats()
{
	f &= ~(P_FLAG_C|P_FLAG_H|P_FLAG_N|P_FLAG_Z);
	if(a == 0) f |= P_FLAG_Z;
}

void cpuAdd16(uint16_t x)
{
	uint32_t r1,r2;
	r1=((h<<8)|l)+(x);
	r2=(((h<<8)|l)&0xFFF)+((x)&0xFFF);

	uint8_t tf = (uint8_t)(f&P_FLAG_Z);
	if(r1 > 0xFFFF)
		tf |= P_FLAG_C;
	if(r2 > 0x0FFF)
		tf |= P_FLAG_H;
	l = r1;
	h = r1 >> 8;
	f = tf;
}

uint16_t cpuAddSp16(uint8_t add)
{
	int32_t n = (int8_t)add;
	uint32_t r1,r2,r3;
	r1=sp+n;
	r2=(sp&0xFF)+(n&0xFF);
	r3=(sp&0xF)+(n&0xF);

	uint8_t tf = 0;

	if(r2 > 0xFF)
		tf |= P_FLAG_C;
	if(r3 > 0x0F)
		tf |= P_FLAG_H;

	f = tf;

	return (uint16_t)r1;
}

void setAddSubCmpFlags(uint16_t r1, uint16_t r2)
{
	if(((uint8_t)r2)==0)
		f |= P_FLAG_Z;
	else
		f &= ~P_FLAG_Z;

	if(r2>0xFF)
		f |= P_FLAG_C;
	else
		f &= ~P_FLAG_C;

	if(r1>0xF)
		f |= P_FLAG_H;
	else
		f &= ~P_FLAG_H;
}

uint8_t cpuDoAdd8(uint8_t x, uint8_t y)
{
	uint16_t r1,r2;
	r1=(uint16_t)((x&0xF)+(y&0xF));
	r2=(uint16_t)(x+y);

	setAddSubCmpFlags(r1, r2);

	f &= ~P_FLAG_N;

	return (uint8_t)r2;
}

void cpuAdd8(uint8_t *reg)
{
	a = cpuDoAdd8(a,*reg);
}

uint8_t cpuDoSub8(uint8_t x, uint8_t y)
{
	uint16_t r1,r2;
	r1=(uint16_t)((x&0xF)-(y&0xF));
	r2=(uint16_t)(x-y);

	setAddSubCmpFlags(r1, r2);

	f |= P_FLAG_N;

	return (uint8_t)r2;
}

void cpuSub8(uint8_t *reg)
{
	a = cpuDoSub8(a,*reg);
}

void cpuCmp8(uint8_t *reg)
{
	cpuDoSub8(a,*reg);
}

void cpuSbc8(uint8_t *reg)
{
	uint16_t r1,r2;
	uint8_t x = *reg;
	r1=(uint16_t)((a&0xF)-((x)&0xF)-((f&P_FLAG_C)?1:0));
	r2=(uint16_t)(a-(x)-((f&P_FLAG_C)?1:0));

	a=(uint8_t)r2;

	setAddSubCmpFlags(r1, r2);

	f |= P_FLAG_N;
}

void cpuAdc8(uint8_t *reg)
{
	uint16_t r1,r2;
	uint8_t x = *reg;
	r1=(uint16_t)((a&0xF)+((x)&0xF)+((f&P_FLAG_C)?1:0));
	r2=(uint16_t)(a+(x)+((f&P_FLAG_C)?1:0));

	a=(uint8_t)r2;

	setAddSubCmpFlags(r1, r2);

	f &= ~P_FLAG_N;
}

static void cpuXOR(uint8_t *reg)
{
	a ^= (*reg);
	setAImmRegStats();
}

static void cpuOR(uint8_t *reg)
{
	a |= (*reg);
	setAImmRegStats();
}

static void cpuAND(uint8_t *reg)
{
	a &= (*reg);
	setAImmRegStats();
	f |= P_FLAG_H;
}

static void cpuCPL(uint8_t *reg)
{
	uint8_t val = (*reg);
	f |= (P_FLAG_H|P_FLAG_N);
	*reg = ~val;
}

//rotate WITHOUT old carry used
static void cpuRLC(uint8_t *reg)
{
	uint8_t val = (*reg);

	f &= ~(P_FLAG_H|P_FLAG_N);

	if(val&0x80)
		f |= P_FLAG_C;
	else
		f &= ~P_FLAG_C;

	val <<= 1;

	if(f & P_FLAG_C)
		val |= 1;

	if(val == 0)
		f |= P_FLAG_Z;
	else
		f &= ~P_FLAG_Z;

	*reg = val;
}

//rotate WITHOUT old carry used
static void cpuRRC(uint8_t *reg)
{
	uint8_t val = (*reg);

	f &= ~(P_FLAG_H|P_FLAG_N);

	if(val&1)
		f |= P_FLAG_C;
	else
		f &= ~P_FLAG_C;

	val >>= 1;

	if(f & P_FLAG_C)
		val |= 0x80;

	if(val == 0)
		f |= P_FLAG_Z;
	else
		f &= ~P_FLAG_Z;

	*reg = val;
}

//rotate WITH old carry used
static void cpuRL(uint8_t *reg)
{
	uint8_t val = (*reg);
	uint8_t oldF = f;

	f &= ~(P_FLAG_H|P_FLAG_N);

	if(val&0x80)
		f |= P_FLAG_C;
	else
		f &= ~P_FLAG_C;

	val <<= 1;

	if(oldF & P_FLAG_C)
		val |= 1;

	if(val == 0)
		f |= P_FLAG_Z;
	else
		f &= ~P_FLAG_Z;

	*reg = val;
}

//rotate WITH old carry used
static void cpuRR(uint8_t *reg)
{
	uint8_t val = (*reg);
	uint8_t oldF = f;

	f &= ~(P_FLAG_H|P_FLAG_N);

	if(val&1)
		f |= P_FLAG_C;
	else
		f &= ~P_FLAG_C;

	val >>= 1;

	if(oldF & P_FLAG_C)
		val |= 0x80;

	if(val == 0)
		f |= P_FLAG_Z;
	else
		f &= ~P_FLAG_Z;

	*reg = val;
}

//Direct calls for a
static void cpuRLCA()
{
	cpuRLC(&a);
	f &= ~P_FLAG_Z;
}

static void cpuRRCA()
{
	cpuRRC(&a);
	f &= ~P_FLAG_Z;
}

static void cpuRLA()
{
	cpuRL(&a);
	f &= ~P_FLAG_Z;
}

static void cpuRRA()
{
	cpuRR(&a);
	f &= ~P_FLAG_Z;
}

//shift left
static void cpuSLA(uint8_t *reg)
{
	uint8_t val = (*reg);

	f &= ~(P_FLAG_H|P_FLAG_N);

	if(val&0x80)
		f |= P_FLAG_C;
	else
		f &= ~P_FLAG_C;

	val <<= 1;

	if(val == 0)
		f |= P_FLAG_Z;
	else
		f &= ~P_FLAG_Z;

	*reg = val;
}

//shift right arithmetic
static void cpuSRA(uint8_t *reg)
{
	uint8_t val = (*reg);
	uint8_t oldSign = (val&0x80);

	f &= ~(P_FLAG_H|P_FLAG_N);

	if(val&1)
		f |= P_FLAG_C;
	else
		f &= ~P_FLAG_C;

	val >>= 1;
	val |= oldSign;

	if(val == 0)
		f |= P_FLAG_Z;
	else
		f &= ~P_FLAG_Z;

	*reg = val;
}

//shift right logical
static void cpuSRL(uint8_t *reg)
{
	uint8_t val = (*reg);

	f &= ~(P_FLAG_H|P_FLAG_N);

	if(val&1)
		f |= P_FLAG_C;
	else
		f &= ~P_FLAG_C;

	val >>= 1;

	if(val == 0)
		f |= P_FLAG_Z;
	else
		f &= ~P_FLAG_Z;

	*reg = val;
}

static void cpuSWAP(uint8_t *reg)
{
	uint8_t val = (*reg);

	f &= ~(P_FLAG_H|P_FLAG_N|P_FLAG_C);

	uint8_t vL = val&0xF;
	val>>=4;
	val|=vL<<4;

	if(val == 0)
		f |= P_FLAG_Z;
	else
		f &= ~P_FLAG_Z;

	*reg = val;
}

static void cpuBIT(uint8_t *reg)
{
	uint8_t val = (*reg);

	f &= ~P_FLAG_N;
	f |= P_FLAG_H;

	if((val&(1<<sub_in_val)) == 0)
		f |= P_FLAG_Z;
	else
		f &= ~P_FLAG_Z;
}

static void cpuSET(uint8_t *reg)
{
	uint8_t val = (*reg);

	val |= (1<<sub_in_val);

	*reg = val;
}

static void cpuRES(uint8_t *reg)
{
	uint8_t val = (*reg);

	val &= ~(1<<sub_in_val);

	*reg = val;
}

static void cpuHALT(uint8_t *none)
{
	(void)none;
	//HALT bug, PC wont increase next instruction!
	if(!irqEnable && memGetCurIrqList())
		cpuHaltBug = true;
	else //enters HALT mode normall
		cpuHaltLoop = true;
}

static void cpuBcInc()
{
	uint16_t tmp = c | b<<8;
	tmp++;
	c = tmp&0xFF;
	b = tmp>>8;
}

static void cpuDeInc()
{
	uint16_t tmp = e | d<<8;
	tmp++;
	e = tmp&0xFF;
	d = tmp>>8;
}

static void cpuHlInc()
{
	uint16_t tmp = l | h<<8;
	tmp++;
	l = tmp&0xFF;
	h = tmp>>8;
}

static void cpuSpInc()
{
	sp++;
}

static void cpuBcDec()
{
	uint16_t tmp = c | b<<8;
	tmp--;
	c = tmp&0xFF;
	b = tmp>>8;
}

static void cpuDeDec()
{
	uint16_t tmp = e | d<<8;
	tmp--;
	e = tmp&0xFF;
	d = tmp>>8;
}

static void cpuHlDec()
{
	uint16_t tmp = l | h<<8;
	tmp--;
	l = tmp&0xFF;
	h = tmp>>8;
}

static void cpuSpDec()
{
	sp--;
}

static void cpuNoAction(uint8_t *reg) { (void)reg; };

static void cpuLDa(uint8_t *reg) { a = (*reg); }
static void cpuLDb(uint8_t *reg) { b = (*reg); }
static void cpuLDc(uint8_t *reg) { c = (*reg); }
static void cpuLDd(uint8_t *reg) { d = (*reg); }
static void cpuLDe(uint8_t *reg) { e = (*reg); }
static void cpuLDl(uint8_t *reg) { l = (*reg); }
static void cpuLDh(uint8_t *reg) { h = (*reg); }

static void cpuSTbc(uint8_t *reg) { memSet8(c | b<<8, (*reg)); }
static void cpuSTde(uint8_t *reg) { memSet8(e | d<<8, (*reg)); }
static void cpuSThl(uint8_t *reg) { memSet8(l | h<<8, (*reg)); }
static void cpuSTt16(uint8_t *reg) { memSet8(cpuTmp16, (*reg)); }

static void cpuSThlInc(uint8_t *reg) { cpuSThl(reg); cpuHlInc(); }
static void cpuSThlDec(uint8_t *reg) { cpuSThl(reg); cpuHlDec(); }

static void cpuInc(uint8_t *reg)
{
	uint8_t tf = f&P_FLAG_C;
	*reg = cpuDoAdd8(*reg,1);
	f&=~P_FLAG_C;
	f|=tf;
}

static void cpuDec(uint8_t *reg)
{
	uint8_t tf = f&P_FLAG_C;
	*reg = cpuDoSub8(*reg,1);
	f&=~P_FLAG_C;
	f|=tf;
}

void cpuDAA(uint8_t *reg)
{
	int16_t in = *reg;

	if (!(f & P_FLAG_N))
	{
		if ((f & P_FLAG_H) || (in & 0xF) > 9)
			in += 0x06;

		if ((f & P_FLAG_C) || in > 0x9F)
			in += 0x60;
	}
	else
	{
		if (f & P_FLAG_H)
			in = (in - 6) & 0xFF;

		if (f & P_FLAG_C)
			in -= 0x60;
	}

	f &= ~(P_FLAG_H | P_FLAG_Z);

	if ((in & 0x100) == 0x100)
		f |= P_FLAG_C;

	in &= 0xFF;

	if (in == 0)
		f |= P_FLAG_Z;

	*reg = (uint8_t)in;
}

static void cpuSTOP(uint8_t *none)
{
	(void)none;
	//printf("CPU called STOP instruction\n");
	if(cpuDoStopSwitch)
	{
		cpuSetSpeed(!cpuCgbSpeed);
		cpuDoStopSwitch = false;
	}
	else
		cpuStopLoop = true;
	//takes up 2 instructions?
	pc++;
}

enum {
	CPU_GET_INSTRUCTION = 0,
	CPU_GET_SUBINSTRUCTION,
	CPU_DELAY_CYCLE,
	CPU_DELAY_RETI_CYCLE,
	CPU_ACTION_GET_INSTRUCTION,
	CPU_A_ACTION_GET_INSTRUCTION,
	CPU_B_ACTION_GET_INSTRUCTION,
	CPU_C_ACTION_GET_INSTRUCTION,
	CPU_D_ACTION_GET_INSTRUCTION,
	CPU_E_ACTION_GET_INSTRUCTION,
	CPU_H_ACTION_GET_INSTRUCTION,
	CPU_L_ACTION_GET_INSTRUCTION,
	CPU_ACTION_WRITE,
	CPU_A_ACTION_WRITE,
	CPU_B_ACTION_WRITE,
	CPU_C_ACTION_WRITE,
	CPU_D_ACTION_WRITE,
	CPU_E_ACTION_WRITE,
	CPU_H_ACTION_WRITE,
	CPU_L_ACTION_WRITE,
	CPU_BC_ACTION_ADD,
	CPU_DE_ACTION_ADD,
	CPU_HL_ACTION_ADD,
	CPU_SP_ACTION_ADD,
	CPU_HL_ADD_SPECIAL,
	CPU_SP_ADD_SPECIAL,
	CPU_ACTION_WRITE8_HL,
	CPU_TMP_ADD_PC,
	CPU_TMP_READ8_BC,
	CPU_TMP_READ8_DE,
	CPU_TMP_READ8_HL,
	CPU_TMP_READ8_HL_INC,
	CPU_TMP_READ8_HL_DEC,
	CPU_TMP_READ8_PC_INC,
	CPU_TMP_READ8_PC_INC_JRNZ_CHK,
	CPU_TMP_READ8_PC_INC_JRZ_CHK,
	CPU_TMP_READ8_PC_INC_JRNC_CHK,
	CPU_TMP_READ8_PC_INC_JRC_CHK,
	CPU_TMP_READ8_SP_INC,
	CPU_PCL_FROM_TMP_PCH_READ8_SP_INC,
	CPU_C_FROM_TMP_B_READ8_SP_INC,
	CPU_E_FROM_TMP_D_READ8_SP_INC,
	CPU_L_FROM_TMP_H_READ8_SP_INC,
	CPU_F_FROM_TMP_A_READ8_SP_INC,
	CPU_TMP_READHIGH_A,
	CPU_TMP_WRITEHIGH_A,
	CPU_C_READHIGH_A,
	CPU_C_WRITEHIGH_A,
	CPU_SP_FROM_HL,
	CPU_SP_WRITE8_A_DEC,
	CPU_SP_WRITE8_B_DEC,
	CPU_SP_WRITE8_C_DEC,
	CPU_SP_WRITE8_D_DEC,
	CPU_SP_WRITE8_E_DEC,
	CPU_SP_WRITE8_F_DEC,
	CPU_SP_WRITE8_H_DEC,
	CPU_SP_WRITE8_L_DEC,
	CPU_SP_WRITE8_PCH_DEC,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_T16,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_00,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_08,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_10,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_18,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_20,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_28,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_30,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_38,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_40,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_48,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_50,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_58,
	CPU_SP_WRITE8_PCL_DEC_PC_FROM_60,
	CPU_A_READ8_TMP16,
	CPU_A_READ8_PC_INC,
	CPU_B_READ8_PC_INC,
	CPU_C_READ8_PC_INC,
	CPU_D_READ8_PC_INC,
	CPU_E_READ8_PC_INC,
	CPU_L_READ8_PC_INC,
	CPU_H_READ8_PC_INC,
	CPU_PCL_FROM_TMP_PCH_READ8_PC,
	CPU_SPL_FROM_TMP_SPH_READ8_PC_INC,
	CPU_T16L_FROM_TMP_T16H_READ8_PC_INC,
	CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CNZ_CHK,
	CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CNC_CHK,
	CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CZ_CHK,
	CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CC_CHK,
	CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPNZ_CHK,
	CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPNC_CHK,
	CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPZ_CHK,
	CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPC_CHK,
	CPU_TMP16_WRITE8_SPL_INC,
	CPU_TMP16_WRITE8_SPH,
	CPU_DI_GET_INSTRUCTION,
	CPU_EI_GET_INSTRUCTION,
	CPU_SCF_GET_INSTRUCTION,
	CPU_CCF_GET_INSTRUCTION,
	CPU_PC_FROM_HL_GET_INSTRUCTION,
	CPU_PC_FROM_T16,
	CPU_RET_NZ_CHK,
	CPU_RET_NC_CHK,
	CPU_RET_Z_CHK,
	CPU_RET_C_CHK,
};

/* arrays for multiple similar instructions */
static uint8_t cpu_imm_arr[1] = { CPU_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_a_arr[1] = { CPU_A_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_b_arr[1] = { CPU_B_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_c_arr[1] = { CPU_C_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_d_arr[1] = { CPU_D_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_e_arr[1] = { CPU_E_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_h_arr[1] = { CPU_H_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_l_arr[1] = { CPU_L_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_bc_arr[2] = { CPU_TMP_READ8_BC, CPU_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_de_arr[2] = { CPU_TMP_READ8_DE, CPU_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_hl_arr[2] = { CPU_TMP_READ8_HL, CPU_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_pc_arr[2] = { CPU_TMP_READ8_PC_INC, CPU_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_hl_inc_arr[2] = { CPU_TMP_READ8_HL_INC, CPU_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_hl_dec_arr[2] = { CPU_TMP_READ8_HL_DEC, CPU_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_delay_arr[2] = { CPU_DELAY_CYCLE, CPU_ACTION_GET_INSTRUCTION };
static uint8_t cpu_imm_hl_st_arr[3] = { CPU_TMP_READ8_HL, CPU_ACTION_WRITE8_HL, CPU_GET_INSTRUCTION };

/* arrays for single special instructions */
static uint8_t cpu_nop_arr[1] = { CPU_GET_INSTRUCTION };
static uint8_t cpu_sub_arr[1] = { CPU_GET_SUBINSTRUCTION };
static uint8_t cpu_hljmp_arr[1] = { CPU_PC_FROM_HL_GET_INSTRUCTION };
static uint8_t cpu_sp_from_hl_arr[2] = { CPU_SP_FROM_HL, CPU_GET_INSTRUCTION };
static uint8_t cpu_absjmp_arr[4] = { CPU_TMP_READ8_PC_INC, CPU_PCL_FROM_TMP_PCH_READ8_PC, CPU_DELAY_CYCLE, CPU_GET_INSTRUCTION };
static uint8_t cpu_absjmpnz_arr[4] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPNZ_CHK, CPU_PC_FROM_T16, CPU_GET_INSTRUCTION };
static uint8_t cpu_absjmpnc_arr[4] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPNC_CHK, CPU_PC_FROM_T16, CPU_GET_INSTRUCTION };
static uint8_t cpu_absjmpz_arr[4] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPZ_CHK, CPU_PC_FROM_T16, CPU_GET_INSTRUCTION };
static uint8_t cpu_absjmpc_arr[4] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPC_CHK, CPU_PC_FROM_T16, CPU_GET_INSTRUCTION };

static uint8_t cpu_abscall_arr[6] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC, CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_T16, CPU_GET_INSTRUCTION };
static uint8_t cpu_abscallnz_arr[6] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CNZ_CHK, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_T16, CPU_PC_FROM_T16, CPU_GET_INSTRUCTION };
static uint8_t cpu_abscallnc_arr[6] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CNC_CHK, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_T16, CPU_PC_FROM_T16, CPU_GET_INSTRUCTION };
static uint8_t cpu_abscallz_arr[6] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CZ_CHK, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_T16, CPU_PC_FROM_T16, CPU_GET_INSTRUCTION };
static uint8_t cpu_abscallc_arr[6] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CC_CHK, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_T16, CPU_PC_FROM_T16, CPU_GET_INSTRUCTION };

static uint8_t cpu_ret_arr[4] = { CPU_DELAY_CYCLE, CPU_TMP_READ8_SP_INC, CPU_PCL_FROM_TMP_PCH_READ8_SP_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_reti_arr[4] = { CPU_DELAY_RETI_CYCLE, CPU_TMP_READ8_SP_INC, CPU_PCL_FROM_TMP_PCH_READ8_SP_INC, CPU_EI_GET_INSTRUCTION };
static uint8_t cpu_retnz_arr[5] = { CPU_RET_NZ_CHK, CPU_DELAY_CYCLE, CPU_TMP_READ8_SP_INC, CPU_PCL_FROM_TMP_PCH_READ8_SP_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_retnc_arr[5] = { CPU_RET_NC_CHK, CPU_DELAY_CYCLE, CPU_TMP_READ8_SP_INC, CPU_PCL_FROM_TMP_PCH_READ8_SP_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_retz_arr[5] = { CPU_RET_Z_CHK, CPU_DELAY_CYCLE, CPU_TMP_READ8_SP_INC, CPU_PCL_FROM_TMP_PCH_READ8_SP_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_retc_arr[5] = { CPU_RET_C_CHK, CPU_DELAY_CYCLE, CPU_TMP_READ8_SP_INC, CPU_PCL_FROM_TMP_PCH_READ8_SP_INC, CPU_GET_INSTRUCTION };

static uint8_t cpu_rst00_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_00, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst08_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_08, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst10_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_10, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst18_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_18, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst20_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_20, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst28_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_28, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst30_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_30, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst38_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_38, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst40_arr[5] = { CPU_DELAY_CYCLE, CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_40, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst48_arr[5] = { CPU_DELAY_CYCLE, CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_48, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst50_arr[5] = { CPU_DELAY_CYCLE, CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_50, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst58_arr[5] = { CPU_DELAY_CYCLE, CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_58, CPU_GET_INSTRUCTION };
static uint8_t cpu_rst60_arr[5] = { CPU_DELAY_CYCLE, CPU_DELAY_CYCLE, CPU_SP_WRITE8_PCH_DEC, CPU_SP_WRITE8_PCL_DEC_PC_FROM_60, CPU_GET_INSTRUCTION };

static uint8_t cpu_push_bc_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_B_DEC, CPU_SP_WRITE8_C_DEC, CPU_GET_INSTRUCTION };
static uint8_t cpu_push_de_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_D_DEC, CPU_SP_WRITE8_E_DEC, CPU_GET_INSTRUCTION };
static uint8_t cpu_push_hl_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_H_DEC, CPU_SP_WRITE8_L_DEC, CPU_GET_INSTRUCTION };
static uint8_t cpu_push_af_arr[4] = { CPU_DELAY_CYCLE, CPU_SP_WRITE8_A_DEC, CPU_SP_WRITE8_F_DEC, CPU_GET_INSTRUCTION };

static uint8_t cpu_pop_bc_arr[3] = { CPU_TMP_READ8_SP_INC, CPU_C_FROM_TMP_B_READ8_SP_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_pop_de_arr[3] = { CPU_TMP_READ8_SP_INC, CPU_E_FROM_TMP_D_READ8_SP_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_pop_hl_arr[3] = { CPU_TMP_READ8_SP_INC, CPU_L_FROM_TMP_H_READ8_SP_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_pop_af_arr[3] = { CPU_TMP_READ8_SP_INC, CPU_F_FROM_TMP_A_READ8_SP_INC, CPU_GET_INSTRUCTION };

static uint8_t cpu_ld_a_arr[2] = { CPU_A_READ8_PC_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_ld_b_arr[2] = { CPU_B_READ8_PC_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_ld_c_arr[2] = { CPU_C_READ8_PC_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_ld_d_arr[2] = { CPU_D_READ8_PC_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_ld_e_arr[2] = { CPU_E_READ8_PC_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_ld_h_arr[2] = { CPU_H_READ8_PC_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_ld_l_arr[2] = { CPU_L_READ8_PC_INC, CPU_GET_INSTRUCTION };

static uint8_t cpu_ldhc_a_arr[2] = { CPU_C_READHIGH_A, CPU_GET_INSTRUCTION };

static uint8_t cpu_ldh_a_arr[3] = { CPU_TMP_READ8_PC_INC, CPU_TMP_READHIGH_A, CPU_GET_INSTRUCTION };

static uint8_t cpu_ld_bc_arr[3] = { CPU_C_READ8_PC_INC, CPU_B_READ8_PC_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_ld_de_arr[3] = { CPU_E_READ8_PC_INC, CPU_D_READ8_PC_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_ld_hl_arr[3] = { CPU_L_READ8_PC_INC, CPU_H_READ8_PC_INC, CPU_GET_INSTRUCTION };
static uint8_t cpu_ld_sp_arr[3] = { CPU_TMP_READ8_PC_INC, CPU_SPL_FROM_TMP_SPH_READ8_PC_INC, CPU_GET_INSTRUCTION };

static uint8_t cpu_ld16_a_arr[4] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC, CPU_A_READ8_TMP16, CPU_GET_INSTRUCTION };

static uint8_t cpu_st_a_arr[2] = { CPU_A_ACTION_WRITE, CPU_GET_INSTRUCTION };
static uint8_t cpu_st_b_arr[2] = { CPU_B_ACTION_WRITE, CPU_GET_INSTRUCTION };
static uint8_t cpu_st_c_arr[2] = { CPU_C_ACTION_WRITE, CPU_GET_INSTRUCTION };
static uint8_t cpu_st_d_arr[2] = { CPU_D_ACTION_WRITE, CPU_GET_INSTRUCTION };
static uint8_t cpu_st_e_arr[2] = { CPU_E_ACTION_WRITE, CPU_GET_INSTRUCTION };
static uint8_t cpu_st_h_arr[2] = { CPU_H_ACTION_WRITE, CPU_GET_INSTRUCTION };
static uint8_t cpu_st_l_arr[2] = { CPU_L_ACTION_WRITE, CPU_GET_INSTRUCTION };

static uint8_t cpu_st_imm_arr[3] = { CPU_TMP_READ8_PC_INC, CPU_ACTION_WRITE, CPU_GET_INSTRUCTION };

static uint8_t cpu_sthc_a_arr[2] = { CPU_C_WRITEHIGH_A, CPU_GET_INSTRUCTION };
static uint8_t cpu_sth_a_arr[3] = { CPU_TMP_READ8_PC_INC, CPU_TMP_WRITEHIGH_A, CPU_GET_INSTRUCTION };

static uint8_t cpu_st16_a_arr[4] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC, CPU_A_ACTION_WRITE, CPU_GET_INSTRUCTION };

static uint8_t cpu_st16_sp_arr[5] = { CPU_TMP_READ8_PC_INC, CPU_T16L_FROM_TMP_T16H_READ8_PC_INC, CPU_TMP16_WRITE8_SPL_INC, CPU_TMP16_WRITE8_SPH, CPU_GET_INSTRUCTION };

static uint8_t cpu_jr_arr[3] = { CPU_TMP_READ8_PC_INC, CPU_TMP_ADD_PC, CPU_GET_INSTRUCTION };
static uint8_t cpu_jrnz_arr[3] = { CPU_TMP_READ8_PC_INC_JRNZ_CHK, CPU_TMP_ADD_PC, CPU_GET_INSTRUCTION };
static uint8_t cpu_jrz_arr[3] = { CPU_TMP_READ8_PC_INC_JRZ_CHK, CPU_TMP_ADD_PC, CPU_GET_INSTRUCTION };
static uint8_t cpu_jrnc_arr[3] = { CPU_TMP_READ8_PC_INC_JRNC_CHK, CPU_TMP_ADD_PC, CPU_GET_INSTRUCTION };
static uint8_t cpu_jrc_arr[3] = { CPU_TMP_READ8_PC_INC_JRC_CHK, CPU_TMP_ADD_PC, CPU_GET_INSTRUCTION };

static uint8_t cpu_di_arr[1] = { CPU_DI_GET_INSTRUCTION };
static uint8_t cpu_ei_arr[1] = { CPU_EI_GET_INSTRUCTION };
static uint8_t cpu_scf_arr[1] = { CPU_SCF_GET_INSTRUCTION };
static uint8_t cpu_ccf_arr[1] = { CPU_CCF_GET_INSTRUCTION };

static uint8_t cpu_add_bc_arr[2] = { CPU_BC_ACTION_ADD, CPU_GET_INSTRUCTION };
static uint8_t cpu_add_de_arr[2] = { CPU_DE_ACTION_ADD, CPU_GET_INSTRUCTION };
static uint8_t cpu_add_hl_arr[2] = { CPU_HL_ACTION_ADD, CPU_GET_INSTRUCTION };
static uint8_t cpu_add_sp_arr[2] = { CPU_SP_ACTION_ADD, CPU_GET_INSTRUCTION };
static uint8_t cpu_ld_hl_add_sp_imm_arr[3] = { CPU_TMP_READ8_PC_INC, CPU_HL_ADD_SPECIAL, CPU_GET_INSTRUCTION };
static uint8_t cpu_add_sp_imm_arr[4] = { CPU_TMP_READ8_PC_INC, CPU_SP_ADD_SPECIAL, CPU_DELAY_CYCLE, CPU_GET_INSTRUCTION };

static uint8_t cpu_start_arr[1] = { CPU_GET_INSTRUCTION };
static uint8_t *cpu_action_arr = cpu_start_arr;
static uint8_t cpu_arr_pos = 0;

static uint8_t *cpu_instr_arr[256] = {
	cpu_nop_arr, cpu_ld_bc_arr, cpu_st_a_arr, cpu_imm_delay_arr, //0x00-0x03
	cpu_imm_b_arr, cpu_imm_b_arr, cpu_ld_b_arr, cpu_imm_a_arr, //0x04-0x07
	cpu_st16_sp_arr, cpu_add_bc_arr, cpu_imm_bc_arr, cpu_imm_delay_arr, //0x08-0x0B
	cpu_imm_c_arr, cpu_imm_c_arr, cpu_ld_c_arr, cpu_imm_a_arr, //0x0C-0x0F

	cpu_imm_arr, cpu_ld_de_arr, cpu_st_a_arr, cpu_imm_delay_arr, //0x10-0x13
	cpu_imm_d_arr, cpu_imm_d_arr, cpu_ld_d_arr, cpu_imm_a_arr, //0x14-0x17
	cpu_jr_arr, cpu_add_de_arr, cpu_imm_de_arr, cpu_imm_delay_arr, //0x18-0x1B
	cpu_imm_e_arr, cpu_imm_e_arr, cpu_ld_e_arr, cpu_imm_a_arr, //0x1C-0x1F

	cpu_jrnz_arr, cpu_ld_hl_arr, cpu_st_a_arr, cpu_imm_delay_arr, //0x20-0x23
	cpu_imm_h_arr, cpu_imm_h_arr, cpu_ld_h_arr, cpu_imm_a_arr, //0x24-0x27
	cpu_jrz_arr, cpu_add_hl_arr, cpu_imm_hl_inc_arr, cpu_imm_delay_arr, //0x28-0x2B
	cpu_imm_l_arr, cpu_imm_l_arr, cpu_ld_l_arr, cpu_imm_a_arr, //0x2C-0x2F

	cpu_jrnc_arr, cpu_ld_sp_arr, cpu_st_a_arr, cpu_imm_delay_arr, //0x30-0x33
	cpu_imm_hl_st_arr, cpu_imm_hl_st_arr, cpu_st_imm_arr, cpu_scf_arr, //0x34-0x37
	cpu_jrc_arr, cpu_add_sp_arr, cpu_imm_hl_dec_arr, cpu_imm_delay_arr, //0x38-0x3B
	cpu_imm_a_arr, cpu_imm_a_arr, cpu_ld_a_arr, cpu_ccf_arr, //0x3C-0x3F

	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x40-0x43
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x44-0x47
	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x48-0x4B
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x4C-0x4F

	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x50-0x53
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x54-0x57
	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x58-0x5B
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x5C-0x5F

	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x60-0x63
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x64-0x67
	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x68-0x6B
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x6C-0x6F

	cpu_st_b_arr, cpu_st_c_arr, cpu_st_d_arr, cpu_st_e_arr, //0x70-0x73
	cpu_st_h_arr, cpu_st_l_arr, cpu_imm_arr, cpu_st_a_arr, //0x74-0x77
	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x78-0x7B
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x7C-0x7F

	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x80-0x83
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x84-0x87
	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x88-0x8B
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x8C-0x8F

	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x90-0x93
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x94-0x97
	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0x98-0x9B
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0x9C-0x9F

	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0xA0-0xA3
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0xA4-0xA7
	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0xA8-0xAB
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0xAC-0xAF

	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0xB0-0xB3
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0xB4-0xB7
	cpu_imm_b_arr, cpu_imm_c_arr, cpu_imm_d_arr, cpu_imm_e_arr, //0xB8-0xBB
	cpu_imm_h_arr, cpu_imm_l_arr, cpu_imm_hl_arr, cpu_imm_a_arr, //0xBC-0xBF

	cpu_retnz_arr, cpu_pop_bc_arr, cpu_absjmpnz_arr, cpu_absjmp_arr, //0xC0-0xC3
	cpu_abscallnz_arr, cpu_push_bc_arr, cpu_imm_pc_arr, cpu_rst00_arr, //0xC4-0xC7
	cpu_retz_arr, cpu_ret_arr, cpu_absjmpz_arr, cpu_sub_arr, //0xC8-0xCB
	cpu_abscallz_arr, cpu_abscall_arr, cpu_imm_pc_arr, cpu_rst08_arr, //0xCC-0xCF

	cpu_retnc_arr, cpu_pop_de_arr, cpu_absjmpnc_arr, NULL, //0xD0-0xD3 (0xD3=Invalid)
	cpu_abscallnc_arr, cpu_push_de_arr, cpu_imm_pc_arr, cpu_rst10_arr, //0xD4-0xD7
	cpu_retc_arr, cpu_reti_arr, cpu_absjmpc_arr, NULL, //0xD8-0xDB (0xDB=Invalid)
	cpu_abscallc_arr, NULL, cpu_imm_pc_arr, cpu_rst18_arr, //0xDC-0xDF (0xDD=Invalid)

	cpu_sth_a_arr, cpu_pop_hl_arr, cpu_sthc_a_arr, NULL, //0xE0-0xE3 (0xE3=Invalid)
	NULL, cpu_push_hl_arr, cpu_imm_pc_arr, cpu_rst20_arr, //0xE4-0xE7 (0xE4=Invalid)
	cpu_add_sp_imm_arr, cpu_hljmp_arr, cpu_st16_a_arr, NULL, //0xE8-0xEB (0xEB=Invalid)
	NULL, NULL, cpu_imm_pc_arr, cpu_rst28_arr, //0xEC-0xEF (0xEC=Invalid, 0xED=Invalid)

	cpu_ldh_a_arr, cpu_pop_af_arr, cpu_ldhc_a_arr, cpu_di_arr, //0xF0-0xF3
	NULL, cpu_push_af_arr, cpu_imm_pc_arr, cpu_rst30_arr, //0xF4-0xF7 (0xF4=Invalid)
	cpu_ld_hl_add_sp_imm_arr, cpu_sp_from_hl_arr, cpu_ld16_a_arr, cpu_ei_arr, //0xF8-0xFB
	NULL, NULL, cpu_imm_pc_arr, cpu_rst38_arr, //0xFC-0xFF (0xFC=Invalid, 0xFD=Invalid)
};

typedef void (*cpu_action_t)(uint8_t*);
static cpu_action_t cpu_actions_arr[256];
static cpu_action_t cpu_action_func;

void cpuSetupActionArr()
{
	cpu_actions_arr[0x00] = cpuNoAction; cpu_actions_arr[0x01] = cpuNoAction; cpu_actions_arr[0x02] = cpuSTbc; cpu_actions_arr[0x03] = cpuBcInc;
	cpu_actions_arr[0x04] = cpuInc; cpu_actions_arr[0x05] = cpuDec; cpu_actions_arr[0x06] = cpuNoAction; cpu_actions_arr[0x07] = cpuRLCA;
	cpu_actions_arr[0x08] = cpuNoAction; cpu_actions_arr[0x09] = cpuNoAction; cpu_actions_arr[0x0A] = cpuLDa; cpu_actions_arr[0x0B] = cpuBcDec; 
	cpu_actions_arr[0x0C] = cpuInc; cpu_actions_arr[0x0D] = cpuDec; cpu_actions_arr[0x0E] = cpuNoAction; cpu_actions_arr[0x0F] = cpuRRCA;

	cpu_actions_arr[0x10] = cpuSTOP; cpu_actions_arr[0x11] = cpuNoAction; cpu_actions_arr[0x12] = cpuSTde; cpu_actions_arr[0x13] = cpuDeInc;
	cpu_actions_arr[0x14] = cpuInc; cpu_actions_arr[0x15] = cpuDec; cpu_actions_arr[0x16] = cpuNoAction; cpu_actions_arr[0x17] = cpuRLA;
	cpu_actions_arr[0x18] = cpuNoAction; cpu_actions_arr[0x19] = cpuNoAction; cpu_actions_arr[0x1A] = cpuLDa; cpu_actions_arr[0x1B] = cpuDeDec; 
	cpu_actions_arr[0x1C] = cpuInc; cpu_actions_arr[0x1D] = cpuDec; cpu_actions_arr[0x1E] = cpuNoAction; cpu_actions_arr[0x1F] = cpuRRA;

	cpu_actions_arr[0x20] = cpuNoAction; cpu_actions_arr[0x21] = cpuNoAction; cpu_actions_arr[0x22] = cpuSThlInc; cpu_actions_arr[0x23] = cpuHlInc;
	cpu_actions_arr[0x24] = cpuInc; cpu_actions_arr[0x25] = cpuDec; cpu_actions_arr[0x26] = cpuNoAction; cpu_actions_arr[0x27] = cpuDAA;
	cpu_actions_arr[0x28] = cpuNoAction; cpu_actions_arr[0x29] = cpuNoAction; cpu_actions_arr[0x2A] = cpuLDa; cpu_actions_arr[0x2B] = cpuHlDec; 
	cpu_actions_arr[0x2C] = cpuInc; cpu_actions_arr[0x2D] = cpuDec; cpu_actions_arr[0x2E] = cpuNoAction; cpu_actions_arr[0x2F] = cpuCPL;

	cpu_actions_arr[0x30] = cpuNoAction; cpu_actions_arr[0x31] = cpuNoAction; cpu_actions_arr[0x32] = cpuSThlDec; cpu_actions_arr[0x33] = cpuSpInc;
	cpu_actions_arr[0x34] = cpuInc; cpu_actions_arr[0x35] = cpuDec; cpu_actions_arr[0x36] = cpuSThl; cpu_actions_arr[0x37] = cpuNoAction;
	cpu_actions_arr[0x38] = cpuNoAction; cpu_actions_arr[0x39] = cpuNoAction; cpu_actions_arr[0x3A] = cpuLDa; cpu_actions_arr[0x3B] = cpuSpDec; 
	cpu_actions_arr[0x3C] = cpuInc; cpu_actions_arr[0x3D] = cpuDec; cpu_actions_arr[0x3E] = cpuNoAction; cpu_actions_arr[0x3F] = cpuNoAction;

	cpu_actions_arr[0x40] = cpuLDb; cpu_actions_arr[0x41] = cpuLDb; cpu_actions_arr[0x42] = cpuLDb; cpu_actions_arr[0x43] = cpuLDb;
	cpu_actions_arr[0x44] = cpuLDb; cpu_actions_arr[0x45] = cpuLDb; cpu_actions_arr[0x46] = cpuLDb; cpu_actions_arr[0x47] = cpuLDb;
	cpu_actions_arr[0x48] = cpuLDc; cpu_actions_arr[0x49] = cpuLDc; cpu_actions_arr[0x4A] = cpuLDc; cpu_actions_arr[0x4B] = cpuLDc; 
	cpu_actions_arr[0x4C] = cpuLDc; cpu_actions_arr[0x4D] = cpuLDc; cpu_actions_arr[0x4E] = cpuLDc; cpu_actions_arr[0x4F] = cpuLDc;

	cpu_actions_arr[0x50] = cpuLDd; cpu_actions_arr[0x51] = cpuLDd; cpu_actions_arr[0x52] = cpuLDd; cpu_actions_arr[0x53] = cpuLDd;
	cpu_actions_arr[0x54] = cpuLDd; cpu_actions_arr[0x55] = cpuLDd; cpu_actions_arr[0x56] = cpuLDd; cpu_actions_arr[0x57] = cpuLDd;
	cpu_actions_arr[0x58] = cpuLDe; cpu_actions_arr[0x59] = cpuLDe; cpu_actions_arr[0x5A] = cpuLDe; cpu_actions_arr[0x5B] = cpuLDe; 
	cpu_actions_arr[0x5C] = cpuLDe; cpu_actions_arr[0x5D] = cpuLDe; cpu_actions_arr[0x5E] = cpuLDe; cpu_actions_arr[0x5F] = cpuLDe;

	cpu_actions_arr[0x60] = cpuLDh; cpu_actions_arr[0x61] = cpuLDh; cpu_actions_arr[0x62] = cpuLDh; cpu_actions_arr[0x63] = cpuLDh;
	cpu_actions_arr[0x64] = cpuLDh; cpu_actions_arr[0x65] = cpuLDh; cpu_actions_arr[0x66] = cpuLDh; cpu_actions_arr[0x67] = cpuLDh;
	cpu_actions_arr[0x68] = cpuLDl; cpu_actions_arr[0x69] = cpuLDl; cpu_actions_arr[0x6A] = cpuLDl; cpu_actions_arr[0x6B] = cpuLDl; 
	cpu_actions_arr[0x6C] = cpuLDl; cpu_actions_arr[0x6D] = cpuLDl; cpu_actions_arr[0x6E] = cpuLDl; cpu_actions_arr[0x6F] = cpuLDl;

	cpu_actions_arr[0x70] = cpuSThl; cpu_actions_arr[0x71] = cpuSThl; cpu_actions_arr[0x72] = cpuSThl; cpu_actions_arr[0x73] = cpuSThl;
	cpu_actions_arr[0x74] = cpuSThl; cpu_actions_arr[0x75] = cpuSThl; cpu_actions_arr[0x76] = cpuHALT; cpu_actions_arr[0x77] = cpuSThl;
	cpu_actions_arr[0x78] = cpuLDa; cpu_actions_arr[0x79] = cpuLDa; cpu_actions_arr[0x7A] = cpuLDa; cpu_actions_arr[0x7B] = cpuLDa; 
	cpu_actions_arr[0x7C] = cpuLDa; cpu_actions_arr[0x7D] = cpuLDa; cpu_actions_arr[0x7E] = cpuLDa; cpu_actions_arr[0x7F] = cpuLDa;

	cpu_actions_arr[0x80] = cpuAdd8; cpu_actions_arr[0x81] = cpuAdd8; cpu_actions_arr[0x82] = cpuAdd8; cpu_actions_arr[0x83] = cpuAdd8;
	cpu_actions_arr[0x84] = cpuAdd8; cpu_actions_arr[0x85] = cpuAdd8; cpu_actions_arr[0x86] = cpuAdd8; cpu_actions_arr[0x87] = cpuAdd8;
	cpu_actions_arr[0x88] = cpuAdc8; cpu_actions_arr[0x89] = cpuAdc8; cpu_actions_arr[0x8A] = cpuAdc8; cpu_actions_arr[0x8B] = cpuAdc8; 
	cpu_actions_arr[0x8C] = cpuAdc8; cpu_actions_arr[0x8D] = cpuAdc8; cpu_actions_arr[0x8E] = cpuAdc8; cpu_actions_arr[0x8F] = cpuAdc8;

	cpu_actions_arr[0x90] = cpuSub8; cpu_actions_arr[0x91] = cpuSub8; cpu_actions_arr[0x92] = cpuSub8; cpu_actions_arr[0x93] = cpuSub8;
	cpu_actions_arr[0x94] = cpuSub8; cpu_actions_arr[0x95] = cpuSub8; cpu_actions_arr[0x96] = cpuSub8; cpu_actions_arr[0x97] = cpuSub8;
	cpu_actions_arr[0x98] = cpuSbc8; cpu_actions_arr[0x99] = cpuSbc8; cpu_actions_arr[0x9A] = cpuSbc8; cpu_actions_arr[0x9B] = cpuSbc8; 
	cpu_actions_arr[0x9C] = cpuSbc8; cpu_actions_arr[0x9D] = cpuSbc8; cpu_actions_arr[0x9E] = cpuSbc8; cpu_actions_arr[0x9F] = cpuSbc8;

	cpu_actions_arr[0xA0] = cpuAND; cpu_actions_arr[0xA1] = cpuAND; cpu_actions_arr[0xA2] = cpuAND; cpu_actions_arr[0xA3] = cpuAND;
	cpu_actions_arr[0xA4] = cpuAND; cpu_actions_arr[0xA5] = cpuAND; cpu_actions_arr[0xA6] = cpuAND; cpu_actions_arr[0xA7] = cpuAND;
	cpu_actions_arr[0xA8] = cpuXOR; cpu_actions_arr[0xA9] = cpuXOR; cpu_actions_arr[0xAA] = cpuXOR; cpu_actions_arr[0xAB] = cpuXOR; 
	cpu_actions_arr[0xAC] = cpuXOR; cpu_actions_arr[0xAD] = cpuXOR; cpu_actions_arr[0xAE] = cpuXOR; cpu_actions_arr[0xAF] = cpuXOR;

	cpu_actions_arr[0xB0] = cpuOR; cpu_actions_arr[0xB1] = cpuOR; cpu_actions_arr[0xB2] = cpuOR; cpu_actions_arr[0xB3] = cpuOR;
	cpu_actions_arr[0xB4] = cpuOR; cpu_actions_arr[0xB5] = cpuOR; cpu_actions_arr[0xB6] = cpuOR; cpu_actions_arr[0xB7] = cpuOR;
	cpu_actions_arr[0xB8] = cpuCmp8; cpu_actions_arr[0xB9] = cpuCmp8; cpu_actions_arr[0xBA] = cpuCmp8; cpu_actions_arr[0xBB] = cpuCmp8; 
	cpu_actions_arr[0xBC] = cpuCmp8; cpu_actions_arr[0xBD] = cpuCmp8; cpu_actions_arr[0xBE] = cpuCmp8; cpu_actions_arr[0xBF] = cpuCmp8;

	cpu_actions_arr[0xC0] = cpuNoAction; cpu_actions_arr[0xC1] = cpuNoAction; cpu_actions_arr[0xC2] = cpuNoAction; cpu_actions_arr[0xC3] = cpuNoAction;
	cpu_actions_arr[0xC4] = cpuNoAction; cpu_actions_arr[0xC5] = cpuNoAction; cpu_actions_arr[0xC6] = cpuAdd8; cpu_actions_arr[0xC7] = cpuNoAction;
	cpu_actions_arr[0xC8] = cpuNoAction; cpu_actions_arr[0xC9] = cpuNoAction; cpu_actions_arr[0xCA] = cpuNoAction; cpu_actions_arr[0xCB] = cpuNoAction; 
	cpu_actions_arr[0xCC] = cpuNoAction; cpu_actions_arr[0xCD] = cpuNoAction; cpu_actions_arr[0xCE] = cpuAdc8; cpu_actions_arr[0xCF] = cpuNoAction;

	cpu_actions_arr[0xD0] = cpuNoAction; cpu_actions_arr[0xD1] = cpuNoAction; cpu_actions_arr[0xD2] = cpuNoAction; cpu_actions_arr[0xD3] = cpuNoAction;
	cpu_actions_arr[0xD4] = cpuNoAction; cpu_actions_arr[0xD5] = cpuNoAction; cpu_actions_arr[0xD6] = cpuSub8; cpu_actions_arr[0xD7] = cpuNoAction;
	cpu_actions_arr[0xD8] = cpuNoAction; cpu_actions_arr[0xD9] = cpuNoAction; cpu_actions_arr[0xDA] = cpuNoAction; cpu_actions_arr[0xDB] = cpuNoAction; 
	cpu_actions_arr[0xDC] = cpuNoAction; cpu_actions_arr[0xDD] = cpuNoAction; cpu_actions_arr[0xDE] = cpuSbc8; cpu_actions_arr[0xDF] = cpuNoAction;

	cpu_actions_arr[0xE0] = cpuNoAction; cpu_actions_arr[0xE1] = cpuNoAction; cpu_actions_arr[0xE2] = cpuNoAction; cpu_actions_arr[0xE3] = cpuNoAction;
	cpu_actions_arr[0xE4] = cpuNoAction; cpu_actions_arr[0xE5] = cpuNoAction; cpu_actions_arr[0xE6] = cpuAND; cpu_actions_arr[0xE7] = cpuNoAction;
	cpu_actions_arr[0xE8] = cpuNoAction; cpu_actions_arr[0xE9] = cpuNoAction; cpu_actions_arr[0xEA] = cpuSTt16; cpu_actions_arr[0xEB] = cpuNoAction; 
	cpu_actions_arr[0xEC] = cpuNoAction; cpu_actions_arr[0xED] = cpuNoAction; cpu_actions_arr[0xEE] = cpuXOR; cpu_actions_arr[0xEF] = cpuNoAction;

	cpu_actions_arr[0xF0] = cpuNoAction; cpu_actions_arr[0xF1] = cpuNoAction; cpu_actions_arr[0xF2] = cpuNoAction; cpu_actions_arr[0xF3] = cpuNoAction;
	cpu_actions_arr[0xF4] = cpuNoAction; cpu_actions_arr[0xF5] = cpuNoAction; cpu_actions_arr[0xF6] = cpuOR; cpu_actions_arr[0xF7] = cpuNoAction;
	cpu_actions_arr[0xF8] = cpuNoAction; cpu_actions_arr[0xF9] = cpuNoAction; cpu_actions_arr[0xFA] = cpuNoAction; cpu_actions_arr[0xFB] = cpuNoAction; 
	cpu_actions_arr[0xFC] = cpuNoAction; cpu_actions_arr[0xFD] = cpuNoAction; cpu_actions_arr[0xFE] = cpuCmp8; cpu_actions_arr[0xFF] = cpuNoAction;

	cpu_action_func = cpuNoAction;
}

bool firstIrq = false, secondIrq = false;

bool cpuHandleIrqUpdates()
{
	if(gbEmuGBSPlayback) return false;
	if(!irqEnable) return false;
	uint8_t irqList = (memGetCurIrqList());
	if(irqList & 1)
	{
		memClearCurIrqList(1);
		//printf("Beep Bop Interrupt 40 and jmp from %04x\n", pc);
		cpu_action_arr = cpu_rst40_arr;
		irqEnable = false;
		//if(firstIrq) secondIrq = true;
		//firstIrq = true;
		return true;
	}
	else if(irqList & 2)
	{
		memClearCurIrqList(2);
		//printf("Beep Bop Interrupt 48 and jmp from %04x\n", pc);
		cpu_action_arr = cpu_rst48_arr;
		irqEnable = false;
		return true;
	}
	else if(irqList & 4)
	{
		memClearCurIrqList(4);
		//printf("Beep Bop Interrupt 50 and jmp from %04x\n", pc);
		cpu_action_arr = cpu_rst50_arr;
		irqEnable = false;
		return true;
	}
	else if(irqList & 8)
	{
		memClearCurIrqList(8);
		//printf("Beep Bop Interrupt 58 and jmp from %04x\n", pc);
		cpu_action_arr = cpu_rst58_arr;
		irqEnable = false;
		return true;
	}
	else if(irqList & 0x10)
	{
		memClearCurIrqList(0x10);
		//printf("Beep Bop Interrupt 60 and jmp from %04x\n", pc);
		cpu_action_arr = cpu_rst60_arr;
		irqEnable = false;
		return true;
	}
	return false;
}
static uint8_t curInstr;
bool cpuGetInstruction()
{
	if(cpuHandleIrqUpdates())
	{
		cpuHaltLoop = false;
		cpu_arr_pos = 0;
		return true;
	}
	if(gbEmuGBSPlayback)
	{
		//init return
		if(pc == 0x8764)
		{
			//if(!gbsInitRet)
			//	printf("Init return\n");
			gbsInitRet = true; //allow play call
			cpu_action_arr = cpu_nop_arr;
			cpu_arr_pos = 0;
			return true;
		} //play return
		else if(pc == 0x8765)
		{
			//if(!gbsPlayRet)
			//	printf("Play return\n");
			gbsPlayRet = true; //allow next play call
			cpu_action_arr = cpu_nop_arr;
			cpu_arr_pos = 0;
			return true;
		}
	}
	if(cpuHaltLoop)
	{
		//happens when IME=0
		if(!irqEnable && memGetCurIrqList())
			cpuHaltLoop = false;
		cpu_action_arr = cpu_nop_arr;
		cpu_arr_pos = 0;
		return true;
	}
	if(cpuStopLoop)
	{
		if(inputAny())
			cpuStopLoop = false;
		cpu_action_arr = cpu_nop_arr;
		cpu_arr_pos = 0;
		return true;
	}
	curInstr = memGet8(pc);
	cpu_action_arr = cpu_instr_arr[curInstr];
	if(cpu_action_arr == NULL)
	{
		printf("Unsupported Instruction at %04x:%02x!\n", pc-1,curInstr);
		return false;
	}
	cpu_arr_pos = 0;
	cpu_action_func = cpu_actions_arr[curInstr];
	
	//if(pc==0xABC || pc == 0xAC1 || pc == 0x5E0E || pc == 0x5E0F)
	//	printf("%04x %02x a %02x b %02x hl %04x\n", pc, curInstr, a, b, (l|(h<<8)));
	//HALT bug: PC doesnt increase after instruction is parsed!
	if(!cpuHaltBug) pc++;
	cpuHaltBug = false;

	return true;
}

/* Main CPU Interpreter */
bool cpuDmaHalt = false;

bool cpuCycle()
{
	bool cycleret = true;
	if(cpuDmaHalt)
		return cycleret;
	uint8_t cpu_action, sub_instr;
	cpu_action = cpu_action_arr[cpu_arr_pos];
	cpu_arr_pos++;
	switch(cpu_action)
	{
		case CPU_GET_INSTRUCTION:
			cycleret = cpuGetInstruction();
			break;
		case CPU_GET_SUBINSTRUCTION:
			sub_instr = memGet8(pc++);
			//set sub array
			switch(sub_instr&7)
			{
				case 0:
					cpu_action_arr = cpu_imm_b_arr;
					break;
				case 1:
					cpu_action_arr = cpu_imm_c_arr;
					break;
				case 2:
					cpu_action_arr = cpu_imm_d_arr;
					break;
				case 3:
					cpu_action_arr = cpu_imm_e_arr;
					break;
				case 4:
					cpu_action_arr = cpu_imm_h_arr;
					break;
				case 5:
					cpu_action_arr = cpu_imm_l_arr;
					break;
				case 6:
					if((sub_instr&0xC0) == 0x40) //BIT only reads
						cpu_action_arr = cpu_imm_hl_arr;
					else //all other CB instructions do I/O
						cpu_action_arr = cpu_imm_hl_st_arr;
					break;
				case 7:
					cpu_action_arr = cpu_imm_a_arr;
					break;
				default: //should never happen
					printf("Unknown sub %02x\n", sub_instr);
					return false;
			}
			//set sub func
			switch(sub_instr>>3)
			{
				case 0:
					cpu_action_func = cpuRLC;
					break;
				case 1:
					cpu_action_func = cpuRRC;
					break;
				case 2:
					cpu_action_func = cpuRL;
					break;
				case 3:
					cpu_action_func = cpuRR;
					break;
				case 4:
					cpu_action_func = cpuSLA;
					break;
				case 5:
					cpu_action_func = cpuSRA;
					break;
				case 6:
					cpu_action_func = cpuSWAP;
					break;
				case 7:
					cpu_action_func = cpuSRL;
					break;
				case  8: case  9: case 10: case 11:
				case 12: case 13: case 14: case 15:
					sub_in_val = ((sub_instr&0x3F)>>3)&7;
					cpu_action_func = cpuBIT;
					break;
				case 16: case 17: case 18: case 19:
				case 20: case 21: case 22: case 23:
					sub_in_val = ((sub_instr&0x3F)>>3)&7;
					cpu_action_func = cpuRES;
					break;
				default:
					sub_in_val = ((sub_instr&0x3F)>>3)&7;
					cpu_action_func = cpuSET;
					break;
			}
			cpu_arr_pos = 0;
			break;
		case CPU_DELAY_CYCLE:
			break;
		case CPU_DELAY_RETI_CYCLE:
			//printf("RETI from %04x\n", pc);
			break;
		case CPU_ACTION_GET_INSTRUCTION:
			cpu_action_func(&cpuTmp);
			cycleret = cpuGetInstruction();
			break;
		case CPU_A_ACTION_GET_INSTRUCTION:
			cpu_action_func(&a);
			cycleret = cpuGetInstruction();
			break;
		case CPU_B_ACTION_GET_INSTRUCTION:
			cpu_action_func(&b);
			cycleret = cpuGetInstruction();
			break;
		case CPU_C_ACTION_GET_INSTRUCTION:
			cpu_action_func(&c);
			cycleret = cpuGetInstruction();
			break;
		case CPU_D_ACTION_GET_INSTRUCTION:
			cpu_action_func(&d);
			cycleret = cpuGetInstruction();
			break;
		case CPU_E_ACTION_GET_INSTRUCTION:
			cpu_action_func(&e);
			cycleret = cpuGetInstruction();
			break;
		case CPU_H_ACTION_GET_INSTRUCTION:
			cpu_action_func(&h);
			cycleret = cpuGetInstruction();
			break;
		case CPU_L_ACTION_GET_INSTRUCTION:
			cpu_action_func(&l);
			cycleret = cpuGetInstruction();
			break;
		case CPU_ACTION_WRITE:
			cpu_action_func(&cpuTmp);
			break;
		case CPU_A_ACTION_WRITE:
			cpu_action_func(&a);
			break;
		case CPU_B_ACTION_WRITE:
			cpu_action_func(&b);
			break;
		case CPU_C_ACTION_WRITE:
			cpu_action_func(&c);
			break;
		case CPU_D_ACTION_WRITE:
			cpu_action_func(&d);
			break;
		case CPU_E_ACTION_WRITE:
			cpu_action_func(&e);
			break;
		case CPU_H_ACTION_WRITE:
			cpu_action_func(&h);
			break;
		case CPU_L_ACTION_WRITE:
			cpu_action_func(&l);
			break;
		case CPU_BC_ACTION_ADD:
			cpuAdd16(c|b<<8);
			break;
		case CPU_DE_ACTION_ADD:
			cpuAdd16(e|d<<8);
			break;
		case CPU_HL_ACTION_ADD:
			cpuAdd16(l|h<<8);
			break;
		case CPU_SP_ACTION_ADD:
			cpuAdd16(sp);
			break;
		case CPU_HL_ADD_SPECIAL:
			cpuTmp16 = cpuAddSp16(cpuTmp);
			h = cpuTmp16>>8;
			l = cpuTmp16&0xFF;
			break;
		case CPU_SP_ADD_SPECIAL:
			sp = cpuAddSp16(cpuTmp);
			break;
		case CPU_ACTION_WRITE8_HL:
			cpu_action_func(&cpuTmp);
			memSet8(l | h<<8, cpuTmp);
			break;
		case CPU_TMP_ADD_PC:
			pc += (int8_t)cpuTmp;
			break;
		case CPU_TMP_READ8_BC:
			cpuTmp = memGet8(c | b<<8);
			break;
		case CPU_TMP_READ8_DE:
			cpuTmp = memGet8(e | d<<8);
			break;
		case CPU_TMP_READ8_HL:
			cpuTmp = memGet8(l | h<<8);
			break;
		case CPU_TMP_READ8_HL_INC:
			cpuTmp = memGet8(l | h<<8);
			cpuHlInc();
			break;
		case CPU_TMP_READ8_HL_DEC:
			cpuTmp = memGet8(l | h<<8);
			cpuHlDec();
			break;
		case CPU_TMP_READ8_PC_INC:
			cpuTmp = memGet8(pc++);
			break;
		case CPU_TMP_READ8_PC_INC_JRNZ_CHK:
			cpuTmp = memGet8(pc++);
			if(f & P_FLAG_Z) cpu_arr_pos++;
			break;
		case CPU_TMP_READ8_PC_INC_JRZ_CHK:
			cpuTmp = memGet8(pc++);
			if(!(f & P_FLAG_Z)) cpu_arr_pos++;
			break;
		case CPU_TMP_READ8_PC_INC_JRNC_CHK:
			cpuTmp = memGet8(pc++);
			if(f & P_FLAG_C) cpu_arr_pos++;
			break;
		case CPU_TMP_READ8_PC_INC_JRC_CHK:
			cpuTmp = memGet8(pc++);
			if(!(f & P_FLAG_C)) cpu_arr_pos++;
			break;
		case CPU_TMP_READ8_SP_INC:
			cpuTmp = memGet8(sp++);
			break;
		case CPU_PCL_FROM_TMP_PCH_READ8_SP_INC:
			pc = (cpuTmp | (memGet8(sp++)<<8));
			break;
		case CPU_C_FROM_TMP_B_READ8_SP_INC:
			c = cpuTmp;
			b = memGet8(sp++);
			break;
		case CPU_E_FROM_TMP_D_READ8_SP_INC:
			e = cpuTmp;
			d = memGet8(sp++);
			break;
		case CPU_L_FROM_TMP_H_READ8_SP_INC:
			l = cpuTmp;
			h = memGet8(sp++);
			break;
		case CPU_F_FROM_TMP_A_READ8_SP_INC:
			f = cpuTmp&0xF0;
			a = memGet8(sp++);
			break;
		case CPU_TMP_READHIGH_A:
			a = memGet8(0xFF00 | cpuTmp);
			break;
		case CPU_TMP_WRITEHIGH_A:
			memSet8(0xFF00 | cpuTmp, a);
			break;
		case CPU_C_READHIGH_A:
			a = memGet8(0xFF00 | c);
			break;
		case CPU_C_WRITEHIGH_A:
			memSet8(0xFF00 | c, a);
			break;
		case CPU_SP_FROM_HL:
			sp = (l | h<<8);
			break;
		case CPU_SP_WRITE8_A_DEC:
			sp--;
			memSet8(sp, a);
			break;
		case CPU_SP_WRITE8_B_DEC:
			sp--;
			memSet8(sp, b);
			break;
		case CPU_SP_WRITE8_C_DEC:
			sp--;
			memSet8(sp, c);
			break;
		case CPU_SP_WRITE8_D_DEC:
			sp--;
			memSet8(sp, d);
			break;
		case CPU_SP_WRITE8_E_DEC:
			sp--;
			memSet8(sp, e);
			break;
		case CPU_SP_WRITE8_F_DEC:
			sp--;
			memSet8(sp, f);
			break;
		case CPU_SP_WRITE8_H_DEC:
			sp--;
			memSet8(sp, h);
			break;
		case CPU_SP_WRITE8_L_DEC:
			sp--;
			memSet8(sp, l);
			break;
		case CPU_SP_WRITE8_PCH_DEC:
			sp--;
			memSet8(sp, pc>>8);
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_T16:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = cpuTmp16;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_00:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x00+gbsLoadAddr;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_08:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x08+gbsLoadAddr;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_10:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x10+gbsLoadAddr;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_18:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x18+gbsLoadAddr;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_20:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x20+gbsLoadAddr;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_28:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x28+gbsLoadAddr;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_30:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x30+gbsLoadAddr;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_38:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x38+gbsLoadAddr;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_40:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x40;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_48:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x48;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_50:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x50;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_58:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x58;
			break;
		case CPU_SP_WRITE8_PCL_DEC_PC_FROM_60:
			sp--;
			memSet8(sp, pc&0xFF);
			pc = 0x60;
			break;
		case CPU_A_READ8_TMP16:
			a = memGet8(cpuTmp16);
			break;
		case CPU_A_READ8_PC_INC:
			a = memGet8(pc++);
			break;
		case CPU_B_READ8_PC_INC:
			b = memGet8(pc++);
			break;
		case CPU_C_READ8_PC_INC:
			c = memGet8(pc++);
			break;
		case CPU_D_READ8_PC_INC:
			d = memGet8(pc++);
			break;
		case CPU_E_READ8_PC_INC:
			e = memGet8(pc++);
			break;
		case CPU_L_READ8_PC_INC:
			l = memGet8(pc++);
			break;
		case CPU_H_READ8_PC_INC:
			h = memGet8(pc++);
			break;
		case CPU_PCL_FROM_TMP_PCH_READ8_PC:
			pc = (cpuTmp | (memGet8(pc)<<8));
			break;
		case CPU_SPL_FROM_TMP_SPH_READ8_PC_INC:
			sp = (cpuTmp | (memGet8(pc++)<<8));
			break;
		case CPU_T16L_FROM_TMP_T16H_READ8_PC_INC:
			cpuTmp16 = (cpuTmp | (memGet8(pc++)<<8));
			break;
		case CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CNZ_CHK:
			cpuTmp16 = (cpuTmp | (memGet8(pc++)<<8));
			if(f & P_FLAG_Z) cpu_arr_pos+=3;
			break;
		case CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CNC_CHK:
			cpuTmp16 = (cpuTmp | (memGet8(pc++)<<8));
			if(f & P_FLAG_C) cpu_arr_pos+=3;
			break;
		case CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CZ_CHK:
			cpuTmp16 = (cpuTmp | (memGet8(pc++)<<8));
			if(!(f & P_FLAG_Z)) cpu_arr_pos+=3;
			break;
		case CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_CC_CHK:
			cpuTmp16 = (cpuTmp | (memGet8(pc++)<<8));
			if(!(f & P_FLAG_C)) cpu_arr_pos+=3;
			break;
		case CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPNZ_CHK:
			cpuTmp16 = (cpuTmp | (memGet8(pc++)<<8));
			if(f & P_FLAG_Z) cpu_arr_pos++;
			break;
		case CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPNC_CHK:
			cpuTmp16 = (cpuTmp | (memGet8(pc++)<<8));
			if(f & P_FLAG_C) cpu_arr_pos++;
			break;
		case CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPZ_CHK:
			cpuTmp16 = (cpuTmp | (memGet8(pc++)<<8));
			if(!(f & P_FLAG_Z)) cpu_arr_pos++;
			break;
		case CPU_T16L_FROM_TMP_T16H_READ8_PC_INC_JPC_CHK:
			cpuTmp16 = (cpuTmp | (memGet8(pc++)<<8));
			if(!(f & P_FLAG_C)) cpu_arr_pos++;
			break;
		case CPU_TMP16_WRITE8_SPL_INC:
			memSet8(cpuTmp16++, sp&0xFF);
			break;
		case CPU_TMP16_WRITE8_SPH:
			memSet8(cpuTmp16, sp>>8);
			break;
		case CPU_DI_GET_INSTRUCTION:
			//printf("Disabled IRQs at %04x\n", pc);
			irqEnable = false;
			cycleret = cpuGetInstruction();
			break;
		case CPU_EI_GET_INSTRUCTION:
			//printf("Enabled IRQs and jmp to %04x ",pc);
			cycleret = cpuGetInstruction();
			//printf("%04x\n",pc);
			irqEnable = true;
			break;
		case CPU_SCF_GET_INSTRUCTION:
			f |= P_FLAG_C;
			f &= ~(P_FLAG_H|P_FLAG_N);
			cycleret = cpuGetInstruction();
			break;
		case CPU_CCF_GET_INSTRUCTION:
			f ^= P_FLAG_C;
			f &= ~(P_FLAG_H|P_FLAG_N);
			cycleret = cpuGetInstruction();
			break;
		case CPU_PC_FROM_HL_GET_INSTRUCTION:
			pc = (l|(h<<8));
			cycleret = cpuGetInstruction();
			break;
		case CPU_PC_FROM_T16:
			pc = cpuTmp16;
			break;
		case CPU_RET_NZ_CHK:
			if(f & P_FLAG_Z) cpu_arr_pos+=3;
			break;
		case CPU_RET_NC_CHK:
			if(f & P_FLAG_C) cpu_arr_pos+=3;
			break;
		case CPU_RET_Z_CHK:
			if(!(f & P_FLAG_Z)) cpu_arr_pos+=3;
			break;
		case CPU_RET_C_CHK:
			if(!(f & P_FLAG_C)) cpu_arr_pos+=3;
			break;
	}
	return cycleret;
}

uint16_t cpuCurPC()
{
	return pc;
}

void cpuSetSpeed(bool cgb)
{
	if(cgb)
	{
		//printf("CPU: CGB Speed\n");
		cpuCgbSpeed = true;
		cpuTimer = 1;
	}
	else
	{
		//printf("CPU: DMG Speed\n");
		cpuCgbSpeed = false;
		cpuTimer = 3;
	}
}

void cpuPlayGBS()
{
	if(!gbsInitRet || !gbsPlayRet)
		return;
	gbsPlayRet = false;
	//push back detect pc
	sp--;
	memSet8(sp, 0x87);
	sp--;
	memSet8(sp, 0x65);
	//jump to play
	pc = gbsPlayAddr;
	cpu_action_arr = cpu_nop_arr;
	cpu_arr_pos = 0;
	//printf("Playback Start at %04x\n", pc);
}
extern uint8_t gbsTMA, gbsTAC;
void cpuLoadGBS(uint8_t song)
{
	//full reset
	cpuInit();
	ppuInit();
	apuInit();
	inputInit();
	memInit(false,true);
	memSet8(0xFF06,gbsTMA);
	memSet8(0xFF07,gbsTAC);
	//set requested sp
	sp = gbsSP;
	//push back detect pc
	sp--;
	memSet8(sp, 0x87);
	sp--;
	memSet8(sp, 0x64);
	//set song and init routine
	a = song;
	pc = gbsInitAddr;
	//start getting instructions
	cpu_action_arr = cpu_nop_arr;
	cpu_arr_pos = 0;
	//printf("Init Start at %04x\n", pc);
}
