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

#ifndef RECEIVER_AUDIO_H_
#define RECEIVER_AUDIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct AudioContext;

struct AudioContext* AudioContextCreate(size_t queue_size);
bool AudioContextDecode(struct AudioContext* audio_context, const void* buffer,
                        size_t size);
uint64_t AudioContextGetLatency(struct AudioContext* audio_context);
void AudioContextDestroy(struct AudioContext* audio_context);

#endif  // RECEIVER_AUDIO_H_
