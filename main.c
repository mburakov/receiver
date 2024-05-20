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

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "audio.h"
#include "decode.h"
#include "input.h"
#include "proto.h"
#include "pui/font.h"
#include "toolbox/buffer.h"
#include "toolbox/perf.h"
#include "toolbox/utils.h"
#include "window.h"

static volatile sig_atomic_t g_signal;
static void OnSignal(int status) { g_signal = status; }

struct Context {
  size_t audio_buffer_size;
  struct InputStream* input_stream;
  struct Window* window;
  size_t overlay_width;
  size_t overlay_height;
  struct Overlay* overlay;
  struct DecodeContext* decode_context;
  struct AudioContext* audio_context;
  struct Buffer buffer;

  size_t video_bitstream;
  size_t audio_bitstream;
  uint64_t timestamp;
  uint64_t ping_sum;
  uint64_t ping_count;
  uint64_t video_latency_sum;
  uint64_t video_latency_count;
  uint64_t audio_latency_sum;
  uint64_t audio_latency_count;
};

static int ConnectSocket(const char* arg) {
  uint16_t port;
  char ip[sizeof("xxx.xxx.xxx.xxx")];
  if (sscanf(arg, "%[0-9.]:%hu", ip, &port) != 2) {
    LOG("Failed to parse address");
    return -1;
  }

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    LOG("Failed to create socket (%s)", strerror(errno));
    return -1;
  }
  if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int))) {
    LOG("Failed to set TCP_NODELAY (%s)", strerror(errno));
    goto rollback_sock;
  }

  // TODO(mburakov): Set and maintain TCP_QUICKACK.
  const struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = inet_addr(ip),
  };
  if (connect(sock, (const struct sockaddr*)&addr, sizeof(addr))) {
    LOG("Failed to connect socket (%s)", strerror(errno));
    goto rollback_sock;
  }
  return sock;

rollback_sock:
  close(sock);
  return -1;
}

static void OnWindowClose(void* user) {
  (void)user;
  g_signal = SIGINT;
}

static void OnWindowFocus(void* user, bool focused) {
  if (focused) return;
  if (!InputStreamHandsoff(user)) {
    LOG("Failed to handle window focus");
    g_signal = SIGABRT;
  }
}

static void OnWindowKey(void* user, unsigned key, bool pressed) {
  if (!InputStreamKeyPress(user, key, pressed)) {
    LOG("Failed to handle key press");
    g_signal = SIGABRT;
  }
}

static void OnWindowMove(void* user, int dx, int dy) {
  if (!InputStreamMouseMove(user, dx, dy)) {
    LOG("Failed to handle mouse move");
    g_signal = SIGABRT;
  }
}

static void OnWindowButton(void* user, unsigned button, bool pressed) {
  if (!InputStreamMouseButton(user, button, pressed)) {
    LOG("Failed to handle mouse button");
    g_signal = SIGABRT;
  }
}

static void OnWindowWheel(void* user, int delta) {
  if (!InputStreamMouseWheel(user, delta)) {
    LOG("Failed to handle mouse wheel");
    g_signal = SIGABRT;
  }
}

static void GetMaxOverlaySize(size_t* width, size_t* height) {
  char str[64];
  snprintf(str, sizeof(str), "Video bitstream: %zu.000 Mbps", SIZE_MAX / 1000);
  *width = 4 + PuiStringWidth(str) + 4;
  *height = 4 + 12 * 5 + 4;
}

