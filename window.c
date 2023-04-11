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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "frame.h"
#include "linux-dmabuf-unstable-v1.h"
#include "pointer-constraints-unstable-v1.h"
#include "relative-pointer-unstable-v1.h"
#include "toolbox/utils.h"
#include "xdg-shell.h"

// TODO(mburakov): This would look like shit until Wayland guys finally fix
// https://gitlab.freedesktop.org/wayland/wayland/-/issues/160

struct Window {
  const struct WindowEventHandlers* event_handlers;
  void* user;

  struct wl_display* wl_display;
  struct wl_surface* wl_surface;
  struct wl_pointer* wl_pointer;
  struct wl_keyboard* wl_keyboard;
  struct xdg_wm_base* xdg_wm_base;
  struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf_v1;
  struct zwp_pointer_constraints_v1* zwp_pointer_constraints_v1;
  struct zwp_relative_pointer_manager_v1* zwp_relative_pointer_manager_v1;

  struct zwp_relative_pointer_v1* zwp_relative_pointer_v1;
  struct zwp_locked_pointer_v1* zwp_locked_pointer_v1;
  struct xdg_surface* xdg_surface;
  struct xdg_toplevel* xdg_toplevel;
  struct wl_buffer** wl_buffers;
  bool was_closed;
};

static void OnWlRegistryGlobal(void* data, struct wl_registry* wl_registry,
                               uint32_t name, const char* interface,
                               uint32_t version) {
  struct Window* window = data;
  if (!strcmp(interface, wl_compositor_interface.name)) {
    struct wl_compositor* compositor =
        wl_registry_bind(wl_registry, name, &wl_compositor_interface, version);
    if (!compositor) {
      LOG("Failed to bind wl_compositor (%s)", strerror(errno));
      return;
    }
    window->wl_surface = wl_compositor_create_surface(compositor);
    if (!window->wl_surface)
      LOG("Failed to create wl_surface (%s)", strerror(errno));
    wl_compositor_destroy(compositor);

  } else if (!strcmp(interface, wl_seat_interface.name) &&
             window->event_handlers) {
    struct wl_seat* wl_seat =
        wl_registry_bind(wl_registry, name, &wl_seat_interface, version);
    if (!wl_seat) {
      LOG("Failed to bind wl_seat (%s)", strerror(errno));
      return;
    }
    window->wl_pointer = wl_seat_get_pointer(wl_seat);
    if (!window->wl_pointer)
      LOG("Failed to get wl_pointer (%s)", strerror(errno));
    window->wl_keyboard = wl_seat_get_keyboard(wl_seat);
    if (!window->wl_keyboard)
      LOG("Failed to get wl_keyboard (%s)", strerror(errno));
    wl_seat_destroy(wl_seat);

  } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
    window->xdg_wm_base =
        wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, version);
    if (!window->xdg_wm_base)
      LOG("Failed to bind xdg_wm_base (%s)", strerror(errno));

  } else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
    window->zwp_linux_dmabuf_v1 = wl_registry_bind(
        wl_registry, name, &zwp_linux_dmabuf_v1_interface, version);
    if (!window->zwp_linux_dmabuf_v1)
      LOG("Failed to bind zwp_linux_dmabuf_v1 (%s)", strerror(errno));

  } else if (!strcmp(interface, zwp_pointer_constraints_v1_interface.name) &&
             window->event_handlers) {
    window->zwp_pointer_constraints_v1 = wl_registry_bind(
        wl_registry, name, &zwp_pointer_constraints_v1_interface, version);
    if (!window->zwp_pointer_constraints_v1)
      LOG("Failed to bind zwp_pointer_constraints_v1 (%s)", strerror(errno));

  } else if (!strcmp(interface,
                     zwp_relative_pointer_manager_v1_interface.name) &&
             window->event_handlers) {
    window->zwp_relative_pointer_manager_v1 = wl_registry_bind(
        wl_registry, name, &zwp_relative_pointer_manager_v1_interface, version);
    if (!window->zwp_relative_pointer_manager_v1) {
      LOG("Failed to bind zwp_relative_pointer_manager_v1 (%s)",
          strerror(errno));
    }
  }
}

