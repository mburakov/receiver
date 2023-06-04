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
#include "toolbox/utils.h"
#include "window.h"

#define OVERLAY_WIDTH 256
#define OVERLAY_HEIGHT 64

static volatile sig_atomic_t g_signal;
static void OnSignal(int status) { g_signal = status; }

static void SocketDtor(int* sock) {
  if (*sock == -1) return;
  close(*sock);
  *sock = -1;
}

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

static void InputStreamDtor(struct InputStream** input_stream) {
  if (!*input_stream) return;
  InputStreamDestroy(*input_stream);
  *input_stream = NULL;
}

static void WindowDtor(struct Window** window) {
  if (!*window) return;
  WindowDestroy(*window);
  *window = NULL;
}

static void OverlayDtor(struct Overlay** overlay) {
  if (!*overlay) return;
  OverlayDestroy(*overlay);
  *overlay = NULL;
}

static void DecodeContextDtor(struct DecodeContext** decode_context) {
  if (!*decode_context) return;
  DecodeContextDestroy(*decode_context);
  *decode_context = NULL;
}

static void RenderOverlay(struct Overlay* overlay) {
  uint32_t* buffer = OverlayLock(overlay);
  if (!buffer) {
    LOG("Failed to lock overlay");
    return;
  }

  for (int y = 0; y < OVERLAY_HEIGHT; y++) {
    for (int x = 0; x < OVERLAY_WIDTH; x++) {
      buffer[x + y * OVERLAY_WIDTH] = 0x40000000;
    }
  }
  OverlayUnlock(overlay);
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    LOG("Usage: %s <ip>:<port> [--no-input] [--stats]", argv[0]);
    return EXIT_FAILURE;
  }
  int __attribute__((cleanup(SocketDtor))) sock = ConnectSocket(argv[1]);
  if (sock == -1) return EXIT_FAILURE;

  bool no_input = false;
  bool stats = false;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--no-input")) {
      no_input = true;
    } else if (!strcmp(argv[i], "--stats")) {
      stats = true;
    }
  }
  struct InputStream
      __attribute__((cleanup(InputStreamDtor)))* maybe_input_stream = NULL;
  const struct WindowEventHandlers* maybe_window_event_handlers = NULL;
  if (!no_input) {
    maybe_input_stream = InputStreamCreate(sock);
    if (!maybe_input_stream) {
      LOG("Failed to create input stream");
      return EXIT_FAILURE;
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
  struct Window __attribute__((cleanup(WindowDtor)))* window =
      WindowCreate(maybe_window_event_handlers, maybe_input_stream);
  if (!window) {
    LOG("Failed to create window");
    return EXIT_FAILURE;
  }

  struct Overlay __attribute__((cleanup(OverlayDtor)))* overlay = NULL;
  if (stats) {
    overlay = OverlayCreate(window, 0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT);
    if (!overlay) {
      LOG("Failed to create overlay");
      return EXIT_FAILURE;
    }
  }

  struct DecodeContext
      __attribute__((cleanup(DecodeContextDtor)))* decode_context =
          DecodeContextCreate(window);
  if (!decode_context) {
    LOG("Failed to create decode context");
    return EXIT_FAILURE;
  }

  int events_fd = WindowGetEventsFd(window);
  if (events_fd == -1) {
    LOG("Failed to get events fd");
    return EXIT_FAILURE;
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
          return EXIT_FAILURE;
        }
        __attribute__((fallthrough));
      case 0:
        continue;
      default:
        break;
    }
    if (overlay) RenderOverlay(overlay);
    if (pfds[0].revents && !DecodeContextDecode(decode_context, sock)) {
      LOG("Failed to decode incoming data");
      return EXIT_FAILURE;
    }
    if (pfds[1].revents && !WindowProcessEvents(window)) {
      LOG("Failed to process window events");
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}
