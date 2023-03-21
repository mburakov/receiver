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
#include <limits.h>
#include <mfxvideo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include "frame.h"
#include "util.h"

struct Surface {
  VASurfaceID surface_id;
  mfxFrameInfo frame_info;
  struct Frame* frame;
  bool locked;
};

struct TimingStats {
  unsigned long long min;
  unsigned long long max;
  unsigned long long sum;
};

struct DecodeContext {
  int drm_fd;
  VADisplay display;
  mfxSession session;
  mfxFrameAllocator allocator;
  bool initialized;

  uint32_t packet_size;
  uint8_t* packet_data;
  uint32_t packet_alloc;
  uint32_t packet_offset;
  struct Surface** sufaces;
  struct Frame* decoded;

  unsigned long long recording_started;
  unsigned long long frame_header_ts;
  unsigned long long frame_received_ts;
  unsigned long long frame_decoded_ts;
  unsigned long long frame_counter;
  unsigned long long bitstream;

  struct TimingStats receive;
  struct TimingStats decode;
  struct TimingStats total;
};

static void TimingStatsReset(struct TimingStats* timing_stats) {
  *timing_stats = (struct TimingStats){.min = ULLONG_MAX};
}

static void TimingStatsRecord(struct TimingStats* timing_stats,
                              unsigned long long value) {
  timing_stats->min = MIN(timing_stats->min, value);
  timing_stats->max = MAX(timing_stats->max, value);
  timing_stats->sum += value;
}

static void TimingStatsLog(const struct TimingStats* timing_stats,
                           const char* name, unsigned long long counter) {
  LOG("%s min/avg/max: %llu/%llu/%llu", name, timing_stats->min,
      timing_stats->sum / counter, timing_stats->max);
}

static unsigned long long MicrosNow(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 0};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000000ull +
         (unsigned long long)ts.tv_nsec / 1000ull;
}

static void SurfaceDestroy(struct Surface*** psurfaces) {
  if (!psurfaces || !*psurfaces) return;
  for (struct Surface** surfaces = *psurfaces; *surfaces; surfaces++) {
    if ((*surfaces)->frame) FrameDestroy(&(*surfaces)->frame);
    free(*surfaces);
  }
  free(*psurfaces);
  *psurfaces = NULL;
}

static struct Frame* ExportFrame(VADisplay display, VASurfaceID surface_id) {
  VADRMPRIMESurfaceDescriptor prime;
  VAStatus status = vaExportSurfaceHandle(
      display, surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
      VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &prime);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to export vaapi surface (%d)", status);
    return NULL;
  }

  struct FramePlane planes[prime.layers[0].num_planes];
  for (size_t i = 0; i < LENGTH(planes); i++) {
    planes[i] = (struct FramePlane){
        .dmabuf_fd = prime.objects[prime.layers[0].object_index[i]].fd,
        .pitch = prime.layers[0].pitch[i],
        .offset = prime.layers[0].offset[i],
        .modifier =
            prime.objects[prime.layers[0].object_index[i]].drm_format_modifier,
    };
  }

  struct Frame* frame = FrameCreate(prime.width, prime.height, prime.fourcc,
                                    prime.layers[0].num_planes, planes);
  if (!frame) LOG("Failed to create frame");
  for (size_t i = prime.num_objects; i; i--) close(prime.objects[i - 1].fd);
  return frame;
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

  struct AUTO(Surface)** surfaces =
      calloc(request->NumFrameSuggested + 1, sizeof(struct Surface*));
  if (!surfaces) {
    LOG("Failed to allocate surfaces storage (%s)", strerror(errno));
    return MFX_ERR_MEMORY_ALLOC;
  }
  for (size_t i = 0; i < request->NumFrameSuggested; i++) {
    surfaces[i] = calloc(1, sizeof(struct Surface));
    if (!surfaces[i]) {
      LOG("Failed to allocate surface (%s)", strerror(errno));
      return MFX_ERR_MEMORY_ALLOC;
    }
  }

  VASurfaceID surface_ids[request->NumFrameSuggested];
  struct DecodeContext* decode_context = pthis;
  VASurfaceAttrib attrib_list[] = {
      {.type = VASurfaceAttribPixelFormat,
       .value.type = VAGenericValueTypeInteger,
       .value.value.i = VA_FOURCC_NV12},
      {.type = VASurfaceAttribUsageHint,
       .value.type = VAGenericValueTypeInteger,
       .value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_DECODER |
                        VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT},
  };
  VAStatus status = vaCreateSurfaces(
      decode_context->display, VA_RT_FORMAT_YUV420, request->Info.Width,
      request->Info.Height, surface_ids, request->NumFrameSuggested,
      attrib_list, LENGTH(attrib_list));
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to allocate surfaces (%d)", status);
    return MFX_ERR_MEMORY_ALLOC;
  }

  for (size_t i = 0; i < request->NumFrameSuggested; i++) {
    surfaces[i]->surface_id = surface_ids[i];
    surfaces[i]->frame_info = request->Info;
  }

  // mburakov: Separate loop for frames to ensure proper cleanup in destructor.
  for (size_t i = 0; i < request->NumFrameSuggested; i++) {
    surfaces[i]->frame =
        ExportFrame(decode_context->display, surfaces[i]->surface_id);
    if (!surfaces[i]->frame) {
      LOG("Failed to export frame");
      return MFX_ERR_MEMORY_ALLOC;
    }
  }

  decode_context->sufaces = RELEASE(surfaces);
  *response = (mfxFrameAllocResponse){
      .AllocId = request->AllocId,
      .mids = (void**)decode_context->sufaces,
      .NumFrameActual = request->NumFrameSuggested,
  };
  return MFX_ERR_NONE;
}