static void OnWlRegistryGlobalRemove(void* data, struct wl_registry* registry,
                                     uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static void OnWlPointerEnter(void* data, struct wl_pointer* wl_pointer,
                             uint32_t serial, struct wl_surface* surface,
                             wl_fixed_t surface_x, wl_fixed_t surface_y) {
  (void)data;
  (void)surface;
  (void)surface_x;
  (void)surface_y;
  wl_pointer_set_cursor(wl_pointer, serial, NULL, 0, 0);
}

static void OnWlPointerLeave(void* data, struct wl_pointer* wl_pointer,
                             uint32_t serial, struct wl_surface* surface) {
  (void)data;
  (void)wl_pointer;
  (void)serial;
  (void)surface;
}

static void OnWlPointerMotion(void* data, struct wl_pointer* wl_pointer,
                              uint32_t time, wl_fixed_t surface_x,
                              wl_fixed_t surface_y) {
  (void)data;
  (void)wl_pointer;
  (void)time;
  (void)surface_x;
  (void)surface_y;
}

static void OnWlPointerButton(void* data, struct wl_pointer* wl_pointer,
                              uint32_t serial, uint32_t time, uint32_t button,
                              uint32_t state) {
  (void)wl_pointer;
  (void)serial;
  (void)time;
  struct Window* window = data;
  window->event_handlers->OnButton(window->user, button, !!state);
}

static void OnWlPointerAxis(void* data, struct wl_pointer* wl_pointer,
                            uint32_t time, uint32_t axis, wl_fixed_t value) {
  (void)data;
  (void)wl_pointer;
  (void)time;
  (void)axis;
  (void)value;
}

static void OnWlPointerFrame(void* data, struct wl_pointer* wl_pointer) {
  (void)data;
  (void)wl_pointer;
}

static void OnWlPointerAxisSource(void* data, struct wl_pointer* wl_pointer,
                                  uint32_t axis_source) {
  (void)data;
  (void)wl_pointer;
  (void)axis_source;
}

static void OnWlPointerAxisStop(void* data, struct wl_pointer* wl_pointer,
                                uint32_t time, uint32_t axis) {
  (void)data;
  (void)wl_pointer;
  (void)time;
  (void)axis;
}

static void OnWlPointerAxisDiscrete(void* data, struct wl_pointer* wl_pointer,
                                    uint32_t axis, int32_t discrete) {
  (void)data;
  (void)wl_pointer;
  (void)axis;
  (void)discrete;
}

static void OnWlPointerAxisValue120(void* data, struct wl_pointer* wl_pointer,
                                    uint32_t axis, int32_t value120) {
  (void)wl_pointer;
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    // mburakov: Current code models regular one-wheeled mouse.
    return;
  }
  struct Window* window = data;
  // TODO(mburakov): Why minus is needed here?
  window->event_handlers->OnWheel(window->user, -value120 / 120);
}

static void OnWlKeyboardKeymap(void* data, struct wl_keyboard* wl_keyboard,
                               uint32_t format, int32_t fd, uint32_t size) {
  (void)data;
  (void)wl_keyboard;
  (void)format;
  (void)fd;
  (void)size;
}

static void OnWlKeyboardEnter(void* data, struct wl_keyboard* wl_keyboard,
                              uint32_t serial, struct wl_surface* surface,
                              struct wl_array* keys) {
  (void)wl_keyboard;
  (void)serial;
  (void)surface;
  (void)keys;
  struct Window* window = data;
  window->event_handlers->OnFocus(window->user, true);
}

static void OnWlKeyboardLeave(void* data, struct wl_keyboard* wl_keyboard,
                              uint32_t serial, struct wl_surface* surface) {
  (void)data;
  (void)wl_keyboard;
  (void)serial;
  (void)surface;
  struct Window* window = data;
  window->event_handlers->OnFocus(window->user, false);
}

