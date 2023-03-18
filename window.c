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

#include "window.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "frame.h"
#include "linux-dmabuf-unstable-v1.h"
#include "util.h"
#include "xdg-shell.h"

struct Window {
  struct wl_display* wl_display;
  struct wl_surface* wl_surface;
  struct xdg_wm_base* xdg_wm_base;
  struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf_v1;
  struct xdg_surface* xdg_surface;
  struct xdg_toplevel* xdg_toplevel;
  struct wl_buffer* wl_buffer;
};

static void wl_registryDestroy(struct wl_registry** wl_registry) {
  if (!wl_registry || !*wl_registry) return;
  wl_registry_destroy(*wl_registry);
  *wl_registry = NULL;
}

static void wl_compositorDestroy(struct wl_compositor** wl_compositor) {
  if (!wl_compositor || !*wl_compositor) return;
  wl_compositor_destroy(*wl_compositor);
  *wl_compositor = NULL;
}

static void xdg_wm_baseDestroy(struct xdg_wm_base** xdg_wm_base) {
  if (!xdg_wm_base || !*xdg_wm_base) return;
  xdg_wm_base_destroy(*xdg_wm_base);
  *xdg_wm_base = NULL;
}

static void zwp_linux_buffer_params_v1Destroy(
    struct zwp_linux_buffer_params_v1** zwp_linux_buffer_params_v1) {
  if (!zwp_linux_buffer_params_v1 || !*zwp_linux_buffer_params_v1) return;
  zwp_linux_buffer_params_v1_destroy(*zwp_linux_buffer_params_v1);
  *zwp_linux_buffer_params_v1 = NULL;
}

static void wl_bufferDestroy(struct wl_buffer** wl_buffer) {
  if (!wl_buffer || !*wl_buffer) return;
  wl_buffer_destroy(*wl_buffer);
  *wl_buffer = NULL;
}

static void OnWmBasePing(void* data, struct xdg_wm_base* xdg_wm_base,
                         uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(xdg_wm_base, serial);
}

static void OnRegistryGlobal(void* data, struct wl_registry* registry,
                             uint32_t name, const char* interface,
                             uint32_t version) {
  struct Window* window = data;
  if (!strcmp(interface, wl_compositor_interface.name)) {
    struct AUTO(wl_compositor)* compositor =
        wl_registry_bind(registry, name, &wl_compositor_interface, version);
    if (!compositor) {
      LOG("Failed to bind wayland compositor (%s)", strerror(errno));
      return;
    }
    window->wl_surface = wl_compositor_create_surface(compositor);
    if (!window->wl_surface) {
      LOG("Failed to create wayland surface (%s)", strerror(errno));
      return;
    }
  } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
    struct AUTO(xdg_wm_base)* xdg_wm_base =
        wl_registry_bind(registry, name, &xdg_wm_base_interface, version);
    if (!xdg_wm_base) {
      LOG("Failed to bind wayland xdg_wm_base (%s)", strerror(errno));
      return;
    }
    static const struct xdg_wm_base_listener wm_base_listener = {
        .ping = OnWmBasePing,
    };
    if (xdg_wm_base_add_listener(xdg_wm_base, &wm_base_listener, NULL)) {
      LOG("Failed to add wayland wm base listener (%s)", strerror(errno));
      return;
    }
    window->xdg_wm_base = RELEASE(xdg_wm_base);
  } else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
    window->zwp_linux_dmabuf_v1 = wl_registry_bind(
        registry, name, &zwp_linux_dmabuf_v1_interface, version);
    if (!window->zwp_linux_dmabuf_v1)
      LOG("Failed to bind wayland zwp_linux_dmabuf_v1 (%s)", strerror(errno));
  }
}

