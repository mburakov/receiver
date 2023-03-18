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

#include "frame.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

struct Frame* FrameCreate(uint32_t width, uint32_t height, uint32_t fourcc,
                          size_t nplanes, const struct FramePlane* planes) {
  struct AUTO(Frame)* frame = malloc(sizeof(struct Frame));
  if (!frame) {
    LOG("Failed to allocate frame (%s)", strerror(errno));
    return NULL;
  }
  *frame = (struct Frame){
      .width = width,
      .height = height,
      .fourcc = fourcc,
      .nplanes = nplanes,
      .planes[0] = {.dmabuf_fd = -1, .pitch = 0, .offset = 0, .modifier = 0},
      .planes[1] = {.dmabuf_fd = -1, .pitch = 0, .offset = 0, .modifier = 0},
      .planes[2] = {.dmabuf_fd = -1, .pitch = 0, .offset = 0, .modifier = 0},
      .planes[3] = {.dmabuf_fd = -1, .pitch = 0, .offset = 0, .modifier = 0},
  };

  for (size_t i = 0; i < nplanes; i++) {
    frame->planes[i] = (struct FramePlane){
        .dmabuf_fd = dup(planes[i].dmabuf_fd),
        .pitch = planes[i].pitch,
        .offset = planes[i].offset,
        .modifier = planes[i].modifier,
    };
    if (frame->planes[i].dmabuf_fd == -1) {
      LOG("Failed to dup dmabuf fd (%s)", strerror(errno));
      return NULL;
    }
  }

  return RELEASE(frame);
}

void FrameDestroy(struct Frame** frame) {
  if (!frame || !*frame) return;
  for (size_t i = (*frame)->nplanes; i; i--) {
    if ((*frame)->planes[i].dmabuf_fd != -1)
      close((*frame)->planes[i].dmabuf_fd);
  }
}
