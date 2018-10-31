/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef _mbc_h_
#define _mem_h_

enum {
	MBC_TYPE_NONE = 0,
	MBC_TYPE_1,
	MBC_TYPE_2,
	MBC_TYPE_3,
	MBC_TYPE_5,
	MBC_TYPE_6,
	MBC_TYPE_7,
	MBC_TYPE_GBS,
};

void mbcInit(uint8_t type);
void mbcResetRegs();
size_t mbcRTCSize();
void mbcRTCInit();
void mbcRTCLoad(FILE *f);
void mbcRTCStore(FILE *f);
void mbcExtRAMInit(uint8_t type);
void mbcExtRAMLoad(FILE *f);
void mbcExtRAMStore(FILE *f);
void mbcExtRAMGBSClear();
extern set8FuncT mbcSet8;
extern set8FuncT mbcSetRAM8;
extern get8FuncT mbcGetRAM8;

#endif