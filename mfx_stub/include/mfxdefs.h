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

#ifndef MFX_STUB_INCLUDE_MFXDEFS_H_
#define MFX_STUB_INCLUDE_MFXDEFS_H_

#include <stdint.h>

#define MFX_INFINITE 0xffffffff

typedef uint8_t mfxU8;
typedef int8_t mfxI8;
typedef int16_t mfxI16;
typedef uint16_t mfxU16;
typedef uint32_t mfxU32;
typedef int32_t mfxI32;
typedef uint64_t mfxU64;
typedef int64_t mfxI64;
typedef void* mfxHDL;
typedef mfxHDL mfxMemId;

typedef enum {
  MFX_ERR_NONE = 0,
  MFX_ERR_UNSUPPORTED = -3,
  MFX_ERR_MEMORY_ALLOC = -4,
  MFX_ERR_MORE_DATA = -10,
  MFX_ERR_MORE_SURFACE = -11,
  MFX_ERR_DEVICE_FAILED = -17,
  MFX_ERR_REALLOC_SURFACE = -22,
  MFX_WRN_DEVICE_BUSY = 2,
  MFX_WRN_VIDEO_PARAM_CHANGED = 3,
  MFX_ERR_NONE_PARTIAL_OUTPUT = 12,
} mfxStatus;

#endif  // MFX_STUB_INCLUDE_MFXDEFS_H_
