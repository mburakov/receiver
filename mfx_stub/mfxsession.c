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

#include <assert.h>
#include <mfxsession.h>
#include <stdlib.h>

#include "mfxsession_impl.h"

mfxStatus MFXInit(mfxIMPL impl, mfxVersion* ver, mfxSession* session) {
  (void)impl;
  (void)ver;
  mfxSession result = calloc(1, sizeof(struct _mfxSession));
  if (!result) return MFX_ERR_MEMORY_ALLOC;
  *result = (struct _mfxSession){
      .config_id = VA_INVALID_ID,
      .context_id = VA_INVALID_ID,
  };
  *session = result;
  return MFX_ERR_NONE;
}

mfxStatus MFXClose(mfxSession session) {
  if (session->mids) {
    for (size_t i = 0; i < session->mids_count; i++) {
      mfxFrameAllocResponse response = {
          .mids = session->mids,
          .NumFrameActual = (mfxU16)session->mids_count,
      };
      assert(session->allocator.Free(session->allocator.pthis, &response) ==
             MFX_ERR_NONE);
    }
    free(session->mids);
  }
  if (session->context_id != VA_INVALID_ID) {
    assert(vaDestroyContext(session->display, session->context_id) ==
           VA_STATUS_SUCCESS);
  }
  if (session->config_id != VA_INVALID_ID) {
    assert(vaDestroyConfig(session->display, session->config_id) ==
           VA_STATUS_SUCCESS);
  }
  free(session);
  return MFX_ERR_NONE;
}
