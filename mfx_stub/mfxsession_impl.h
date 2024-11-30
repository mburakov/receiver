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

#ifndef MFX_STUB_MFXSESSION_IMPL_H_
#define MFX_STUB_MFXSESSION_IMPL_H_

#include <stddef.h>
#include <va/va.h>

#include "mfxvideo.h"

struct _mfxSession {
  mfxFrameAllocator allocator;
  VADisplay display;

  VAConfigID config_id;
  VAContextID context_id;
  mfxMemId* mids;
  size_t mids_count;

  mfxU16 crop_rect[4];
  VAPictureParameterBufferHEVC ppb;
  VASliceParameterBufferHEVC spb;
  size_t global_frame_counter;
  size_t local_frame_counter;
};

#endif  // MFX_STUB_MFXSESSION_IMPL_H_
