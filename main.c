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
#include "toolbox/utils.h"
#include "window.h"

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
  static const int one = 1;
  if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one))) {
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

static void OnWindowKey(void* user, unsigned key, bool pressed) {
  // TODO
}

static void WindowDtor(struct Window** window) {
  if (!*window) return;
  WindowDestroy(*window);
  *window = NULL;
}

static void DecodeContextDtor(struct DecodeContext** decode_context) {
  if (!*decode_context) return;
  DecodeContextDestroy(*decode_context);
  *decode_context = NULL;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    LOG("Usage: %s <ip>:<port>", argv[0]);
    return EXIT_FAILURE;
  }
  int __attribute__((cleanup(SocketDtor))) sock = ConnectSocket(argv[1]);
  if (sock == -1) return EXIT_FAILURE;

  static const struct WindowEventHandlers window_event_handlers = {
      .OnClose = OnWindowClose,
      .OnKey = OnWindowKey,
  };
  struct Window __attribute__((cleanup(WindowDtor)))* window =
      WindowCreate(&window_event_handlers, NULL);
  if (!window) {
    LOG("Failed to create window");
    return EXIT_FAILURE;
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
