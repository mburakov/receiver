/*
 * Copyright (C) 2024 Mikhail Burakov. This file is part of receiver.
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

#ifndef MFX_STUB_BITSTREAM_H_
#define MFX_STUB_BITSTREAM_H_

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BitstreamCreate(a, b) \
  (struct Bitstream) { .data = a, .size = b }
#define BitstreamReadFailed(x) setjmp((x)->trap)

struct Bitstream {
  const uint8_t* data;
  size_t size;
  size_t offset;
  size_t epb_count;
  uint32_t cache;
  jmp_buf trap;
};

uint64_t BitstreamReadU(struct Bitstream* bitstream, size_t size);
uint64_t BitstreamReadUE(struct Bitstream* bitstream);
int64_t BitstreamReadSE(struct Bitstream* bitstream);
void BitstreamByteAlign(struct Bitstream* bitstream);
bool BitstreamReadNalu(struct Bitstream* bitstream, struct Bitstream* output);
bool BitstreamAvail(const struct Bitstream* bitstream);

#endif  // MFX_STUB_BITSTREAM_H_
