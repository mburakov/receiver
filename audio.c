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

#include "audio.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>

#include "atomic_queue.h"
#include "toolbox/utils.h"

struct AudioContext {
  const char* device;
  atomic_bool running;
  struct AtomicQueue queue;
  atomic_uint_fast64_t latency;
  thrd_t thread;
};

static int AudioContextThreadProc(void* arg) {
  struct AudioContext* context = arg;

  snd_pcm_t* pcm = NULL;
  int err = snd_pcm_open(&pcm, context->device, SND_PCM_STREAM_PLAYBACK, 0);
  if (err) {
    LOG("Failed to open pcm (%s)", snd_strerror(err));
    atomic_store_explicit(&context->running, 0, memory_order_relaxed);
    return 0;
  }

  // TODO(mburakov): Read audio configuration from the server.
  err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 10000);
  if (err) {
    LOG("Failed to set pcm params (%s)", snd_strerror(err));
    atomic_store_explicit(&context->running, 0, memory_order_relaxed);
    goto rollback_pcm;
  }

  while (atomic_load_explicit(&context->running, memory_order_relaxed)) {
    // TODO(mburakov): Frame size depends on dynamic audio configuration.
    static const unsigned frame_size = sizeof(int16_t) * 2;
    uint8_t buffer[480 * frame_size];

    size_t size = AtomicQueueRead(&context->queue, buffer, sizeof(buffer));
    if (size < sizeof(buffer)) {
      // LOG("Audio queue underflow!");
      memset(buffer + size, 0, sizeof(buffer) - size);
      uint64_t micros = (sizeof(buffer) - size) * 1000 / frame_size / 48;
      atomic_fetch_add_explicit(&context->latency, micros,
                                memory_order_relaxed);
    }

    for (snd_pcm_uframes_t offset = 0; offset < sizeof(buffer) / frame_size;) {
      snd_pcm_sframes_t nframes =
          snd_pcm_writei(pcm, buffer + offset * frame_size,
                         sizeof(buffer) / frame_size - offset);
      if (nframes < 0) {
        LOG("Failed to write pcm (%s)", snd_strerror((int)nframes));
        atomic_store_explicit(&context->running, 0, memory_order_relaxed);
        goto rollback_pcm;
      }
      offset += (snd_pcm_uframes_t)nframes;
    }
  }

rollback_pcm:
  snd_pcm_close(pcm);
  return 0;
}

struct AudioContext* AudioContextCreate(const char* device) {
  struct AudioContext* audio_context = malloc(sizeof(struct AudioContext));
  if (!audio_context) {
    LOG("Failed to allocate context (%s)", strerror(errno));
    return NULL;
  }

  audio_context->device = device;
  atomic_init(&audio_context->running, 1);
  if (!AtomicQueueCreate(&audio_context->queue, 4800 * sizeof(int16_t) * 2)) {
    LOG("Failed to create queue (%s)", strerror(errno));
    goto rollback_context;
  }

  if (thrd_create(&audio_context->thread, AudioContextThreadProc,
                  audio_context) != thrd_success) {
    LOG("Failed to create thread (%s)", strerror(errno));
    goto rollback_queue;
  }
  return audio_context;

rollback_queue:
  AtomicQueueDestroy(&audio_context->queue);
rollback_context:
  free(audio_context);
  return NULL;
}

bool AudioContextDecode(struct AudioContext* audio_context, const void* buffer,
                        size_t size) {
  if (!atomic_load_explicit(&audio_context->running, memory_order_relaxed)) {
    LOG("Audio thread was stopped early!");
    return false;
  }
  if (AtomicQueueWrite(&audio_context->queue, buffer, size) < size)
    LOG("Audio queue overflow!");
  return true;
}

uint64_t AudioContextGetLatency(const struct AudioContext* audio_context) {
  return atomic_load_explicit(&audio_context->latency, memory_order_relaxed);
}

void AudioContextDestroy(struct AudioContext* audio_context) {
  atomic_store_explicit(&audio_context->running, 0, memory_order_relaxed);
  thrd_join(audio_context->thread, NULL);
  AtomicQueueDestroy(&audio_context->queue);
  free(audio_context);
}
