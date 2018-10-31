/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef _mem_h_
#define _mem_h_

typedef uint8_t (*get8FuncT)(uint16_t);
typedef void (*set8FuncT)(uint16_t, uint8_t val);

bool memInit(bool romcheck, bool gbs);
void memDeinit();
void memInitGetSetPointers();
bool memInitCGBBootrom();
uint8_t memGet8(uint16_t addr);
void memSet8(uint16_t addr, uint8_t val);
void memStartGBS();
void memDumpMainMem();
void memClockTimers();
void memDmaClockTimers();
void memSaveGame();

uint8_t memGetCurIrqList();
void memClearCurIrqList(uint8_t num);
void memEnableVBlankIrq();
void memEnableStatIrq();

extern uint8_t Ext_Mem[0x20000];

#endif
