/*
 * Copyright (C) 2024 Mikhail Burakov. This file is part of receiver.
 *
 * receiver is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * receiver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with receiver.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MFX_STUB_INCLUDE_MFXCOMMON_H_
#define MFX_STUB_INCLUDE_MFXCOMMON_H_

#include "mfxdefs.h"

#define MFX_MAKEFOURCC(a, b, c, d)                       \
  (((a) << 24 & 0xff000000) | ((b) << 16 & 0x00ff0000) | \
   ((c) << 8 & 0x0000ff00) | ((d) << 0 & 0x000000ff))

typedef mfxI32 mfxIMPL;

enum {
  MFX_IMPL_HARDWARE = 0x0002,
};

typedef union mfxVersion mfxVersion;

typedef struct {
  mfxI64 DecodeTimeStamp;
  mfxU64 TimeStamp;
  mfxU8* Data;
  mfxU32 DataLength;
  mfxU32 MaxLength;
  mfxU16 DataFlag;
} mfxBitstream;

typedef struct _mfxSyncPoint* mfxSyncPoint;

#endif  // MFX_STUB_INCLUDE_MFXCOMMON_H_