static struct Context* ContextCreate(int sock, bool no_input, bool stats,
                                     const char* audio_buffer) {
  int audio_buffer_size = 0;
  if (audio_buffer) {
    audio_buffer_size = atoi(audio_buffer);
    if (audio_buffer_size <= 0) {
      LOG("Invalid audio buffer size");
      return NULL;
    }
  }

  struct Context* context = calloc(1, sizeof(struct Context));
  if (!context) {
    LOG("Failed to allocate context (%s)", strerror(errno));
    return NULL;
  }

  context->audio_buffer_size = (size_t)audio_buffer_size;
  const struct WindowEventHandlers* maybe_window_event_handlers = NULL;
  if (!no_input) {
    context->input_stream = InputStreamCreate(sock);
    if (!context->input_stream) {
      LOG("Failed to create input stream");
      goto rollback_context;
    }
    static const struct WindowEventHandlers window_event_handlers = {
        .OnClose = OnWindowClose,
        .OnFocus = OnWindowFocus,
        .OnKey = OnWindowKey,
        .OnMove = OnWindowMove,
        .OnButton = OnWindowButton,
        .OnWheel = OnWindowWheel,
    };
    maybe_window_event_handlers = &window_event_handlers;
  }

  context->window =
      WindowCreate(maybe_window_event_handlers, context->input_stream);
  if (!context->window) {
    LOG("Failed to create window");
    goto rollback_input_stream;
  }

  if (stats) {
    GetMaxOverlaySize(&context->overlay_width, &context->overlay_height);
    context->overlay =
        OverlayCreate(context->window, 4, 4, (int)context->overlay_width,
                      (int)context->overlay_height);
    if (!context->overlay) {
      LOG("Failed to create stats overlay");
      goto rollback_window;
    }
  }

  context->decode_context = DecodeContextCreate(context->window);
  if (!context->decode_context) {
    LOG("Failed to create decode context");
    goto rollback_overlay;
  }
  return context;

rollback_overlay:
  if (context->overlay) OverlayDestroy(context->overlay);
rollback_window:
  WindowDestroy(context->window);
rollback_input_stream:
  if (context->input_stream) InputStreamDestroy(context->input_stream);
rollback_context:
  free(context);
  return NULL;
}

static bool RenderOverlay(struct Context* context, uint64_t timestamp) {
  uint32_t* buffer = OverlayLock(context->overlay);
  if (!buffer) {
    LOG("Failed to lock overlay");
    return false;
  }

  char ping_str[64];
  uint64_t ping = 0;
  if (context->ping_count) {
    ping = context->ping_sum / context->ping_count;
  }
  snprintf(ping_str, sizeof(ping_str), "Ping: %zu.%03zu ms", ping / 1000,
           ping % 1000);

  char video_bitrate_str[64];
  uint64_t clock_delta = timestamp - context->timestamp;
  // mburakov: Kbps = nbytes * 1sec * 8bit / clock_delta / 1024
  size_t video_bitrate =
      context->video_bitstream * 1000000 * 8 / clock_delta / 1024;
  snprintf(video_bitrate_str, sizeof(video_bitrate_str),
           "Video bitrate: %zu.%03zu Mbps", video_bitrate / 1000,
           video_bitrate % 1000);

  char audio_bitrate_str[64];
  size_t audio_bitrate = 0;
  if (context->audio_context) {
    // mburakov: Kbps = nbytes * 1sec * 8bit / clock_delta / 1024
    audio_bitrate = context->audio_bitstream * 1000000 * 8 / clock_delta / 1024;
    snprintf(audio_bitrate_str, sizeof(audio_bitrate_str),
             "Audio bitrate: %zu.%03zu Mbps", audio_bitrate / 1000,
             audio_bitrate % 1000);
  }

  char video_latency_str[64];
  uint64_t video_latency = 0;
  if (context->video_latency_count) {
    // mburakov: Pessimistic calculations, these assume one fully missed vsync
    // for capture, one fully missed vsync for rendering, and 100Mbit network.
    // latency = avg_latency + ping + vsync + vsync + Kbps * 1sec / 100Mbps
    video_latency =
        context->video_latency_sum / context->video_latency_count + ping +
        16666 + 16666 +
        video_bitrate * 1000000 / 100000000 / context->video_latency_count;
  }
  snprintf(video_latency_str, sizeof(video_latency_str),
           "Video latency: %zu.%03zu ms", video_latency / 1000,
           video_latency % 1000);

  char audio_latency_str[64];
  if (context->audio_context) {
    uint64_t audio_latency = 0;
    if (context->audio_latency_count) {
      // mburakov: Pessimistic calculations, assume 100Mbit network. Capture
      // and playback periods are unknown, but should roughly correspond to the
      // latency reported by the audio context, because it commulatively
      // includes all the missed periods since the beginning of streming.
      // latency = avg_latency + ping + Kbps * 1sec / 100Mbps + context_latency
      audio_latency =
          context->audio_latency_sum / context->audio_latency_count + ping +
          audio_bitrate * 1000000 / 100000000 +
          AudioContextGetLatency(context->audio_context);
    }
    snprintf(audio_latency_str, sizeof(audio_latency_str),
             "Audio latency: %zu.%03zu ms", audio_latency / 1000,
             audio_latency % 1000);
  }

  char* lines[5] = {NULL};
  char** plines = lines;
  *plines++ = ping_str;
  *plines++ = video_bitrate_str;
  if (context->audio_context) *plines++ = audio_bitrate_str;
  *plines++ = video_latency_str;
  if (context->audio_context) *plines++ = audio_latency_str;
  size_t nlines = (size_t)(plines - lines);

  size_t overlay_width = 0;
  for (size_t i = 0; i < nlines; i++)
    overlay_width = MAX(overlay_width, PuiStringWidth(lines[i]));
  overlay_width += 8;
  size_t overlay_height = 12 * nlines + 8;

  memset(buffer, 0, context->overlay_width * context->overlay_height * 4);
  for (size_t y = 0; y < overlay_height; y++) {
    for (size_t x = 0; x < overlay_width; x++)
      buffer[x + y * context->overlay_width] = 0x40000000;
  }

  for (size_t i = 0; i < nlines; i++) {
    size_t voffset = context->overlay_width * (4 + 12 * i);
    PuiStringRender(lines[i], buffer + voffset + 4, context->overlay_width,
                    0xffffffff);
  }
  OverlayUnlock(context->overlay);
  return true;
}

