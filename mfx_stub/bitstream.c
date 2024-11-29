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

#include "bitstream.h"

#include <string.h>

static uint64_t BitstreamReadBit(struct Bitstream* bitstream) {
  size_t shift = 7 - (bitstream->offset & 0x7);
  if (shift == 7) {
    size_t byte;
  repeat:
    byte = bitstream->offset >> 3;
    if (byte >= bitstream->size) longjmp(bitstream->trap, 1);
    bitstream->cache = (bitstream->cache << 8) | bitstream->data[byte];
    if (bitstream->offset >= 24 && (bitstream->cache & 0xffffff) == 3) {
      bitstream->offset += 8;
      bitstream->epb_count++;
      goto repeat;
    }
  }
  bitstream->offset++;
  return bitstream->cache >> shift & 0x1;
}

uint64_t BitstreamReadU(struct Bitstream* bitstream, size_t size) {
  uint64_t result = 0;
  for (; size; size--) {
    result = (result << 1) | BitstreamReadBit(bitstream);
  }
  return result;
}

uint64_t BitstreamReadUE(struct Bitstream* bitstream) {
  size_t size = 0;
  while (!BitstreamReadBit(bitstream)) size++;
  return (BitstreamReadU(bitstream, size) | (1 << size)) - 1;
}

int64_t BitstreamReadSE(struct Bitstream* bitstream) {
  uint64_t result = BitstreamReadUE(bitstream);
  int64_t sign = (int64_t)((result & 1) << 1) - 1;
  return sign * (int64_t)((result + 1) >> 1);
}

void BitstreamByteAlign(struct Bitstream* bitstream) {
  bitstream->offset = (bitstream->offset + 7) & ~(size_t)7;
}

bool BitstreamReadNalu(struct Bitstream* bitstream, struct Bitstream* output) {
  if (bitstream->offset & 0x7) return false;
  size_t byte_offset = bitstream->offset >> 3;
  static const uint8_t kStartCodePrefix[] = {0, 0, 0, 1};
  if (bitstream->size - byte_offset < sizeof(kStartCodePrefix)) {
    return false;
  }
  const uint8_t* data = bitstream->data + byte_offset;
  if (memcmp(bitstream->data + byte_offset, kStartCodePrefix,
             sizeof(kStartCodePrefix))) {
    return false;
  }
  data += sizeof(kStartCodePrefix);
  byte_offset += sizeof(kStartCodePrefix);
  size_t max_size = bitstream->size - byte_offset;
  uint8_t* next =
      memmem(data, max_size, kStartCodePrefix, sizeof(kStartCodePrefix));
  size_t size = next ? (size_t)(next - data) : max_size;
  bitstream->offset = (byte_offset + size) << 3;
  *output = BitstreamCreate(data, size);
  return true;
}

bool BitstreamAvail(const struct Bitstream* bitstream) {
  return bitstream->offset < (bitstream->size << 3);
}
