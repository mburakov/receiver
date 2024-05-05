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

#ifndef ATOMIC_QUEUE_H_
#define ATOMIC_QUEUE_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

struct AtomicQueue {
  void* buffer;
  size_t alloc;
  size_t read;
  size_t write;
  atomic_size_t size;
};

bool AtomicQueueCreate(struct AtomicQueue* atomic_queue, size_t alloc);
size_t AtomicQueueWrite(struct AtomicQueue* atomic_queue, const void* buffer,
                        size_t size);
size_t AtomicQueueRead(struct AtomicQueue* atomic_queue, void* buffer,
                       size_t size);
void AtomicQueueDestroy(struct AtomicQueue* atomic_queue);

#endif  // ATOMIC_QUEUE_H_
