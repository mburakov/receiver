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

#include <errno.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/utils/result.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atomic_queue.h"
#include "toolbox/utils.h"

struct AudioContext {
  size_t sample_rate;
  size_t audio_stride;
  struct AtomicQueue queue;
  struct pw_thread_loop* pw_thread_loop;
  struct pw_stream* pw_stream;

  size_t queue_samples_sum;
  size_t queue_samples_count;
};

static bool LookupChannel(const char* name, uint32_t* value) {
  struct {
    const char* name;
    enum spa_audio_channel value;
  } static const kChannelMap[] = {
#define _(op) {.name = #op, .value = SPA_AUDIO_CHANNEL_##op}
      _(FL),  _(FR),   _(FC),   _(LFE),  _(SL),  _(SR),   _(FLC),
      _(FRC), _(RC),   _(RL),   _(RR),   _(TC),  _(TFL),  _(TFC),
      _(TFR), _(TRL),  _(TRC),  _(TRR),  _(RLC), _(RRC),  _(FLW),
      _(FRW), _(LFE2), _(FLH),  _(FCH),  _(FRH), _(TFLC), _(TFRC),
      _(TSL), _(TSR),  _(LLFE), _(RLFE), _(BC),  _(BLC),  _(BRC),
#undef _
  };
  for (size_t i = 0; i < LENGTH(kChannelMap); i++) {
    if (!strcmp(kChannelMap[i].name, name)) {
      if (value) *value = kChannelMap[i].value;
      return true;
    }
  }
  return false;
}

static size_t ParseChannelMap(
    const char* channel_map,
    uint32_t channel_positions[SPA_AUDIO_MAX_CHANNELS]) {
  char minibuf[5];
  size_t channels_counter = 0;
  for (size_t i = 0, j = 0;; i++) {
    switch (channel_map[i]) {
      case 0:
      case ',':
        minibuf[j] = 0;
        if (channels_counter == SPA_AUDIO_MAX_CHANNELS ||
            !LookupChannel(minibuf, &channel_positions[channels_counter++]))
          return 0;
        if (!channel_map[i]) return channels_counter;
        j = 0;
        break;
      default:
        if (j == 4) return 0;
        minibuf[j++] = channel_map[i];
        break;
    }
  }
}

static bool ParseAudioConfig(const char* audio_config,
                             struct spa_audio_info_raw* out_audio_info) {
  int sample_rate = atoi(audio_config);
  if (sample_rate != 44100 && sample_rate != 48000) {
    LOG("Invalid sample rate requested");
    return false;
  }
  const char* channel_map = strchr(audio_config, ':');
  if (!channel_map) {
    LOG("Invalid audio config requested");
    return false;
  }

  channel_map++;
  struct spa_audio_info_raw audio_info = {
      .format = SPA_AUDIO_FORMAT_S16_LE,
      .rate = (uint32_t)sample_rate,
  };
  audio_info.channels =
      (uint32_t)ParseChannelMap(channel_map, audio_info.position);
  if (!audio_info.channels) {
    LOG("Invalid channel map requested");
    return false;
  }

  *out_audio_info = audio_info;
  return true;
}

static void OnStreamProcess(void* data) {
  struct AudioContext* audio_context = data;
  struct pw_buffer* pw_buffer =
      pw_stream_dequeue_buffer(audio_context->pw_stream);
  if (!pw_buffer) {
    LOG("Failed to dequeue stream buffer");
    return;
  }

  struct spa_data* spa_data = &pw_buffer->buffer->datas[0];
  size_t requested = MIN(pw_buffer->requested,
                         spa_data->maxsize / audio_context->audio_stride) *
                     audio_context->audio_stride;
  size_t available =
      AtomicQueueRead(&audio_context->queue, spa_data->data, requested);

  if (available < requested) {
    // LOG("Audio queue underflow (%zu < %zu)!", available, requested);
    memset((uint8_t*)spa_data->data + available, 0, requested - available);
  }

  spa_data->chunk->offset = 0;
  spa_data->chunk->stride = (int32_t)audio_context->audio_stride;
  spa_data->chunk->size = (uint32_t)requested;
  pw_stream_queue_buffer(audio_context->pw_stream, pw_buffer);
  return;
}