static void OnRegistryGlobalRemove(void* data, struct wl_registry* registry,
                                   uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static void OnSurfaceConfigure(void* data, struct xdg_surface* xdg_surface,
                               uint32_t serial) {
  (void)data;
  xdg_surface_ack_configure(xdg_surface, serial);
}

static void OnToplevelConfigure(void* data, struct xdg_toplevel* xdg_toplevel,
                                int32_t width, int32_t height,
                                struct wl_array* states) {
  (void)data;
  (void)xdg_toplevel;
  (void)width;
  (void)height;
  (void)states;
}

static void OnToplevelClose(void* data, struct xdg_toplevel* xdg_toplevel) {
  (void)data;
  (void)xdg_toplevel;
  raise(SIGINT);
}

static void OnToplevelConfigureBounds(void* data,
                                      struct xdg_toplevel* xdg_toplevel,
                                      int32_t width, int32_t height) {
  (void)data;
  (void)xdg_toplevel;
  (void)width;
  (void)height;
}

static void OnToplevelWmCapabilities(void* data,
                                     struct xdg_toplevel* xdg_toplevel,
                                     struct wl_array* capabilities) {
  (void)data;
  (void)xdg_toplevel;
  (void)capabilities;
}

struct Window* WindowCreate(void) {
  struct AUTO(Window)* window = malloc(sizeof(struct Window));
  if (!window) {
    LOG("Failed to allocate window (%s)", strerror(errno));
    return NULL;
  }
  *window = (struct Window){
      .wl_display = NULL,
      .wl_surface = NULL,
      .xdg_wm_base = NULL,
      .zwp_linux_dmabuf_v1 = NULL,
      .xdg_surface = NULL,
      .xdg_toplevel = NULL,
      .wl_buffer = NULL,
  };

  window->wl_display = wl_display_connect(NULL);
  if (!window->wl_display) {
    LOG("Failed to connect wayland display (%s)", strerror(errno));
    return NULL;
  }

  struct AUTO(wl_registry)* wl_registry =
      wl_display_get_registry(window->wl_display);
  if (!wl_registry) {
    LOG("Failed to get wayland registry (%s)", strerror(errno));
    return NULL;
  }
  static const struct wl_registry_listener wl_registry_listener = {
      .global = OnRegistryGlobal,
      .global_remove = OnRegistryGlobalRemove,
  };
  if (wl_registry_add_listener(wl_registry, &wl_registry_listener, window)) {
    LOG("Failed to set wayland registry listener (%s)", strerror(errno));
    return NULL;
  }
  if (wl_display_roundtrip(window->wl_display) == -1) {
    LOG("Failed to roundtrip wayland display (%s)", strerror(errno));
    return NULL;
  }

  if (!window->wl_surface || !window->xdg_wm_base ||
      !window->zwp_linux_dmabuf_v1) {
    LOG("Some wayland objects are missing");
    return NULL;
  }
  window->xdg_surface =
      xdg_wm_base_get_xdg_surface(window->xdg_wm_base, window->wl_surface);
  if (!window->xdg_surface) {
    LOG("Failed to get wayland surface (%s)", strerror(errno));
    return NULL;
  }
  static const struct xdg_surface_listener xdg_surface_listener = {
      .configure = OnSurfaceConfigure,
  };
  if (xdg_surface_add_listener(window->xdg_surface, &xdg_surface_listener,
                               NULL)) {
    LOG("Failed to add wayland surface listener (%s)", strerror(errno));
    return NULL;
  }
  window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
  if (!window->xdg_toplevel) {
    LOG("Failed to get wayland toplevel (%s)", strerror(errno));
    return NULL;
  }
  static const struct xdg_toplevel_listener xdg_toplevel_listener = {
      .configure = OnToplevelConfigure,
      .close = OnToplevelClose,
      .configure_bounds = OnToplevelConfigureBounds,
      .wm_capabilities = OnToplevelWmCapabilities,
  };
  if (xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener,
                                NULL)) {
    LOG("Failed to add wayland toplevel listener (%s)", strerror(errno));
    return NULL;
  }
  xdg_toplevel_set_fullscreen(window->xdg_toplevel, NULL);
  wl_surface_commit(window->wl_surface);
  return RELEASE(window);
}

