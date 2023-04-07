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

#ifndef RECEIVER_INPUT_H_
#define RECEIVER_INPUT_H_

#include <stdbool.h>

struct InputStream;

struct InputStream* InputStreamCreate(int fd);
bool InputStreamKeyPress(struct InputStream* input_stream, unsigned evdev_code,
                         bool pressed);
bool InputStreamMouseMove(struct InputStream* input_stream, int dx, int dy);
bool InputStreamMouseButton(struct InputStream* input_stream, unsigned button,
                            bool pressed);
bool InputStreamMouseWheel(struct InputStream* input_stream, int delta);
bool InputStreamHandsoff(struct InputStream* input_stream);
void InputStreamDestroy(struct InputStream* input_stream);

#endif  // RECEIVER_INPUT_H_