static bool HandleVideoStream(struct Context* context) {
  const struct Proto* proto = context->buffer.data;
  if (!DecodeContextDecode(context->decode_context, proto->data, proto->size)) {
    LOG("Failed to decode incoming video data");
    return false;
  }

  if (!context->overlay) return true;
  if (!context->timestamp) {
    context->timestamp = MicrosNow();
    return true;
  }

  context->video_bitstream += proto->size;
  context->video_latency_sum += proto->latency;
  context->video_latency_count++;

  if (!(proto->flags & PROTO_FLAG_KEYFRAME)) return true;

  uint64_t timestamp = MicrosNow();
  if (!RenderOverlay(context, timestamp)) LOG("Failed to render overlay");
  context->video_bitstream = 0;
  context->audio_bitstream = 0;
  context->timestamp = timestamp;
  context->ping_sum = 0;
  context->ping_count = 0;
  context->video_latency_sum = 0;
  context->video_latency_count = 0;
  context->audio_latency_sum = 0;
  context->audio_latency_count = 0;
  return true;
}

static bool HandleAudioStream(struct Context* context) {
  const struct Proto* proto = context->buffer.data;

  if (proto->flags & PROTO_FLAG_KEYFRAME) {
    // TODO(mburakov): Dynamic reconfiguration is unsupported.
    if (context->audio_context || !context->audio_buffer_size) return true;
    context->audio_context = AudioContextCreate(context->audio_buffer_size,
                                                (const char*)proto->data);
    if (!context->audio_context) LOG("Failed to create audio context");
    return !!context->audio_context;
  }

  if (!context->audio_context) return true;
  if (!AudioContextDecode(context->audio_context, proto->data, proto->size)) {
    LOG("Failed to decode incoming audio data");
    return false;
  }

  if (!context->overlay) return true;
  if (!context->timestamp) {
    context->timestamp = MicrosNow();
    return true;
  }

  context->audio_bitstream += proto->size;
  context->audio_latency_sum += proto->latency;
  context->audio_latency_count++;
  return true;
}