static void OnWlKeyboardKey(void* data, struct wl_keyboard* wl_keyboard,
                            uint32_t serial, uint32_t time, uint32_t key,
                            uint32_t state) {
  (void)wl_keyboard;
  (void)serial;
  (void)time;
  struct Window* window = data;
  window->event_handlers->OnKey(window->user, key, !!state);
}

static void OnWlKeyboardModifiers(void* data, struct wl_keyboard* wl_keyboard,
                                  uint32_t serial, uint32_t mods_depressed,
                                  uint32_t mods_latched, uint32_t mods_locked,
                                  uint32_t group) {
  (void)data;
  (void)wl_keyboard;
  (void)serial;
  (void)mods_depressed;
  (void)mods_latched;
  (void)mods_locked;
  (void)group;
}

static void OnWlKeyboardRepeatInfo(void* data, struct wl_keyboard* wl_keyboard,
                                   int32_t rate, int32_t delay) {
  (void)data;
  (void)wl_keyboard;
  (void)rate;
  (void)delay;
}

static void OnXdgWmBasePing(void* data, struct xdg_wm_base* xdg_wm_base,
                            uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(xdg_wm_base, serial);
}

static void OnZwpRelativePointerMotion(
    void* data, struct zwp_relative_pointer_v1* zwp_relative_pointer_v1,
    uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy,
    wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
  (void)zwp_relative_pointer_v1;
  (void)utime_hi;
  (void)utime_lo;
  (void)dx;
  (void)dy;
  struct Window* window = data;
  window->event_handlers->OnMove(window->user, wl_fixed_to_int(dx_unaccel),
                                 wl_fixed_to_int(dy_unaccel));
}

static void OnXdgSurfaceConfigure(void* data, struct xdg_surface* xdg_surface,
                                  uint32_t serial) {
  (void)data;
  xdg_surface_ack_configure(xdg_surface, serial);
}

static void OnXdgToplevelConfigure(void* data,
                                   struct xdg_toplevel* xdg_toplevel,
                                   int32_t width, int32_t height,
                                   struct wl_array* states) {
  (void)data;
  (void)xdg_toplevel;
  (void)width;
  (void)height;
  (void)states;
}

static void OnXdgToplevelClose(void* data, struct xdg_toplevel* xdg_toplevel) {
  (void)xdg_toplevel;
  struct Window* window = data;
  if (window->event_handlers) {
    window->event_handlers->OnClose(window->user);
  } else {
    window->was_closed = true;
  }
}

static void OnXdgToplevelConfigureBounds(void* data,
                                         struct xdg_toplevel* xdg_toplevel,
                                         int32_t width, int32_t height) {
  (void)data;
  (void)xdg_toplevel;
  (void)width;
  (void)height;
}

static void OnXdgToplevelWmCapabilities(void* data,
                                        struct xdg_toplevel* xdg_toplevel,
                                        struct wl_array* capabilities) {
  (void)data;
  (void)xdg_toplevel;
  (void)capabilities;
}

