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
#include <unistd.h>

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
  struct InputStream* input_stream;
  struct Window* window;
  size_t overlay_width;
  size_t overlay_height;
  struct Overlay* overlay;
  struct DecodeContext* decode_context;
  struct Buffer buffer;

  size_t video_bitstream;
  uint64_t timestamp;
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
  snprintf(str, sizeof(str), "Bitrate: %zu.000 Mbps", SIZE_MAX / 1000);
  *width = PuiStringWidth(str) + 8;
  *height = 20;
}

static struct Context* ContextCreate(int sock, bool no_input, bool stats) {
  struct Context* context = calloc(1, sizeof(struct Context));
  if (!context) {
    LOG("Failed to allocate context (%s)", strerror(errno));
    return NULL;
  }

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

  char str[64];
  uint64_t clock_delta = timestamp - context->timestamp;
  size_t bitrate = context->video_bitstream * 1000000 / clock_delta / 1024;
  snprintf(str, sizeof(str), "Bitrate: %zu.%03zu Mbps", bitrate / 1000,
           bitrate % 1000);

  size_t overlay_width = PuiStringWidth(str) + 8;
  memset(buffer, 0, context->overlay_width * context->overlay_height * 4);
  for (size_t y = 0; y < context->overlay_height; y++) {
    for (size_t x = 0; x < overlay_width; x++)
      buffer[x + y * context->overlay_width] = 0x40000000;
  }

  size_t voffset = context->overlay_width * 4;
  PuiStringRender(str, buffer + voffset + 4, context->overlay_width,
                  0xffffffff);
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
  if (!(proto->flags & PROTO_FLAG_KEYFRAME)) return true;

  uint64_t timestamp = MicrosNow();
  if (!RenderOverlay(context, timestamp)) LOG("Failed to render overlay");
  context->video_bitstream = 0;
  context->timestamp = timestamp;
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
    case PROTO_TYPE_VIDEO:
      if (!HandleVideoStream(context)) {
        LOG("Failed to handle video stream");
        return false;
      }
      break;
  }

  BufferDiscard(&context->buffer, sizeof(struct Proto) + proto->size);
  goto again;
}

static void ContextDestroy(struct Context* context) {
  BufferDestroy(&context->buffer);
  DecodeContextDestroy(context->decode_context);
  if (context->overlay) OverlayDestroy(context->overlay);
  WindowDestroy(context->window);
  if (context->input_stream) InputStreamDestroy(context->input_stream);
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    LOG("Usage: %s <ip>:<port> [--no-input] [--stats]", argv[0]);
    return EXIT_FAILURE;
  }

  int sock = ConnectSocket(argv[1]);
  if (sock == -1) {
    LOG("Failed to connect socket");
    return EXIT_FAILURE;
  }

  bool no_input = false;
  bool stats = false;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--no-input")) {
      no_input = true;
    } else if (!strcmp(argv[i], "--stats")) {
      stats = true;
    }
  }

  struct Context* context = ContextCreate(sock, no_input, stats);
  if (!context) {
    LOG("Failed to create context");
    goto rollback_socket;
  }

  int events_fd = WindowGetEventsFd(context->window);
  if (events_fd == -1) {
    LOG("Failed to get events fd");
    goto rollback_context;
  }

  if (signal(SIGINT, OnSignal) == SIG_ERR ||
      signal(SIGTERM, OnSignal) == SIG_ERR) {
    LOG("Failed to set signal handlers (%s)", strerror(errno));
    return EXIT_FAILURE;
  }

  while (!g_signal) {
    struct pollfd pfds[] = {
        {.fd = sock, .events = POLLIN},
        {.fd = events_fd, .events = POLLIN},
    };
    switch (poll(pfds, LENGTH(pfds), -1)) {
      case -1:
        if (errno != EINTR) {
          LOG("Failed to poll (%s)", strerror(errno));
          goto rollback_context;
        }
        __attribute__((fallthrough));
      case 0:
        continue;
      default:
        break;
    }
    if (pfds[0].revents && !DemuxProtoStream(sock, context)) {
      LOG("Failed to demux proto stream");
      goto rollback_context;
    }
    if (pfds[1].revents && !WindowProcessEvents(context->window)) {
      LOG("Failed to process window events");
      goto rollback_context;
    }
  }

rollback_context:
  ContextDestroy(context);
rollback_socket:
  close(sock);
  bool result = g_signal == SIGINT || g_signal == SIGTERM;
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