static mfxStatus OnAllocatorGetHDL(mfxHDL pthis, mfxMemId mid, mfxHDL* handle) {
  (void)pthis;
  struct Surface* surface = mid;
  *handle = &surface->surface_id;
  return MFX_ERR_NONE;
}

static mfxStatus OnAllocatorFree(mfxHDL pthis,
                                 mfxFrameAllocResponse* response) {
  LOG("%s(AllocId=%u)", __func__, response->AllocId);
  VASurfaceID surface_ids[response->NumFrameActual];
  struct Surface** surfaces = (struct Surface**)response->mids;
  for (size_t i = 0; i < response->NumFrameActual; i++)
    surface_ids[i] = surfaces[i]->surface_id;
  struct DecodeContext* decode_context = pthis;
  vaDestroySurfaces(decode_context->display, surface_ids,
                    response->NumFrameActual);
  SurfaceDestroy(&decode_context->sufaces);
  return MFX_ERR_NONE;
}

struct DecodeContext* DecodeContextCreate(void) {
  struct AUTO(DecodeContext)* decode_context =
      malloc(sizeof(struct DecodeContext));
  if (!decode_context) {
    LOG("Failed to allocate decode context (%s)", strerror(errno));
    return NULL;
  }
  *decode_context = (struct DecodeContext){
      .drm_fd = -1,
      .allocator.pthis = decode_context,
      .allocator.Alloc = OnAllocatorAlloc,
      .allocator.GetHDL = OnAllocatorGetHDL,
      .allocator.Free = OnAllocatorFree,
  };

  decode_context->drm_fd = open("/dev/dri/renderD128", O_RDWR);
  if (decode_context->drm_fd == -1) {
    LOG("Failed to open render node (%s)", strerror(errno));
    return NULL;
  }

  decode_context->display = vaGetDisplayDRM(decode_context->drm_fd);
  if (!decode_context->display) {
    LOG("Failed to get vaapi display (%s)", strerror(errno));
    return NULL;
  }
  int major, minor;
  VAStatus st = vaInitialize(decode_context->display, &major, &minor);
  if (st != VA_STATUS_SUCCESS) {
    LOG("Failed to init vaapi (%d)", st);
    return NULL;
  }

  LOG("Initialized vaapi %d.%d", major, minor);
  mfxStatus status = MFXInit(MFX_IMPL_HARDWARE, NULL, &decode_context->session);
  if (status != MFX_ERR_NONE) {
    LOG("Failed to init mfx session (%d)", status);
    return NULL;
  }
  status = MFXVideoCORE_SetHandle(
      decode_context->session, MFX_HANDLE_VA_DISPLAY, decode_context->display);
  if (status != MFX_ERR_NONE) {
    LOG("Failed to set mfx session display (%d)", status);
    return NULL;
  }
  status = MFXVideoCORE_SetFrameAllocator(decode_context->session,
                                          &decode_context->allocator);
  if (status != MFX_ERR_NONE) {
    LOG("Failed to set frame allocator (%d)", status);
    return NULL;
  }

