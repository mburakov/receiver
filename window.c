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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "frame.h"
#include "linux-dmabuf-unstable-v1.h"
#include "pointer-constraints-unstable-v1.h"
#include "relative-pointer-unstable-v1.h"
#include "toolbox/utils.h"
#include "viewporter.h"
#include "xdg-shell.h"

#define OVERLAY_BUFFERS_COUNT 2

// TODO(mburakov): This would look like shit until Wayland guys finally fix
// https://gitlab.freedesktop.org/wayland/wayland/-/issues/160

struct Window {
  const struct WindowEventHandlers* event_handlers;
  void* user;

  // Wayland globals
  struct wl_display* wl_display;
  struct wl_registry* wl_registry;
  struct wl_compositor* wl_compositor;
  struct wl_shm* wl_shm;
  struct wl_seat* wl_seat;
  struct wl_subcompositor* wl_subcompositor;
  struct wp_viewporter* wp_viewporter;
  struct xdg_wm_base* xdg_wm_base;
  struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf_v1;
  struct zwp_pointer_constraints_v1* zwp_pointer_constraints_v1;
  struct zwp_relative_pointer_manager_v1* zwp_relative_pointer_manager_v1;

  // Wayland toplevel
  struct wl_surface* wl_surface;
  struct wp_viewport* wp_viewport;
  struct xdg_surface* xdg_surface;
  struct xdg_toplevel* xdg_toplevel;

  // Wayland inputs
  struct wl_keyboard* wl_keyboard;
  struct wl_pointer* wl_pointer;
  struct zwp_relative_pointer_v1* zwp_relative_pointer_v1;
  struct zwp_locked_pointer_v1* zwp_locked_pointer_v1;

  // Wayland dynamics
  size_t wl_buffers_count;
  struct wl_buffer** wl_buffers;
  int32_t window_width;
  int32_t window_height;
  bool was_closed;
};

struct Overlay {
  int width;
  int height;
  int shm_fd;
  void* shm_buffer;
  struct wl_surface* wl_surface;
  struct wl_subsurface* wl_subsurface;
  struct wl_shm_pool* wl_shm_pool;
  struct wl_buffer* wl_buffers[OVERLAY_BUFFERS_COUNT];
  size_t wl_buffer_current;
};