int WindowGetEventsFd(const struct Window* window) {
  int events_fd = wl_display_get_fd(window->wl_display);
  if (events_fd == -1)
    LOG("Failed to get wayland display fd (%s)", strerror(errno));
  return events_fd;
}

bool WindowProcessEvents(const struct Window* window) {
  if (wl_display_dispatch(window->wl_display) == -1) {
    LOG("Failed to dispatch wayland display (%s)", strerror(errno));
    return false;
  }
  return true;
}

static void OnZwpLinuxBufferParamsCreated(
    void* data, struct zwp_linux_buffer_params_v1* zwp_linux_buffer_params_v1,
    struct wl_buffer* buffer) {
  (void)zwp_linux_buffer_params_v1;
  struct wl_buffer** wl_buffer = data;
  *wl_buffer = buffer;
}

static void OnZwpLinuxBufferParamsFailed(
    void* data, struct zwp_linux_buffer_params_v1* zwp_linux_buffer_params_v1) {
  (void)data;
  (void)zwp_linux_buffer_params_v1;
}

bool WindowRenderFrame(struct Window* window, const struct Frame* frame) {
  struct AUTO(zwp_linux_buffer_params_v1)* zwp_linux_buffer_params_v1 =
      zwp_linux_dmabuf_v1_create_params(window->zwp_linux_dmabuf_v1);
  if (!zwp_linux_buffer_params_v1) {
    LOG("Failed to create wayland dmabuf params (%s)", strerror(errno));
    return false;
  }

  struct AUTO(wl_buffer)* wl_buffer = NULL;
  static const struct zwp_linux_buffer_params_v1_listener
      zwp_linux_buffer_params_v1_listener = {
          .created = OnZwpLinuxBufferParamsCreated,
          .failed = OnZwpLinuxBufferParamsFailed,
      };
  if (zwp_linux_buffer_params_v1_add_listener(
          zwp_linux_buffer_params_v1, &zwp_linux_buffer_params_v1_listener,
          &wl_buffer)) {
    LOG("Failed to add buffer wayland dmabuf params listener (%s)",
        strerror(errno));
    return false;
  }

  for (size_t i = 0; i < frame->nplanes; i++) {
    zwp_linux_buffer_params_v1_add(
        zwp_linux_buffer_params_v1, frame->planes[i].dmabuf_fd, (uint32_t)i,
        frame->planes[i].offset, frame->planes[i].pitch,
        frame->planes[i].modifier >> 32,
        frame->planes[i].modifier & UINT32_MAX);
  }
  zwp_linux_buffer_params_v1_create(zwp_linux_buffer_params_v1,
                                    (int)frame->width, (int)frame->height,
                                    frame->fourcc, 0);
  if (wl_display_roundtrip(window->wl_display) == -1) {
    LOG("Failed to roundtrip wayland display (%s)", strerror(errno));
    return false;
  }
  if (!wl_buffer) {
    LOG("Failed to create wl_buffer");
    return false;
  }

  SWAP(window->wl_buffer, wl_buffer);
  wl_surface_attach(window->wl_surface, window->wl_buffer, 0, 0);
  wl_surface_damage(window->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(window->wl_surface);
  if (wl_display_roundtrip(window->wl_display) == -1) {
    LOG("Failed to roundtrip wayland display (%s)", strerror(errno));
    return false;
  }
  return true;
}

void WindowDestroy(struct Window** window) {
  if (!window || !*window) return;
  if ((*window)->xdg_toplevel) xdg_toplevel_destroy((*window)->xdg_toplevel);
  if ((*window)->xdg_surface) xdg_surface_destroy((*window)->xdg_surface);
  if ((*window)->zwp_linux_dmabuf_v1)
    zwp_linux_dmabuf_v1_destroy((*window)->zwp_linux_dmabuf_v1);
  if ((*window)->xdg_wm_base) xdg_wm_base_destroy((*window)->xdg_wm_base);
  if ((*window)->wl_surface) wl_surface_destroy((*window)->wl_surface);
  if ((*window)->wl_display) wl_display_disconnect((*window)->wl_display);
  free(*window);
  *window = NULL;
}
