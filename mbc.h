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
	MBC_TYPE_4,
	MBC_TYPE_5,
	MBC_TYPE_6,
	MBC_TYPE_7,
};

typedef void (*set8FuncT)(uint16_t, uint8_t);

void mbcInit(uint8_t type);

extern set8FuncT mbcSet8;

#endif