struct Window* WindowCreate(const struct WindowEventHandlers* event_handlers,
                            void* user) {
  struct Window* window = malloc(sizeof(struct Window));
  if (!window) {
    LOG("Failed to allocate window (%s)", strerror(errno));
    return NULL;
  }
  *window = (struct Window){
      .event_handlers = event_handlers,
      .user = user,
  };

  window->wl_display = wl_display_connect(NULL);
  if (!window->wl_display) {
    LOG("Failed to connect wl_display (%s)", strerror(errno));
    goto rollback_window;
  }

  struct wl_registry* wl_registry = wl_display_get_registry(window->wl_display);
  if (!wl_registry) {
    LOG("Failed to get wl_registry (%s)", strerror(errno));
    goto rollback_wl_display;
  }
  static const struct wl_registry_listener wl_registry_listener = {
      .global = OnWlRegistryGlobal,
      .global_remove = OnWlRegistryGlobalRemove,
  };
  if (wl_registry_add_listener(wl_registry, &wl_registry_listener, window)) {
    LOG("Failed to add wl_registry listener (%s)", strerror(errno));
    goto rollback_wl_registry;
  }
  if (wl_display_roundtrip(window->wl_display) == -1) {
    LOG("Failed to roundtrip wl_display (%s)", strerror(errno));
    goto rollback_wl_registry;
  }
  if (!window->wl_surface || !window->xdg_wm_base ||
      !window->zwp_linux_dmabuf_v1) {
    LOG("Some fundamental wayland objects are missing");
    goto rollback_globals;
  }
  if (window->event_handlers && (!window->wl_pointer || !window->wl_keyboard ||
                                 !window->zwp_pointer_constraints_v1 ||
                                 !window->zwp_relative_pointer_manager_v1)) {
    LOG("Some input-related wayland objects are missing");
    goto rollback_globals;
  }

  if (window->wl_pointer) {
    static const struct wl_pointer_listener wl_pointer_listener = {
        .enter = OnWlPointerEnter,
        .leave = OnWlPointerLeave,
        .motion = OnWlPointerMotion,
        .button = OnWlPointerButton,
        .axis = OnWlPointerAxis,
        .frame = OnWlPointerFrame,
        .axis_source = OnWlPointerAxisSource,
        .axis_stop = OnWlPointerAxisStop,
        .axis_discrete = OnWlPointerAxisDiscrete,
        .axis_value120 = OnWlPointerAxisValue120,
    };
    if (wl_pointer_add_listener(window->wl_pointer, &wl_pointer_listener,
                                window)) {
      LOG("Failed to add wl_pointer listener (%s)", strerror(errno));
      goto rollback_globals;
    }
  }
  if (window->wl_keyboard) {
    static const struct wl_keyboard_listener wl_keyboard_listener = {
        .keymap = OnWlKeyboardKeymap,
        .enter = OnWlKeyboardEnter,
        .leave = OnWlKeyboardLeave,
        .key = OnWlKeyboardKey,
        .modifiers = OnWlKeyboardModifiers,
        .repeat_info = OnWlKeyboardRepeatInfo,
    };
    if (wl_keyboard_add_listener(window->wl_keyboard, &wl_keyboard_listener,
                                 window)) {
      LOG("Failed to add wl_keyboard listener (%s)", strerror(errno));
      goto rollback_globals;
    }
  }
  static const struct xdg_wm_base_listener xdg_wm_base_listener = {
      .ping = OnXdgWmBasePing,
  };
  if (xdg_wm_base_add_listener(window->xdg_wm_base, &xdg_wm_base_listener,
                               NULL)) {
    LOG("Failed to add xdg_wm_base listener (%s)", strerror(errno));
    goto rollback_globals;
  }

  if (window->wl_pointer) {
    window->zwp_locked_pointer_v1 = zwp_pointer_constraints_v1_lock_pointer(
        window->zwp_pointer_constraints_v1, window->wl_surface,
        window->wl_pointer, NULL,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    if (!window->zwp_locked_pointer_v1) {
      LOG("Failed to lock wl_pointer (%s)", strerror(errno));
      goto rollback_globals;
    }
  }

  if (window->wl_pointer) {
    window->zwp_relative_pointer_v1 =
        zwp_relative_pointer_manager_v1_get_relative_pointer(
            window->zwp_relative_pointer_manager_v1, window->wl_pointer);
    if (!window->zwp_relative_pointer_v1) {
      LOG("Failed to get zwp_relative_pointer_v1 (%s)", strerror(errno));
      goto rollback_zwp_locked_pointer_v1;
    }
    static const struct zwp_relative_pointer_v1_listener
        zwp_relative_pointer_v1_listener = {
            .relative_motion = OnZwpRelativePointerMotion,
        };
    if (zwp_relative_pointer_v1_add_listener(window->zwp_relative_pointer_v1,
                                             &zwp_relative_pointer_v1_listener,
                                             window)) {
      LOG("Failed to add zwp_relative_pointer_v1 listener (%s)",
          strerror(errno));
      goto rollback_zwp_relative_pointer_v1;
    }
  }

  window->xdg_surface =
      xdg_wm_base_get_xdg_surface(window->xdg_wm_base, window->wl_surface);
  if (!window->xdg_surface) {
    LOG("Failed to get xdg_surface (%s)", strerror(errno));
    goto rollback_zwp_relative_pointer_v1;
  }
  static const struct xdg_surface_listener xdg_surface_listener = {
      .configure = OnXdgSurfaceConfigure,
  };
  if (xdg_surface_add_listener(window->xdg_surface, &xdg_surface_listener,
                               NULL)) {
    LOG("Failed to add xdg_surface listener (%s)", strerror(errno));
    goto rollback_xdg_surface;
  }

  window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
  if (!window->xdg_toplevel) {
    LOG("Failed to get xdg_toplevel (%s)", strerror(errno));
    goto rollback_xdg_surface;
  }
  static const struct xdg_toplevel_listener xdg_toplevel_listener = {
      .configure = OnXdgToplevelConfigure,
      .close = OnXdgToplevelClose,
      .configure_bounds = OnXdgToplevelConfigureBounds,
      .wm_capabilities = OnXdgToplevelWmCapabilities,
  };
  if (xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener,
                                window)) {
    LOG("Failed to add xdg_toplevel listener (%s)", strerror(errno));
    goto rollback_xdg_toplevel;
  }
  xdg_toplevel_set_fullscreen(window->xdg_toplevel, NULL);
  wl_surface_commit(window->wl_surface);
  if (wl_display_roundtrip(window->wl_display) == -1) {
    LOG("Failed to roundtrip wl_display (%s)", strerror(errno));
    goto rollback_xdg_toplevel;
  }
  if (window->zwp_locked_pointer_v1)
    zwp_locked_pointer_v1_set_region(window->zwp_locked_pointer_v1, NULL);
  wl_registry_destroy(wl_registry);
  return window;

rollback_xdg_toplevel:
  xdg_toplevel_destroy(window->xdg_toplevel);
rollback_xdg_surface:
  xdg_surface_destroy(window->xdg_surface);
rollback_zwp_relative_pointer_v1:
  if (window->zwp_relative_pointer_v1)
    zwp_relative_pointer_v1_destroy(window->zwp_relative_pointer_v1);
rollback_zwp_locked_pointer_v1:
  if (window->zwp_locked_pointer_v1)
    zwp_locked_pointer_v1_destroy(window->zwp_locked_pointer_v1);
rollback_globals:
  if (window->zwp_relative_pointer_manager_v1) {
    zwp_relative_pointer_manager_v1_destroy(
        window->zwp_relative_pointer_manager_v1);
  }
  if (window->zwp_pointer_constraints_v1)
    zwp_pointer_constraints_v1_destroy(window->zwp_pointer_constraints_v1);
  if (window->zwp_linux_dmabuf_v1)
    zwp_linux_dmabuf_v1_destroy(window->zwp_linux_dmabuf_v1);
  if (window->xdg_wm_base) xdg_wm_base_destroy(window->xdg_wm_base);
  if (window->wl_keyboard) wl_keyboard_release(window->wl_keyboard);
  if (window->wl_pointer) wl_pointer_release(window->wl_pointer);
  if (window->wl_surface) wl_surface_destroy(window->wl_surface);
rollback_wl_registry:
  wl_registry_destroy(wl_registry);
rollback_wl_display:
  wl_display_disconnect(window->wl_display);
rollback_window:
  free(window);
  return NULL;
}