static bool DemuxProtoStream(int sock, struct Context* context) {
  switch (BufferAppendFrom(&context->buffer, sock)) {
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
  if (context->buffer.size < sizeof(struct Proto)) return true;
  const struct Proto* proto = context->buffer.data;
  if (context->buffer.size < sizeof(struct Proto) + proto->size) return true;

  switch (proto->type) {
    case PROTO_TYPE_MISC:
      context->ping_sum +=
          MicrosNow() - *(const uint64_t*)(const void*)proto->data;
      context->ping_count++;
      break;
    case PROTO_TYPE_VIDEO:
      if (!HandleVideoStream(context)) {
        LOG("Failed to handle video stream");
        return false;
      }
      break;
    case PROTO_TYPE_AUDIO:
      if (!HandleAudioStream(context)) {
        LOG("Failed to handle audio stream");
        return false;
      }
  }

  BufferDiscard(&context->buffer, sizeof(struct Proto) + proto->size);
  goto again;
}

static bool SendPingMessage(int sock, int timer_fd, struct Context* context) {
  uint64_t expirations;
  if (read(timer_fd, &expirations, sizeof(expirations)) !=
      sizeof(expirations)) {
    LOG("Failed to read timer expirations (%s)", strerror(errno));
    return false;
  }

  struct {
    uint32_t type;
    uint64_t timestamp;
  } __attribute__((packed)) ping = {
      .type = ~0u,
      .timestamp = MicrosNow(),
  };

  if (write(sock, &ping, sizeof(ping)) != sizeof(ping)) {
    LOG("Failed to write ping message (%s)", strerror(errno));
    return false;
  }
  return true;
}

static void ContextDestroy(struct Context* context) {
  BufferDestroy(&context->buffer);
  if (context->audio_context) AudioContextDestroy(context->audio_context);
  DecodeContextDestroy(context->decode_context);
  if (context->overlay) OverlayDestroy(context->overlay);
  WindowDestroy(context->window);
  if (context->input_stream) InputStreamDestroy(context->input_stream);
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    LOG("Usage: %s <ip>:<port> [--no-input] [--stats] [--audio <buffer_size>]",
        argv[0]);
    return EXIT_FAILURE;
  }

  int sock = ConnectSocket(argv[1]);
  if (sock == -1) {
    LOG("Failed to connect socket");
    return EXIT_FAILURE;
  }

  bool no_input = false;
  bool stats = false;
  const char* audio_buffer = NULL;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--no-input")) {
      no_input = true;
    } else if (!strcmp(argv[i], "--stats")) {
      stats = true;
    } else if (!strcmp(argv[i], "--audio")) {
      audio_buffer = argv[++i];
      if (i == argc) {
        LOG("Audio argument requires a value");
        return EXIT_FAILURE;
      }
    }
  }

  struct Context* context = ContextCreate(sock, no_input, stats, audio_buffer);
  if (!context) {
    LOG("Failed to create context");
    goto rollback_socket;
  }

  int events_fd = WindowGetEventsFd(context->window);
  if (events_fd == -1) {
    LOG("Failed to get events fd");
    goto rollback_context;
  }
  int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (timer_fd == -1) {
    LOG("Failed to create timer (%s)", strerror(errno));
    goto rollback_context;
  }

  static const unsigned ping_period_ns = 1000 * 1000 * 1000 / 3;
  static const struct itimerspec spec = {
      .it_interval.tv_nsec = ping_period_ns,
      .it_value.tv_nsec = ping_period_ns,
  };
  if (timerfd_settime(timer_fd, 0, &spec, NULL)) {
    LOG("Failed to arm timer (%s)", strerror(errno));
    goto rollback_timer_fd;
  }
  if (signal(SIGINT, OnSignal) == SIG_ERR ||
      signal(SIGTERM, OnSignal) == SIG_ERR) {
    LOG("Failed to set signal handlers (%s)", strerror(errno));
    goto rollback_timer_fd;
  }

  while (!g_signal) {
    struct pollfd pfds[] = {
        {.fd = sock, .events = POLLIN},
        {.fd = events_fd, .events = POLLIN},
        {.fd = timer_fd, .events = POLLIN},
    };
    switch (poll(pfds, LENGTH(pfds), -1)) {
      case -1:
        if (errno != EINTR) {
          LOG("Failed to poll (%s)", strerror(errno));
          goto rollback_timer_fd;
        }
        __attribute__((fallthrough));
      case 0:
        continue;
      default:
        break;
    }
    if (pfds[0].revents && !DemuxProtoStream(sock, context)) {
      LOG("Failed to demux proto stream");
      goto rollback_timer_fd;
    }
    if (pfds[1].revents && !WindowProcessEvents(context->window)) {
      LOG("Failed to process window events");
      goto rollback_timer_fd;
    }
    if (pfds[2].revents && !SendPingMessage(sock, timer_fd, context)) {
      LOG("Failed to send ping message");
      goto rollback_timer_fd;
    }
  }

rollback_timer_fd:
  close(timer_fd);
rollback_context:
  ContextDestroy(context);
rollback_socket:
  close(sock);
  bool result = g_signal == SIGINT || g_signal == SIGTERM;
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