static void OnWlRegistryGlobal(void* data, struct wl_registry* wl_registry,
                               uint32_t name, const char* interface,
                               uint32_t version) {
  (void)version;
#define MAYBE_BIND(what, ver)                                                 \
  if (!strcmp(interface, what##_interface.name)) {                            \
    window->what =                                                            \
        wl_registry_bind(wl_registry, name, &what##_interface, ver);          \
    if (!window->what) LOG("Failed to bind " #what " (%s)", strerror(errno)); \
    return;                                                                   \
  }
  struct Window* window = data;
  MAYBE_BIND(wl_compositor, 1)
  MAYBE_BIND(wl_shm, 1)
  MAYBE_BIND(wl_seat, 8)
  MAYBE_BIND(wl_subcompositor, 1)
  MAYBE_BIND(wp_viewporter, 1)
  MAYBE_BIND(xdg_wm_base, 1)
  MAYBE_BIND(zwp_linux_dmabuf_v1, 2)
  MAYBE_BIND(zwp_pointer_constraints_v1, 1)
  MAYBE_BIND(zwp_relative_pointer_manager_v1, 1)
#undef MAYBE_BIND
}

static void OnWlRegistryGlobalRemove(void* data, struct wl_registry* registry,
                                     uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static void OnXdgWmBasePing(void* data, struct xdg_wm_base* xdg_wm_base,
                            uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(xdg_wm_base, serial);
}

static bool InitWaylandGlobals(struct Window* window) {
  window->wl_display = wl_display_connect(NULL);
  if (!window->wl_display) {
    LOG("Failed to connect wl_display (%s)", strerror(errno));
    return false;
  }

  window->wl_registry = wl_display_get_registry(window->wl_display);
  if (!window->wl_registry) {
    LOG("Failed to get wl_registry (%s)", strerror(errno));
    goto rollback_wl_display;
  }

  static const struct wl_registry_listener wl_registry_listener = {
      .global = OnWlRegistryGlobal,
      .global_remove = OnWlRegistryGlobalRemove,
  };
  if (wl_registry_add_listener(window->wl_registry, &wl_registry_listener,
                               window)) {
    LOG("Failed to add wl_registry listener (%s)", strerror(errno));
    goto rollback_wl_registry;
  }
  if (wl_display_roundtrip(window->wl_display) == -1) {
    LOG("Failed to roundtrip wl_display (%s)", strerror(errno));
    goto rollback_wl_registry;
  }

  if (!window->wl_compositor || !window->wl_shm || !window->wl_seat ||
      !window->wl_subcompositor || !window->wp_viewporter ||
      !window->xdg_wm_base || !window->zwp_linux_dmabuf_v1 ||
      !window->zwp_pointer_constraints_v1 ||
      !window->zwp_relative_pointer_manager_v1) {
    LOG("Some required wayland globals are missing");
    goto rollback_globals;
  }
  static const struct xdg_wm_base_listener xdg_wm_base_listener = {
      .ping = OnXdgWmBasePing,
  };
  if (xdg_wm_base_add_listener(window->xdg_wm_base, &xdg_wm_base_listener,
                               NULL)) {
    LOG("Failed to add xdg_wm_base listener (%s)", strerror(errno));
    goto rollback_globals;
  }
  return true;

rollback_globals:
  if (window->zwp_relative_pointer_manager_v1)
    zwp_relative_pointer_manager_v1_destroy(
        window->zwp_relative_pointer_manager_v1);
  if (window->zwp_pointer_constraints_v1)
    zwp_pointer_constraints_v1_destroy(window->zwp_pointer_constraints_v1);
  if (window->zwp_linux_dmabuf_v1)
    zwp_linux_dmabuf_v1_destroy(window->zwp_linux_dmabuf_v1);
  if (window->xdg_wm_base) xdg_wm_base_destroy(window->xdg_wm_base);
  if (window->wp_viewporter) wp_viewporter_destroy(window->wp_viewporter);
  if (window->wl_subcompositor)
    wl_subcompositor_destroy(window->wl_subcompositor);
  if (window->wl_seat) wl_seat_destroy(window->wl_seat);
  if (window->wl_shm) wl_shm_destroy(window->wl_shm);
  if (window->wl_compositor) wl_compositor_destroy(window->wl_compositor);
rollback_wl_registry:
  wl_registry_destroy(window->wl_registry);
rollback_wl_display:
  wl_display_disconnect(window->wl_display);
  return false;
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
  (void)xdg_toplevel;
  (void)states;
  struct Window* window = data;
  if (width && height) {
    window->window_width = width;
    window->window_height = height;
  }
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

static bool InitWaylandToplevel(struct Window* window) {
  window->wl_surface = wl_compositor_create_surface(window->wl_compositor);
  if (!window->wl_surface) {
    LOG("Failed to create wl_surface (%s)", strerror(errno));
    return false;
  }

  window->wp_viewport =
      wp_viewporter_get_viewport(window->wp_viewporter, window->wl_surface);
  if (!window->wp_viewport) {
    LOG("Failed to get wp_viewport (%s)", strerror(errno));
    goto rollback_wl_surface;
  }

  window->xdg_surface =
      xdg_wm_base_get_xdg_surface(window->xdg_wm_base, window->wl_surface);
  if (!window->xdg_surface) {
    LOG("Failed to get xdg_surface (%s)", strerror(errno));
    goto rollback_wp_viewport;
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
  };
  if (xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener,
                                window)) {
    LOG("Failed to add xdg_toplevel listener (%s)", strerror(errno));
    goto rollback_xdg_toplevel;
  }
  return true;

rollback_xdg_toplevel:
  xdg_toplevel_destroy(window->xdg_toplevel);
rollback_xdg_surface:
  xdg_surface_destroy(window->xdg_surface);
rollback_wp_viewport:
  wp_viewport_destroy(window->wp_viewport);
rollback_wl_surface:
  wl_surface_destroy(window->wl_surface);
  return false;
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

static bool InitWaylandInputs(struct Window* window) {
  window->wl_keyboard = wl_seat_get_keyboard(window->wl_seat);
  if (!window->wl_keyboard) {
    LOG("Failed to get wl_keyboard (%s)", strerror(errno));
    return false;
  }

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
    goto rollback_wl_keyboard;
  }
  window->wl_pointer = wl_seat_get_pointer(window->wl_seat);
  if (!window->wl_pointer) {
    LOG("Failed to get wl_pointer (%s)", strerror(errno));
    goto rollback_wl_keyboard;
  }

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
    goto rollback_wl_pointer;
  }
  window->zwp_locked_pointer_v1 = zwp_pointer_constraints_v1_lock_pointer(
      window->zwp_pointer_constraints_v1, window->wl_surface,
      window->wl_pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
  if (!window->zwp_locked_pointer_v1) {
    LOG("Failed to lock wl_pointer (%s)", strerror(errno));
    goto rollback_wl_pointer;
  }

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
    LOG("Failed to add zwp_relative_pointer_v1 listener (%s)", strerror(errno));
    goto rollback_zwp_relative_pointer_v1;
  }
  return true;

rollback_zwp_relative_pointer_v1:
  zwp_relative_pointer_v1_destroy(window->zwp_relative_pointer_v1);
rollback_zwp_locked_pointer_v1:
  zwp_locked_pointer_v1_destroy(window->zwp_locked_pointer_v1);
rollback_wl_pointer:
  wl_pointer_destroy(window->wl_pointer);
rollback_wl_keyboard:
  wl_keyboard_destroy(window->wl_keyboard);
  return false;
}

static void DeinitWaylandInputs(struct Window* window) {
  zwp_relative_pointer_v1_destroy(window->zwp_relative_pointer_v1);
  zwp_locked_pointer_v1_destroy(window->zwp_locked_pointer_v1);
  wl_pointer_destroy(window->wl_pointer);
  wl_keyboard_destroy(window->wl_keyboard);
}

static void DeinitWaylandToplevel(struct Window* window) {
  xdg_toplevel_destroy(window->xdg_toplevel);
  xdg_surface_destroy(window->xdg_surface);
  wp_viewport_destroy(window->wp_viewport);
  wl_surface_destroy(window->wl_surface);
}

static void DeinitWaylandGlobals(struct Window* window) {
  zwp_relative_pointer_manager_v1_destroy(
      window->zwp_relative_pointer_manager_v1);
  zwp_pointer_constraints_v1_destroy(window->zwp_pointer_constraints_v1);
  zwp_linux_dmabuf_v1_destroy(window->zwp_linux_dmabuf_v1);
  xdg_wm_base_destroy(window->xdg_wm_base);
  wp_viewporter_destroy(window->wp_viewporter);
  wl_subcompositor_destroy(window->wl_subcompositor);
  wl_seat_destroy(window->wl_seat);
  wl_shm_destroy(window->wl_shm);
  wl_compositor_destroy(window->wl_compositor);
  wl_registry_destroy(window->wl_registry);
  wl_display_disconnect(window->wl_display);
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

  if (!InitWaylandGlobals(window)) {
    LOG("Failed to initialize wayland globals");
    goto rollback_window;
  }

  if (!InitWaylandToplevel(window)) {
    LOG("Failed to initialize wayland toplevel");
    goto rollback_wayland_globals;
  }

  if (window->event_handlers && !InitWaylandInputs(window)) {
    LOG("Failed to initialize wayland inputs");
    goto rollback_wayland_toplevel;
  }

  xdg_toplevel_set_fullscreen(window->xdg_toplevel, NULL);
  wl_surface_commit(window->wl_surface);
  if (wl_display_roundtrip(window->wl_display) == -1) {
    LOG("Failed to roundtrip wl_display (%s)", strerror(errno));
    goto rollback_wayland_inputs;
  }
  if (window->event_handlers)
    zwp_locked_pointer_v1_set_region(window->zwp_locked_pointer_v1, NULL);
  return window;

rollback_wayland_inputs:
  if (window->event_handlers) DeinitWaylandInputs(window);
rollback_wayland_toplevel:
  DeinitWaylandToplevel(window);
rollback_wayland_globals:
  DeinitWaylandGlobals(window);
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
  for (; window->wl_buffers_count; window->wl_buffers_count--)
    wl_buffer_destroy(window->wl_buffers[window->wl_buffers_count - 1]);
  free(window->wl_buffers);
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
  window->wl_buffers = malloc(nframes * sizeof(struct wl_buffer*));
  if (!window->wl_buffers) {
    LOG("Failed to alloc window buffers (%s)", strerror(errno));
    return false;
  }
  for (; window->wl_buffers_count != nframes; window->wl_buffers_count++) {
    window->wl_buffers[window->wl_buffers_count] =
        CreateBuffer(window, &frames[window->wl_buffers_count]);
    if (!window->wl_buffers[window->wl_buffers_count]) {
      LOG("Failed to create window buffer");
      goto rollback_buffers;
    }
  }
  return true;

rollback_buffers:
  DestroyBuffers(window);
  return false;
}

bool WindowShowFrame(struct Window* window, size_t index, int x, int y,
                     int width, int height) {
  wp_viewport_set_source(window->wp_viewport, wl_fixed_from_int(x),
                         wl_fixed_from_int(y), wl_fixed_from_int(width),
                         wl_fixed_from_int(height));
  if (window->window_width && window->window_height) {
    wp_viewport_set_destination(window->wp_viewport, window->window_width,
                                window->window_height);
  }
  wl_surface_attach(window->wl_surface, window->wl_buffers[index], 0, 0);
  wl_surface_damage(window->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(window->wl_surface);
  bool result = wl_display_roundtrip(window->wl_display) != -1;
  if (!result) LOG("Failed to roundtrip wl_display (%s)", strerror(errno));
  return result;
}

void WindowDestroy(struct Window* window) {
  DestroyBuffers(window);
  if (window->event_handlers) DeinitWaylandInputs(window);
  DeinitWaylandToplevel(window);
  DeinitWaylandGlobals(window);
  free(window);
}

struct Overlay* OverlayCreate(const struct Window* window, int x, int y,
                              int width, int height) {
  ssize_t stride = width * 4;
  ssize_t buffer_size = stride * height;
  ssize_t pool_size = OVERLAY_BUFFERS_COUNT * buffer_size;
  if (pool_size > INT32_MAX || width < 0 || height < 0) {
    LOG("Suspicious overlay size %ux%u", width, height);
    return NULL;
  }

  struct Overlay* overlay = malloc(sizeof(struct Overlay));
  if (!overlay) {
    LOG("Failed to allocate overlay (%s)", strerror(errno));
    return NULL;
  }
  *overlay = (struct Overlay){
      .width = width,
      .height = height,
      .shm_fd = -1,
  };

  char name[64];
  static size_t counter = 0;
  snprintf(name, sizeof(name), "/wl_shm-%d-%zu", getpid(), counter++);
  overlay->shm_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (overlay->shm_fd == -1) {
    LOG("Failed to open shm (%s)", strerror(errno));
    goto rollback_overlay;
  }
  shm_unlink(name);

  if (ftruncate(overlay->shm_fd, pool_size) == -1) {
    LOG("Failed to truncate shm (%s)", strerror(errno));
    goto rollback_shm_fd;
  }
  overlay->shm_buffer = mmap(NULL, (size_t)pool_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, overlay->shm_fd, 0);
  if (!overlay->shm_buffer) {
    LOG("Failed to mmap shm (%s)", strerror(errno));
    goto rollback_shm_fd;
  }

  overlay->wl_surface = wl_compositor_create_surface(window->wl_compositor);
  if (!overlay->wl_surface) {
    LOG("Failed to create wl_surface (%s)", strerror(errno));
    goto rollback_shm_buffer;
  }

  overlay->wl_subsurface = wl_subcompositor_get_subsurface(
      window->wl_subcompositor, overlay->wl_surface, window->wl_surface);
  if (!overlay->wl_subsurface) {
    LOG("Failed to create wl_subsurface (%s)", strerror(errno));
    goto rollback_wl_surface;
  }
  wl_subsurface_place_above(overlay->wl_subsurface, window->wl_surface);
  wl_subsurface_set_position(overlay->wl_subsurface, x, y);

  overlay->wl_shm_pool =
      wl_shm_create_pool(window->wl_shm, overlay->shm_fd, (int32_t)pool_size);
  if (!overlay->wl_shm_pool) {
    LOG("Failed to create wl_shm_pool (%s)", strerror(errno));
    goto rollback_wl_subsurface;
  }

  int i = 0;
  for (; i < OVERLAY_BUFFERS_COUNT; i++) {
    overlay->wl_buffers[i] = wl_shm_pool_create_buffer(
        overlay->wl_shm_pool, (int)(i * buffer_size), overlay->width,
        overlay->height, (int)stride, WL_SHM_FORMAT_ARGB8888);
    if (!overlay->wl_buffers[i]) {
      LOG("Failed to create wl_buffer (%s)", strerror(errno));
      goto rollback_wl_buffers;
    }
  }
  return overlay;

rollback_wl_buffers:
  for (; i; i--) wl_buffer_destroy(overlay->wl_buffers[i - 1]);
  wl_shm_pool_destroy(overlay->wl_shm_pool);
rollback_wl_subsurface:
  wl_subsurface_destroy(overlay->wl_subsurface);
rollback_wl_surface:
  wl_surface_destroy(overlay->wl_surface);
rollback_shm_buffer:
  munmap(overlay->shm_buffer, (size_t)pool_size);
rollback_shm_fd:
  close(overlay->shm_fd);
rollback_overlay:
  free(overlay);
  return NULL;
}

void* OverlayLock(struct Overlay* overlay) {
  ssize_t stride = overlay->width * 4;
  ssize_t buffer_size = stride * overlay->height;
  size_t next = (overlay->wl_buffer_current + 1) % OVERLAY_BUFFERS_COUNT;
  return (uint8_t*)overlay->shm_buffer + (size_t)buffer_size * next;
}

void OverlayUnlock(struct Overlay* overlay) {
  size_t next = (overlay->wl_buffer_current + 1) % OVERLAY_BUFFERS_COUNT;
  wl_surface_attach(overlay->wl_surface, overlay->wl_buffers[next], 0, 0);
  wl_surface_damage(overlay->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(overlay->wl_surface);
  overlay->wl_buffer_current = next;
}

void OverlayDestroy(struct Overlay* overlay) {
  for (int i = OVERLAY_BUFFERS_COUNT; i; i--)
    wl_buffer_destroy(overlay->wl_buffers[i - 1]);
  wl_shm_pool_destroy(overlay->wl_shm_pool);
  wl_subsurface_destroy(overlay->wl_subsurface);
  wl_surface_destroy(overlay->wl_surface);
  ssize_t pool_size =
      OVERLAY_BUFFERS_COUNT * overlay->width * overlay->height * 4;
  munmap(overlay->shm_buffer, (size_t)(pool_size));
  close(overlay->shm_fd);
  free(overlay);
}