int WindowGetEventsFd(const struct Window* window) {
  int events_fd = wl_display_get_fd(window->wl_display);
  if (events_fd == -1) LOG("Failed to get wl_display fd (%s)", strerror(errno));
  return events_fd;
}

bool WindowProcessEvents(const struct Window* window) {
  bool result = wl_display_dispatch(window->wl_display) != -1;
  if (!result) LOG("Failed to dispatch wl_display (%s)", strerror(errno));
  return result && !window->was_closed;
}

static void DestroyBuffers(struct Window* window) {
  if (!window->wl_buffers) return;
  for (size_t i = 0; window->wl_buffers[i]; i++)
    wl_buffer_destroy(window->wl_buffers[i]);
  free(window->wl_buffers);
  window->wl_buffers = NULL;
}

static struct wl_buffer* CreateBuffer(struct Window* window,
                                      const struct Frame* frame) {
  struct zwp_linux_buffer_params_v1* zwp_linux_buffer_params_v1 =
      zwp_linux_dmabuf_v1_create_params(window->zwp_linux_dmabuf_v1);
  if (!zwp_linux_buffer_params_v1) {
    LOG("Failed to create zwp_linux_buffer_params_v1 (%s)", strerror(errno));
    return NULL;
  }
  for (uint32_t i = 0; i < frame->nplanes; i++) {
    zwp_linux_buffer_params_v1_add(
        zwp_linux_buffer_params_v1, frame->planes[i].dmabuf_fd, i,
        frame->planes[i].offset, frame->planes[i].pitch,
        frame->planes[i].modifier >> 32,
        frame->planes[i].modifier & UINT32_MAX);
  }
  struct wl_buffer* wl_buffer = zwp_linux_buffer_params_v1_create_immed(
      zwp_linux_buffer_params_v1, (int)frame->width, (int)frame->height,
      frame->fourcc, 0);
  zwp_linux_buffer_params_v1_destroy(zwp_linux_buffer_params_v1);
  if (!wl_buffer) LOG("Failed to create wl_buffer (%s)", strerror(errno));
  return wl_buffer;
}

