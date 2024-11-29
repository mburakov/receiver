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

#ifndef MFX_STUB_INCLUDE_MFXSESSION_H_
#define MFX_STUB_INCLUDE_MFXSESSION_H_

#include "mfxcommon.h"

typedef struct _mfxSession* mfxSession;
mfxStatus MFXInit(mfxIMPL impl, mfxVersion* ver, mfxSession* session);
mfxStatus MFXClose(mfxSession session);

#endif  // MFX_STUB_INCLUDE_MFXSESSION_H_
