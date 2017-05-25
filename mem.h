/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef _mem_h_
#define _mem_h_

bool memInit(bool romcheck, bool gbs);
void memStartGBS();
uint8_t memGet8(uint16_t addr);
void memSet8(uint16_t addr, uint8_t val);
void memSet16(uint16_t addr, uint16_t val);
void memDumpMainMem();
void memClockTimers();
void memDmaClockTimers();
void memSaveGame();

uint8_t memGetCurIrqList();
void memClearCurIrqList(uint8_t num);
void memEnableVBlankIrq();
void memEnableStatIrq();

#endif