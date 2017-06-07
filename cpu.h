/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef _cpu_c_
#define _cpu_h_

void cpuInit();
void cpuCycle();
uint16_t cpuCurPC();
void cpuSetSpeed(bool cgb);
void cpuLoadGBS(uint8_t song);
void cpuPlayGBS();

#endif