  decode_context->recording_started = MicrosNow();
  TimingStatsReset(&decode_context->receive);
  TimingStatsReset(&decode_context->decode);
  TimingStatsReset(&decode_context->total);
  return RELEASE(decode_context);
}

static bool InitializeDecoder(struct DecodeContext* decode_context,
                              mfxBitstream* bitstream) {
  mfxVideoParam video_param = {
      .mfx.CodecId = MFX_CODEC_AVC,
  };
  mfxStatus status = MFXVideoDECODE_DecodeHeader(decode_context->session,
                                                 bitstream, &video_param);
  switch (status) {
    case MFX_ERR_NONE:
      break;
    case MFX_ERR_MORE_DATA:
      return true;
    default:
      LOG("Failed to parse decode header (%d)", status);
      return false;
  }

  video_param.AsyncDepth = 1;
  video_param.mfx.DecodedOrder = 1;
  video_param.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
  status =
      MFXVideoDECODE_Query(decode_context->session, &video_param, &video_param);
  if (status != MFX_ERR_NONE) {
    LOG("Failed to query decode (%d)", status);
    return false;
  }

  status = MFXVideoDECODE_Init(decode_context->session, &video_param);
  if (status != MFX_ERR_NONE) {
    LOG("Failed to init decode (%d)", status);
    return false;
  }
  decode_context->initialized = true;
  return true;
}

static bool ReadSomePacketData(struct DecodeContext* decode_context, int fd) {
again:;
  void* target;
  size_t size;
  if (!decode_context->packet_size) {
    target = &decode_context->packet_size;
    size = sizeof(decode_context->packet_size);
    decode_context->frame_header_ts = MicrosNow();
  } else {
    target = decode_context->packet_data + decode_context->packet_offset;
    size = decode_context->packet_size - decode_context->packet_offset;
  }
  ssize_t result = read(fd, target, size);
  switch (result) {
    case -1:
      if (errno == EINTR) goto again;
      LOG("Failed to read packet data (%s)", strerror(errno));
      return false;
    case 0:
      LOG("File descriptor was closed");
      return false;
    default:
      break;
  }
  if (target != &decode_context->packet_size) {
    decode_context->packet_offset += result;
    return true;
  }
  if (result != (ssize_t)size) {
    LOG("Failed to read complete packet size");
    return false;
  }
  if (decode_context->packet_size > decode_context->packet_alloc) {
    uint32_t packet_alloc = decode_context->packet_size;
    uint8_t* packet_data = malloc(packet_alloc);
    if (!packet_data) {
      LOG("Failed to reallocate packet data (%s)", strerror(errno));
      return false;
    }
    free(decode_context->packet_data);
    decode_context->packet_data = packet_data;
    decode_context->packet_alloc = packet_alloc;
  }
  return true;
}

static struct Surface* GetFreeSurface(struct DecodeContext* decode_context) {
  struct Surface** psurface = decode_context->sufaces;
  for (; *psurface && (*psurface)->locked; psurface++)
    ;
  (*psurface)->locked = true;
  return *psurface;
}

static void UnlockAllSurfaces(struct DecodeContext* decode_context,
                              const struct Surface* keep_locked) {
  struct Surface** psurface = decode_context->sufaces;
  for (; *psurface; psurface++) {
    if (*psurface != keep_locked) (*psurface)->locked = false;
  }
}

