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

#ifndef MFX_STUB_INCLUDE_MFXVIDEO_H_
#define MFX_STUB_INCLUDE_MFXVIDEO_H_

#include "mfxsession.h"
#include "mfxstructures.h"

typedef struct {
  mfxHDL pthis;
  mfxStatus (*Alloc)(mfxHDL, mfxFrameAllocRequest*, mfxFrameAllocResponse*);
  mfxStatus (*GetHDL)(mfxHDL, mfxMemId, mfxHDL*);
  mfxStatus (*Free)(mfxHDL, mfxFrameAllocResponse*);
} mfxFrameAllocator;

mfxStatus MFXVideoCORE_SetFrameAllocator(mfxSession session,
                                         mfxFrameAllocator* allocator);
mfxStatus MFXVideoCORE_SetHandle(mfxSession session, mfxHandleType type,
                                 mfxHDL hdl);
mfxStatus MFXVideoCORE_SyncOperation(mfxSession session, mfxSyncPoint syncp,
                                     mfxU32 wait);

mfxStatus MFXVideoDECODE_Query(mfxSession session, mfxVideoParam* in,
                               mfxVideoParam* out);
mfxStatus MFXVideoDECODE_DecodeHeader(mfxSession session, mfxBitstream* bs,
                                      mfxVideoParam* par);
mfxStatus MFXVideoDECODE_Init(mfxSession session, mfxVideoParam* par);
mfxStatus MFXVideoDECODE_DecodeFrameAsync(mfxSession session, mfxBitstream* bs,
                                          mfxFrameSurface1* surface_work,
                                          mfxFrameSurface1** surface_out,
                                          mfxSyncPoint* syncp);

#endif  // MFX_STUB_INCLUDE_MFXVIDEO_H_
