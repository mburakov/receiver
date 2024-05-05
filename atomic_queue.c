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

#include "atomic_queue.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t min(size_t a, size_t b) { return a < b ? a : b; }
static size_t min3(size_t a, size_t b, size_t c) { return min(min(a, b), c); }

bool AtomicQueueCreate(struct AtomicQueue* atomic_queue, size_t alloc) {
  void* buffer = malloc(alloc);
  if (!buffer) return false;
  *atomic_queue = (struct AtomicQueue){
      .buffer = buffer,
      .alloc = alloc,
  };
  atomic_init(&atomic_queue->size, 0);
  return true;
}

size_t AtomicQueueWrite(struct AtomicQueue* atomic_queue, const void* buffer,
                        size_t size) {
  size_t capacity =
      atomic_queue->alloc -
      atomic_load_explicit(&atomic_queue->size, memory_order_acquire);

  size_t tail_size = atomic_queue->alloc - atomic_queue->write;
  size_t copy_size = min3(size, capacity, tail_size);
  memcpy((uint8_t*)atomic_queue->buffer + atomic_queue->write, buffer,
         copy_size);

  size_t offset = copy_size;
  copy_size = min(size - copy_size, capacity - copy_size);
  memcpy(atomic_queue->buffer, (const uint8_t*)buffer + offset, copy_size);

  offset += copy_size;
  atomic_queue->write = (atomic_queue->write + offset) % atomic_queue->alloc;
  atomic_fetch_add_explicit(&atomic_queue->size, offset, memory_order_release);
  return offset;
}

size_t AtomicQueueRead(struct AtomicQueue* atomic_queue, void* buffer,
                       size_t size) {
  size_t avail =
      atomic_load_explicit(&atomic_queue->size, memory_order_acquire);

  size_t tail_size = atomic_queue->alloc - atomic_queue->read;
  size_t copy_size = min3(size, avail, tail_size);
  memcpy(buffer, (const uint8_t*)atomic_queue->buffer + atomic_queue->read,
         copy_size);

  size_t offset = copy_size;
  copy_size = min(size - copy_size, avail - copy_size);
  memcpy((uint8_t*)buffer + offset, atomic_queue->buffer, copy_size);

  offset += copy_size;
  atomic_queue->read = (atomic_queue->read + offset) % atomic_queue->alloc;
  atomic_fetch_sub_explicit(&atomic_queue->size, offset, memory_order_release);
  return offset;
}

void AtomicQueueDestroy(struct AtomicQueue* atomic_queue) {
  free(atomic_queue->buffer);
}