void HandleTimingStats(struct DecodeContext* decode_context) {
  TimingStatsRecord(
      &decode_context->receive,
      decode_context->frame_received_ts - decode_context->frame_header_ts);
  TimingStatsRecord(
      &decode_context->decode,
      decode_context->frame_decoded_ts - decode_context->frame_received_ts);
  TimingStatsRecord(
      &decode_context->total,
      decode_context->frame_received_ts - decode_context->frame_header_ts);

  unsigned long long period =
      decode_context->frame_decoded_ts - decode_context->recording_started;
  static const unsigned long long second = 1000000;
  if (period < 10 * second) return;

  LOG("---->8-------->8-------->8----");
  TimingStatsLog(&decode_context->receive, "Receive",
                 decode_context->frame_counter);
  TimingStatsLog(&decode_context->decode, "Decode",
                 decode_context->frame_counter);
  TimingStatsLog(&decode_context->total, "Total",
                 decode_context->frame_counter);
  LOG("Framerate: %llu fps", decode_context->frame_counter * second / period);
  LOG("Bitstream: %llu Kbps",
      decode_context->bitstream * second * 8 / period / 1024);
  decode_context->recording_started = decode_context->frame_decoded_ts;
  TimingStatsReset(&decode_context->receive);
  TimingStatsReset(&decode_context->decode);
  TimingStatsReset(&decode_context->total);
  decode_context->frame_counter = 0;
  decode_context->bitstream = 0;
}

bool DecodeContextDecode(struct DecodeContext* decode_context, int fd) {
  if (!ReadSomePacketData(decode_context, fd)) {
    LOG("Failed to read some packet data");
    return false;
  }

  if (decode_context->packet_size != decode_context->packet_offset) {
    // mburakov: Full frame has to be available for decoding.
    return true;
  }
  mfxBitstream bitstream = {
      .DecodeTimeStamp = MFX_TIMESTAMP_UNKNOWN,
      .TimeStamp = (mfxU64)MFX_TIMESTAMP_UNKNOWN,
      .Data = decode_context->packet_data,
      .DataLength = decode_context->packet_size,
      .MaxLength = decode_context->packet_size,
      .DataFlag = MFX_BITSTREAM_COMPLETE_FRAME,
  };
  decode_context->packet_size = 0;
  decode_context->packet_offset = 0;
  decode_context->frame_received_ts = MicrosNow();
  decode_context->bitstream += bitstream.DataLength;

  if (!decode_context->initialized) {
    if (!InitializeDecoder(decode_context, &bitstream)) {
      LOG("Failed to initialize decoder");
      return false;
    }
    // mburakov: Initialization might be postponed.
    if (!decode_context->initialized) return true;
  }

  for (;;) {
    struct Surface* surface = GetFreeSurface(decode_context);
    mfxFrameSurface1 surface_work = {
        .Info = surface->frame_info,
        .Data.MemId = surface,
    };
    mfxFrameSurface1* surface_out = NULL;
    mfxSyncPoint sync = NULL;
    mfxStatus status =
        MFXVideoDECODE_DecodeFrameAsync(decode_context->session, &bitstream,
                                        &surface_work, &surface_out, &sync);
    switch (status) {
      case MFX_ERR_MORE_SURFACE:
        continue;
      case MFX_ERR_NONE:
        break;
      case MFX_WRN_VIDEO_PARAM_CHANGED:
        continue;
      default:
        LOG("Failed to decode frame (%d)", status);
        return false;
    }

    status =
        MFXVideoCORE_SyncOperation(decode_context->session, sync, MFX_INFINITE);
    if (status != MFX_ERR_NONE) {
      LOG("Failed to sync operation (%d)", status);
      return false;
    }

    surface = surface_out->Data.MemId;
    decode_context->decoded = surface->frame;
    UnlockAllSurfaces(decode_context, surface);

    decode_context->frame_decoded_ts = MicrosNow();
    decode_context->frame_counter++;
    HandleTimingStats(decode_context);
    return true;
  }
}

const struct Frame* DecodeContextGetFrame(
    struct DecodeContext* decode_context) {
  return decode_context->decoded;
}

void DecodeContextDestroy(struct DecodeContext** decode_context) {
  if (!decode_context || !*decode_context) return;
  if ((*decode_context)->packet_data) free((*decode_context)->packet_data);
  if ((*decode_context)->session) MFXClose((*decode_context)->session);
  if ((*decode_context)->display) vaTerminate((*decode_context)->display);
  if ((*decode_context)->drm_fd) close((*decode_context)->drm_fd);
  free(*decode_context);
  *decode_context = NULL;
}
