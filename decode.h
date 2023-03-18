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

#ifndef RECEIVER_DECODE_H_
#define RECEIVER_DECODE_H_

#include <stdbool.h>
#include <stddef.h>

struct DecodeContext;
struct Frame;

struct DecodeContext* DecodeContextCreate(void);
bool DecodeContextDecode(struct DecodeContext* decode_context, int fd);
const struct Frame* DecodeContextGetFrame(struct DecodeContext* decode_context);
void DecodeContextDestroy(struct DecodeContext** decode_context);

#endif  // RECEIVER_DECODE_H_