bool WindowAssignFrames(struct Window* window, size_t nframes,
                        const struct Frame* frames) {
  DestroyBuffers(window);
  window->wl_buffers = calloc(nframes + 1, sizeof(struct wl_buffer*));
  if (!window->wl_buffers) {
    LOG("Failed to alloc window buffers (%s)", strerror(errno));
    return false;
  }
  for (size_t i = 0; i < nframes; i++) {
    window->wl_buffers[i] = CreateBuffer(window, &frames[i]);
    if (!window->wl_buffers[i]) {
      LOG("Failed to create window buffer");
      DestroyBuffers(window);
      return false;
    }
  }
  return true;
}

bool WindowShowFrame(struct Window* window, size_t index) {
  wl_surface_attach(window->wl_surface, window->wl_buffers[index], 0, 0);
  wl_surface_damage(window->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(window->wl_surface);
  bool result = wl_display_roundtrip(window->wl_display) != -1;
  if (!result) LOG("Failed to roundtrip wl_display (%s)", strerror(errno));
  return result;
}

void WindowDestroy(struct Window* window) {
  DestroyBuffers(window);
  xdg_toplevel_destroy(window->xdg_toplevel);
  xdg_surface_destroy(window->xdg_surface);
  if (window->zwp_relative_pointer_v1)
    zwp_relative_pointer_v1_destroy(window->zwp_relative_pointer_v1);
  if (window->zwp_locked_pointer_v1)
    zwp_locked_pointer_v1_destroy(window->zwp_locked_pointer_v1);
  if (window->zwp_relative_pointer_manager_v1) {
    zwp_relative_pointer_manager_v1_destroy(
        window->zwp_relative_pointer_manager_v1);
  }
  if (window->zwp_pointer_constraints_v1)
    zwp_pointer_constraints_v1_destroy(window->zwp_pointer_constraints_v1);
  zwp_linux_dmabuf_v1_destroy(window->zwp_linux_dmabuf_v1);
  xdg_wm_base_destroy(window->xdg_wm_base);
  if (window->wl_keyboard) wl_keyboard_release(window->wl_keyboard);
  if (window->wl_pointer) wl_pointer_release(window->wl_pointer);
  wl_surface_destroy(window->wl_surface);
  wl_display_disconnect(window->wl_display);
  free(window);
}
