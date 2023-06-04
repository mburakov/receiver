/*
 * Copyright (C) 2023 Mikhail Burakov. This file is part of receiver.
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

#include "decode.h"

#include <errno.h>
#include <fcntl.h>
#include <mfxvideo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include "frame.h"
#include "toolbox/buffer.h"
#include "toolbox/utils.h"
#include "window.h"

struct Surface {
  mfxFrameInfo mfx_frame_info;
  VASurfaceID va_surface_id;
  int dmabuf_fds[4];
  bool locked;
};

struct DecodeContext {
  struct Window* window;
  mfxFrameAllocator allocator;

  int drm_fd;
  VADisplay va_display;
  mfxSession mfx_session;

  struct Buffer buffer;
  struct Surface** surfaces;

  size_t bitrate;
};

static const char* VaStatusString(VAStatus status) {
  static const char* va_status_strings[] = {
      "VA_STATUS_SUCCESS",
      "VA_STATUS_ERROR_OPERATION_FAILED",
      "VA_STATUS_ERROR_ALLOCATION_FAILED",
      "VA_STATUS_ERROR_INVALID_DISPLAY",
      "VA_STATUS_ERROR_INVALID_CONFIG",
      "VA_STATUS_ERROR_INVALID_CONTEXT",
      "VA_STATUS_ERROR_INVALID_SURFACE",
      "VA_STATUS_ERROR_INVALID_BUFFER",
      "VA_STATUS_ERROR_INVALID_IMAGE",
      "VA_STATUS_ERROR_INVALID_SUBPICTURE",
      "VA_STATUS_ERROR_ATTR_NOT_SUPPORTED",
      "VA_STATUS_ERROR_MAX_NUM_EXCEEDED",
      "VA_STATUS_ERROR_UNSUPPORTED_PROFILE",
      "VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT",
      "VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT",
      "VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE",
      "VA_STATUS_ERROR_SURFACE_BUSY",
      "VA_STATUS_ERROR_FLAG_NOT_SUPPORTED",
      "VA_STATUS_ERROR_INVALID_PARAMETER",
      "VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED",
      "VA_STATUS_ERROR_UNIMPLEMENTED",
      "VA_STATUS_ERROR_SURFACE_IN_DISPLAYING",
      "VA_STATUS_ERROR_INVALID_IMAGE_FORMAT",
      "VA_STATUS_ERROR_DECODING_ERROR",
      "VA_STATUS_ERROR_ENCODING_ERROR",
      "VA_STATUS_ERROR_INVALID_VALUE",
      "???",
      "???",
      "???",
      "???",
      "???",
      "???",
      "VA_STATUS_ERROR_UNSUPPORTED_FILTER",
      "VA_STATUS_ERROR_INVALID_FILTER_CHAIN",
      "VA_STATUS_ERROR_HW_BUSY",
      "???",
      "VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE",
      "VA_STATUS_ERROR_NOT_ENOUGH_BUFFER",
      "VA_STATUS_ERROR_TIMEDOUT",
  };
  return (VA_STATUS_SUCCESS <= status && status <= VA_STATUS_ERROR_TIMEDOUT)
             ? va_status_strings[status - VA_STATUS_SUCCESS]
             : "???";
}

static struct Surface* SurfaceCreate(const mfxFrameInfo* mfx_frame_info,
                                     VADisplay va_display,
                                     struct Frame* out_frame) {
  struct Surface* surface = malloc(sizeof(struct Surface));
  if (!surface) {
    LOG("Failed to allocate surface (%s)", strerror(errno));
    return NULL;
  }
  *surface = (struct Surface){
      .mfx_frame_info = *mfx_frame_info,
      .dmabuf_fds = {-1, -1, -1, -1},
  };

  VASurfaceAttrib attrib_list[] = {
      {.type = VASurfaceAttribPixelFormat,
       .value.type = VAGenericValueTypeInteger,
       .value.value.i = VA_FOURCC_NV12},
      {.type = VASurfaceAttribUsageHint,
       .value.type = VAGenericValueTypeInteger,
       .value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_DECODER |
                        VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT},
  };
  VAStatus va_status =
      vaCreateSurfaces(va_display, VA_RT_FORMAT_YUV420, mfx_frame_info->Width,
                       mfx_frame_info->Height, &surface->va_surface_id, 1,
                       attrib_list, LENGTH(attrib_list));
  if (va_status != VA_STATUS_SUCCESS) {
    LOG("Failed to create vaapi surface (%s)", VaStatusString(va_status));
    goto rollback_surface;
  }

  VADRMPRIMESurfaceDescriptor prime;
  va_status = vaExportSurfaceHandle(
      va_display, surface->va_surface_id,
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
      VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &prime);
  if (va_status != VA_STATUS_SUCCESS) {
    LOG("Failed to export vaapi surface (%s)", VaStatusString(va_status));
    goto rollback_va_surface_id;
  }

  out_frame->width = prime.width;
  out_frame->height = prime.height;
  out_frame->fourcc = prime.fourcc;
  out_frame->nplanes = prime.layers[0].num_planes;
  for (uint32_t i = 0; i < prime.layers[0].num_planes; i++) {
    surface->dmabuf_fds[i] = prime.objects[prime.layers[0].object_index[i]].fd;
    out_frame->planes[i] = (struct FramePlane){
        .dmabuf_fd = surface->dmabuf_fds[i],
        .pitch = prime.layers[0].pitch[i],
        .offset = prime.layers[0].offset[i],
        .modifier =
            prime.objects[prime.layers[0].object_index[i]].drm_format_modifier,
    };
  }
  return surface;

rollback_va_surface_id:
  vaDestroySurfaces(va_display, &surface->va_surface_id, 1);
rollback_surface:
  free(surface);
  return NULL;
}

static void SurfaceDestroy(struct Surface* surface, VADisplay va_display) {
  for (size_t i = LENGTH(surface->dmabuf_fds); i; i--) {
    if (surface->dmabuf_fds[i - 1] != -1) close(surface->dmabuf_fds[i - 1]);
  }
  vaDestroySurfaces(va_display, &surface->va_surface_id, 1);
  free(surface);
}

static mfxStatus OnAllocatorAlloc(mfxHDL pthis, mfxFrameAllocRequest* request,
                                  mfxFrameAllocResponse* response) {
  LOG("%s(AllocId=%u, NumFrameSuggested=%u)", __func__, request->AllocId,
      request->NumFrameSuggested);
  if (request->Info.FourCC != MFX_FOURCC_NV12) {
    LOG("Allocation of %.4s surfaces is not supported",
        (const char*)&request->Info.FourCC);
    return MFX_ERR_UNSUPPORTED;
  }
  if (request->Info.ChromaFormat != MFX_CHROMAFORMAT_YUV420) {
    LOG("Chroma format %u is not supported", request->Info.ChromaFormat);
    return MFX_ERR_UNSUPPORTED;
  }

  struct DecodeContext* decode_context = pthis;
  decode_context->surfaces =
      calloc(request->NumFrameSuggested + 1, sizeof(struct Surface*));
  if (!decode_context->surfaces) {
    LOG("Failed to allocate surfaces storage (%s)", strerror(errno));
    return MFX_ERR_MEMORY_ALLOC;
  }

  struct Frame frames[request->NumFrameSuggested];
  for (size_t i = 0; i < request->NumFrameSuggested; i++) {
    decode_context->surfaces[i] =
        SurfaceCreate(&request->Info, decode_context->va_display, &frames[i]);
    if (!decode_context->surfaces[i]) {
      LOG("Failed to create surface");
      goto rollback_surfaces;
    }
  }

  if (!WindowAssignFrames(decode_context->window, request->NumFrameSuggested,
                          frames)) {
    LOG("Failed to assign frames to window");
    goto rollback_surfaces;
  }

  *response = (mfxFrameAllocResponse){
      .AllocId = request->AllocId,
      .mids = (void**)decode_context->surfaces,
      .NumFrameActual = request->NumFrameSuggested,
  };
  return MFX_ERR_NONE;

rollback_surfaces:
  for (size_t i = request->NumFrameSuggested; i; i--) {
    if (decode_context->surfaces[i - 1])
      SurfaceDestroy(decode_context->surfaces[i - 1],
                     decode_context->va_display);
  }
  free(decode_context->surfaces);
  return MFX_ERR_MEMORY_ALLOC;
}

static mfxStatus OnAllocatorGetHDL(mfxHDL pthis, mfxMemId mid, mfxHDL* handle) {
  (void)pthis;
  struct Surface* surface = mid;
  *handle = &surface->va_surface_id;
  return MFX_ERR_NONE;
}

static mfxStatus OnAllocatorFree(mfxHDL pthis,
                                 mfxFrameAllocResponse* response) {
  LOG("%s(AllocId=%u)", __func__, response->AllocId);
  struct DecodeContext* decode_context = pthis;
  for (size_t i = response->NumFrameActual; i; i--)
    SurfaceDestroy(decode_context->surfaces[i - 1], decode_context->va_display);
  free(decode_context->surfaces);
  return MFX_ERR_NONE;
}

static const char* MfxStatusString(mfxStatus status) {
  static const char* mfx_status_strings[] = {
      "MFX_ERR_REALLOC_SURFACE",
      "MFX_ERR_GPU_HANG",
      "MFX_ERR_INVALID_AUDIO_PARAM",
      "MFX_ERR_INCOMPATIBLE_AUDIO_PARAM",
      "MFX_ERR_MORE_BITSTREAM",
      "MFX_ERR_DEVICE_FAILED",
      "MFX_ERR_UNDEFINED_BEHAVIOR",
      "MFX_ERR_INVALID_VIDEO_PARAM",
      "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM",
      "MFX_ERR_DEVICE_LOST",
      "MFX_ERR_ABORTED",
      "MFX_ERR_MORE_SURFACE",
      "MFX_ERR_MORE_DATA",
      "MFX_ERR_NOT_FOUND",
      "MFX_ERR_NOT_INITIALIZED",
      "MFX_ERR_LOCK_MEMORY",
      "MFX_ERR_INVALID_HANDLE",
      "MFX_ERR_NOT_ENOUGH_BUFFER",
      "MFX_ERR_MEMORY_ALLOC",
      "MFX_ERR_UNSUPPORTED",
      "MFX_ERR_NULL_PTR",
      "MFX_ERR_UNKNOWN",
      "MFX_ERR_NONE",
      "MFX_WRN_IN_EXECUTION",
      "MFX_WRN_DEVICE_BUSY",
      "MFX_WRN_VIDEO_PARAM_CHANGED",
      "MFX_WRN_PARTIAL_ACCELERATION",
      "MFX_WRN_INCOMPATIBLE_VIDEO_PARAM",
      "MFX_WRN_VALUE_NOT_CHANGED",
      "MFX_WRN_OUT_OF_RANGE",
      "MFX_TASK_WORKING",
      "MFX_TASK_BUSY",
      "MFX_WRN_FILTER_SKIPPED",
      "MFX_WRN_INCOMPATIBLE_AUDIO_PARAM",
      "MFX_ERR_NONE_PARTIAL_OUTPUT",
  };
  return (MFX_ERR_REALLOC_SURFACE <= status &&
          status <= MFX_ERR_NONE_PARTIAL_OUTPUT)
             ? mfx_status_strings[status - MFX_ERR_REALLOC_SURFACE]
             : "???";
}

struct DecodeContext* DecodeContextCreate(struct Window* window) {
  struct DecodeContext* decode_context = malloc(sizeof(struct DecodeContext));
  if (!decode_context) {
    LOG("Failed to allocate decode context (%s)", strerror(errno));
    return NULL;
  }
  *decode_context = (struct DecodeContext){
      .window = window,
      .allocator.pthis = decode_context,
      .allocator.Alloc = OnAllocatorAlloc,
      .allocator.GetHDL = OnAllocatorGetHDL,
      .allocator.Free = OnAllocatorFree,
  };

  decode_context->drm_fd = open("/dev/dri/renderD128", O_RDWR);
  if (decode_context->drm_fd == -1) {
    LOG("Failed to open render node (%s)", strerror(errno));
    goto rollback_decode_context;
  }

  decode_context->va_display = vaGetDisplayDRM(decode_context->drm_fd);
  if (!decode_context->va_display) {
    LOG("Failed to get vaapi display (%s)", strerror(errno));
    goto rollback_drm_fd;
  }
  int major, minor;
  VAStatus va_status = vaInitialize(decode_context->va_display, &major, &minor);
  if (va_status != VA_STATUS_SUCCESS) {
    LOG("Failed to init vaapi (%s)", VaStatusString(va_status));
    goto rollback_display;
  }

  LOG("Initialized vaapi %d.%d", major, minor);
  mfxStatus mfx_status =
      MFXInit(MFX_IMPL_HARDWARE, NULL, &decode_context->mfx_session);
  if (mfx_status != MFX_ERR_NONE) {
    LOG("Failed to init mfx session (%s)", MfxStatusString(mfx_status));
    goto rollback_display;
  }
  mfx_status =
      MFXVideoCORE_SetHandle(decode_context->mfx_session, MFX_HANDLE_VA_DISPLAY,
                             decode_context->va_display);
  if (mfx_status != MFX_ERR_NONE) {
    LOG("Failed to set mfx session display (%s)", MfxStatusString(mfx_status));
    goto rollback_session;
  }
  mfx_status = MFXVideoCORE_SetFrameAllocator(decode_context->mfx_session,
                                              &decode_context->allocator);
  if (mfx_status != MFX_ERR_NONE) {
    LOG("Failed to set frame allocator (%s)", MfxStatusString(mfx_status));
    goto rollback_session;
  }
  return decode_context;

rollback_session:
  MFXClose(decode_context->mfx_session);
rollback_display:
  vaTerminate(decode_context->va_display);
rollback_drm_fd:
  close(decode_context->drm_fd);
rollback_decode_context:
  free(decode_context);
  return NULL;
}

static bool InitializeDecoder(struct DecodeContext* decode_context,
                              mfxBitstream* bitstream) {
  mfxVideoParam video_param = {
      .mfx.CodecId = MFX_CODEC_HEVC,
  };
  mfxStatus mfx_status = MFXVideoDECODE_DecodeHeader(
      decode_context->mfx_session, bitstream, &video_param);
  switch (mfx_status) {
    case MFX_ERR_NONE:
      break;
    case MFX_ERR_MORE_DATA:
      return true;
    default:
      LOG("Failed to decode header (%s)", MfxStatusString(mfx_status));
      return false;
  }

  video_param.AsyncDepth = 1;
  video_param.mfx.DecodedOrder = 1;
  video_param.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
  mfx_status = MFXVideoDECODE_Query(decode_context->mfx_session, &video_param,
                                    &video_param);
  if (mfx_status != MFX_ERR_NONE) {
    LOG("Failed to query decode (%s)", MfxStatusString(mfx_status));
    return false;
  }

  mfx_status = MFXVideoDECODE_Init(decode_context->mfx_session, &video_param);
  if (mfx_status != MFX_ERR_NONE) {
    LOG("Failed to init decode (%s)", MfxStatusString(mfx_status));
    return false;
  }
  return true;
}

static struct Surface* GetFreeSurface(struct DecodeContext* decode_context) {
  struct Surface** psurface = decode_context->surfaces;
  for (; *psurface && (*psurface)->locked; psurface++)
    ;
  (*psurface)->locked = true;
  return *psurface;
}

static size_t UnlockAllSurfaces(struct DecodeContext* decode_context,
                                const struct Surface* keep_locked) {
  size_t result = 0;
  for (size_t i = 0; decode_context->surfaces[i]; i++) {
    if (decode_context->surfaces[i] != keep_locked) {
      decode_context->surfaces[i]->locked = false;
    } else {
      result = i;
    }
  }
  return result;
}

bool DecodeContextDecode(struct DecodeContext* decode_context, int fd) {
  switch (BufferAppendFrom(&decode_context->buffer, fd)) {
    case -1:
      LOG("Failed to append packet data to buffer (%s)", strerror(errno));
      return false;
    case 0:
      LOG("Server closed connection");
      return false;
    default:
      break;
  }

again:
  if (decode_context->buffer.size < sizeof(uint32_t)) {
    // mburakov: Packet size is not yet available.
    return true;
  }
  uint32_t packet_size = *(uint32_t*)decode_context->buffer.data;
  if (decode_context->buffer.size < sizeof(uint32_t) + packet_size) {
    // mburakov: Full packet is not yet available.
    return true;
  }

  mfxBitstream bitstream = {
      .DecodeTimeStamp = MFX_TIMESTAMP_UNKNOWN,
      .TimeStamp = (mfxU64)MFX_TIMESTAMP_UNKNOWN,
      .Data = (mfxU8*)decode_context->buffer.data + sizeof(uint32_t),
      .DataLength = packet_size,
      .MaxLength = packet_size,
      .DataFlag = MFX_BITSTREAM_COMPLETE_FRAME,
  };

  if (!decode_context->surfaces) {
    if (!InitializeDecoder(decode_context, &bitstream)) {
      LOG("Failed to initialize decoder");
      return false;
    }
    if (!decode_context->surfaces) {
      // mburakov: Initialization might be postponed.
      return true;
    }
  }

  for (;;) {
    struct Surface* surface = GetFreeSurface(decode_context);
    mfxFrameSurface1 surface_work = {
        .Info = surface->mfx_frame_info,
        .Data.MemId = surface,
    };
    mfxFrameSurface1* surface_out = NULL;
    mfxSyncPoint sync = NULL;
    mfxStatus mfx_status =
        MFXVideoDECODE_DecodeFrameAsync(decode_context->mfx_session, &bitstream,
                                        &surface_work, &surface_out, &sync);
    switch (mfx_status) {
      case MFX_ERR_MORE_SURFACE:
        continue;
      case MFX_ERR_NONE:
        break;
      case MFX_WRN_DEVICE_BUSY:
        usleep(500);
        __attribute__((fallthrough));
      case MFX_WRN_VIDEO_PARAM_CHANGED:
        continue;
      default:
        LOG("Failed to decode frame (%s)", MfxStatusString(mfx_status));
        return false;
    }

    mfx_status = MFXVideoCORE_SyncOperation(decode_context->mfx_session, sync,
                                            MFX_INFINITE);
    if (mfx_status != MFX_ERR_NONE) {
      LOG("Failed to sync operation (%s)", MfxStatusString(mfx_status));
      return false;
    }

    size_t locked = UnlockAllSurfaces(decode_context, surface_out->Data.MemId);
    if (!WindowShowFrame(decode_context->window, locked)) {
      LOG("Failed to show frame");
      return false;
    }

    BufferDiscard(&decode_context->buffer, sizeof(uint32_t) + packet_size);
    decode_context->bitrate += (sizeof(uint32_t) + packet_size) * 8;
    goto again;
  }
}

void DecodeContextGetStats(struct DecodeContext* decode_context,
                           struct DecodeStats* decode_stats) {
  decode_stats->bitrate = decode_context->bitrate;
  decode_context->bitrate = 0;
}

void DecodeContextDestroy(struct DecodeContext* decode_context) {
  BufferDestroy(&decode_context->buffer);
  MFXClose(decode_context->mfx_session);
  vaTerminate(decode_context->va_display);
  close(decode_context->drm_fd);
  free(decode_context);
}