struct AudioContext* AudioContextCreate(size_t queue_size,
                                        const char* audio_config) {
  LOG("Audio config is \"%s\"", audio_config);

  struct spa_audio_info_raw audio_info;
  if (!ParseAudioConfig(audio_config, &audio_info)) {
    LOG("Failed to parse audio config argument");
    return NULL;
  }

  pw_init(0, NULL);
  struct AudioContext* audio_context = malloc(sizeof(struct AudioContext));
  if (!audio_context) {
    LOG("Failed to allocate audio context (%s)", strerror(errno));
    return NULL;
  }

  audio_context->sample_rate = audio_info.rate;
  audio_context->audio_stride = audio_info.channels * sizeof(int16_t);
  if (!AtomicQueueCreate(&audio_context->queue,
                         queue_size * audio_context->audio_stride)) {
    LOG("Failed to create buffer queue (%s)", strerror(errno));
    goto rollback_audio_context;
  }

  audio_context->pw_thread_loop = pw_thread_loop_new("audio-playback", NULL);
  if (!audio_context->pw_thread_loop) {
    LOG("Failed to create pipewire thread loop");
    goto rollback_queue;
  }

  pw_thread_loop_lock(audio_context->pw_thread_loop);
  int err = pw_thread_loop_start(audio_context->pw_thread_loop);
  if (err) {
    LOG("Failed to start pipewire thread loop (%s)", spa_strerror(err));
    pw_thread_loop_unlock(audio_context->pw_thread_loop);
    goto rollback_thread_loop;
  }

  struct pw_properties* pw_properties = pw_properties_new(
#define _(...) __VA_ARGS__
      _(PW_KEY_MEDIA_TYPE, "Audio"), _(PW_KEY_MEDIA_CATEGORY, "Playback"),
      _(PW_KEY_MEDIA_ROLE, "Game"), NULL
#undef _
  );
  if (!pw_properties) {
    LOG("Failed to create pipewire properties");
    pw_thread_loop_unlock(audio_context->pw_thread_loop);
    goto rollback_thread_loop;
  }

  pw_properties_setf(pw_properties, PW_KEY_NODE_LATENCY, "128/%du",
                     audio_info.rate);
  static const struct pw_stream_events kPwStreamEvents = {
      .version = PW_VERSION_STREAM_EVENTS,
      .process = OnStreamProcess,
  };
  audio_context->pw_stream = pw_stream_new_simple(
      pw_thread_loop_get_loop(audio_context->pw_thread_loop), "audio-playback",
      pw_properties, &kPwStreamEvents, audio_context);
  if (!audio_context->pw_stream) {
    LOG("Failed to create pipewire stream");
    pw_thread_loop_unlock(audio_context->pw_thread_loop);
    goto rollback_thread_loop;
  }

  uint8_t buffer[1024];
  struct spa_pod_builder spa_pod_builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod* params[] = {spa_format_audio_raw_build(
      &spa_pod_builder, SPA_PARAM_EnumFormat, &audio_info)};
  static const enum pw_stream_flags kPwStreamFlags =
      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
      PW_STREAM_FLAG_RT_PROCESS;
  if (pw_stream_connect(audio_context->pw_stream, PW_DIRECTION_OUTPUT,
                        PW_ID_ANY, kPwStreamFlags, params, LENGTH(params))) {
    LOG("Failed to connect pipewire stream");
    pw_stream_destroy(audio_context->pw_stream);
    pw_thread_loop_unlock(audio_context->pw_thread_loop);
    goto rollback_thread_loop;
  }

  audio_context->queue_samples_sum = 0;
  audio_context->queue_samples_count = 0;
  pw_thread_loop_unlock(audio_context->pw_thread_loop);
  return audio_context;

rollback_thread_loop:
  pw_thread_loop_destroy(audio_context->pw_thread_loop);
rollback_queue:
  AtomicQueueDestroy(&audio_context->queue);
rollback_audio_context:
  free(audio_context);
  pw_deinit();
  return NULL;
}

bool AudioContextDecode(struct AudioContext* audio_context, const void* buffer,
                        size_t size) {
  if (AtomicQueueWrite(&audio_context->queue, buffer, size) < size)
    LOG("Audio queue overflow!");
  size_t queue_size =
      atomic_load_explicit(&audio_context->queue.size, memory_order_relaxed);
  audio_context->queue_samples_sum += queue_size / audio_context->audio_stride;
  audio_context->queue_samples_count++;
  return true;
}

uint64_t AudioContextGetLatency(struct AudioContext* audio_context) {
  size_t queue_latency = 0;
  if (audio_context->queue_samples_count) {
    queue_latency =
        audio_context->queue_samples_sum / audio_context->queue_samples_count;
    audio_context->queue_samples_sum = 0;
    audio_context->queue_samples_count = 0;
  }
  // TODO(mburakov): This number is extremely optimistic, i.e. Bluetooth delays
  // are not accounted for. Is it anyhow possible to get this information?
  return (128 + queue_latency) * 1000000 / audio_context->sample_rate;
}

void AudioContextDestroy(struct AudioContext* audio_context) {
  pw_thread_loop_lock(audio_context->pw_thread_loop);
  pw_stream_destroy(audio_context->pw_stream);
  pw_thread_loop_unlock(audio_context->pw_thread_loop);
  pw_thread_loop_destroy(audio_context->pw_thread_loop);
  AtomicQueueDestroy(&audio_context->queue);
  free(audio_context);
  pw_deinit();
}
