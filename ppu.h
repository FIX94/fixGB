/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef _ppu_h_
#define _ppu_h_

void ppuInit();
bool ppuCycle();
bool ppuDrawDone();
uint8_t ppuGetVRAMBank8(uint16_t addr);
uint8_t ppuGetVRAMNoBank8(uint16_t addr);
uint8_t ppuGetOAM8(uint16_t addr);
uint8_t ppuGetReg8(uint16_t addr);
void ppuSetVRAMBank8(uint16_t addr, uint8_t val);
void ppuSetVRAMNoBank8(uint16_t addr, uint8_t val);
void ppuSetOAM8(uint16_t addr, uint8_t val);
void ppuSetReg8(uint16_t addr, uint8_t val);
bool ppuInVBlank();
bool ppuInHBlank();
void ppuDumpMem();

#endif
