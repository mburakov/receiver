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

struct Window* WindowCreate(void);
int WindowGetEventsFd(const struct Window* window);
bool WindowProcessEvents(const struct Window* window);
bool WindowAssignFrames(struct Window* window, size_t nframes,
                        const struct Frame* frames);
bool WindowShowFrame(struct Window* window, size_t index);
void WindowDestroy(struct Window** window);

#endif  // RECEIVER_WINDOW_H_
