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
uint8_t ppuGet8(uint16_t addr);
void ppuSet8(uint16_t addr, uint8_t val);
bool ppuInVBlank();
void ppuDumpMem();

#endif
