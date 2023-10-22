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

#ifndef RECEIVER_WINDOW_H_
#define RECEIVER_WINDOW_H_

#include <stdbool.h>
#include <stddef.h>

struct Window;
struct Frame;
struct Overlay;

struct WindowEventHandlers {
  void (*OnClose)(void* user);
  void (*OnFocus)(void* user, bool focused);
  void (*OnKey)(void* user, unsigned key, bool pressed);
  void (*OnMove)(void* user, int dx, int dy);
  void (*OnButton)(void* user, unsigned button, bool pressed);
  void (*OnWheel)(void* user, int delta);
};

struct Window* WindowCreate(
    const struct WindowEventHandlers* window_event_handlers, void* user);
int WindowGetEventsFd(const struct Window* window);
bool WindowProcessEvents(const struct Window* window);
bool WindowAssignFrames(struct Window* window, size_t nframes,
                        const struct Frame* frames);
bool WindowShowFrame(struct Window* window, size_t index, int x, int y,
                     int width, int height);
void WindowDestroy(struct Window* window);

struct Overlay* OverlayCreate(const struct Window* window, int x, int y,
                              int width, int height);
void* OverlayLock(struct Overlay* overlay);
void OverlayUnlock(struct Overlay* overlay);
void OverlayDestroy(struct Overlay* overlay);

#endif  // RECEIVER_WINDOW_H_
