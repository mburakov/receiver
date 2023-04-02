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

#ifndef RECEIVER_FRAME_H_
#define RECEIVER_FRAME_H_

#include <stddef.h>
#include <stdint.h>

struct FramePlane {
  int dmabuf_fd;
  uint32_t pitch;
  uint32_t offset;
  uint64_t modifier;
};

struct Frame {
  uint32_t width;
  uint32_t height;
  uint32_t fourcc;
  uint32_t nplanes;
  struct FramePlane planes[4];
};

struct Frame* FrameCreate(uint32_t width, uint32_t height, uint32_t fourcc,
                          uint32_t nplanes, const struct FramePlane* planes);
void FrameDestroy(struct Frame** frame);

#endif  // RECEIVER_FRAME_H_
