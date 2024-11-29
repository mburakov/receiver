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

#ifndef MFX_STUB_INCLUDE_MFXSTRUCTURES_H_
#define MFX_STUB_INCLUDE_MFXSTRUCTURES_H_

#include "mfxcommon.h"

typedef struct {
  mfxU32 FourCC;
  mfxU16 Width;
  mfxU16 Height;
  mfxU16 CropX;
  mfxU16 CropY;
  mfxU16 CropW;
  mfxU16 CropH;
  mfxU16 ChromaFormat;
} mfxFrameInfo;

enum {
  MFX_FOURCC_NV12 = MFX_MAKEFOURCC('N', 'V', '1', '2'),
};

enum {
  MFX_CHROMAFORMAT_YUV420 = 1,
};

enum {
  MFX_TIMESTAMP_UNKNOWN = -1,
};

typedef struct {
  mfxFrameInfo Info;
  struct {
    mfxMemId MemId;
  } Data;
} mfxFrameSurface1;

typedef struct {
  mfxU16 AsyncDepth;
  struct {
    mfxU32 CodecId;
    mfxU16 DecodedOrder;
  } mfx;
  mfxU16 IOPattern;
} mfxVideoParam;

enum {
  MFX_IOPATTERN_OUT_VIDEO_MEMORY = 0x10,
};

enum {
  MFX_CODEC_HEVC = MFX_MAKEFOURCC('H', 'E', 'V', 'C'),
};

enum {
  MFX_BITSTREAM_COMPLETE_FRAME = 0x0001,
};

typedef struct {
  mfxU32 AllocId;
  mfxU16 NumFrameSuggested;
  mfxFrameInfo Info;
} mfxFrameAllocRequest;

typedef struct {
  mfxU32 AllocId;
  mfxMemId* mids;
  mfxU16 NumFrameActual;
} mfxFrameAllocResponse;

typedef enum {
  MFX_HANDLE_VA_DISPLAY = 4,
} mfxHandleType;

#endif  // MFX_STUB_INCLUDE_MFXSTRUCTURES_H_
