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

#include "input.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <linux/uhid.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "toolbox/utils.h"

static const uint8_t evdev_to_hid[] = {
#define NOOP 0x00
    /* 0x00 */ NOOP, 0x29, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23,
    /* 0x08 */ 0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e, 0x2a, 0x2b,
    /* 0x10 */ 0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c,
    /* 0x18 */ 0x12, 0x13, 0x2f, 0x30, 0x28, 0xe0, 0x04, 0x16,
    /* 0x20 */ 0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f, 0x33,
    /* 0x28 */ 0x34, 0x35, 0xe1, 0x31, 0x1d, 0x1b, 0x06, 0x19,
    /* 0x30 */ 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xe5, 0x55,
    /* 0x38 */ 0xe2, 0x2c, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e,
    /* 0x40 */ 0x3f, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5f,
    /* 0x48 */ 0x60, 0x61, 0x56, 0x5c, 0x5d, 0x5e, 0x57, 0x59,
    /* 0x50 */ 0x5a, 0x5b, 0x62, 0x63, NOOP, 0x94, 0x64, 0x44,
    /* 0x58 */ 0x45, 0x87, 0x92, 0x93, 0x8a, 0x88, 0x8b, NOOP,
    /* 0x60 */ 0x58, 0xe4, 0x54, 0x46, 0xe6, NOOP, 0x4a, 0x52,
    /* 0x68 */ 0x4b, 0x50, 0x4f, 0x4d, 0x51, 0x4e, 0x49, 0x4c,
    /* 0x70 */ NOOP, 0x7f, 0x81, 0x80, 0x66, 0x67, 0xd7, 0x48,
    /* 0x78 */ NOOP, 0x85, 0x90, 0x91, 0x89, 0xe3, 0xe7, 0x65,
    /* 0x80 */ NOOP, 0x79, NOOP, 0x7a, 0x77, 0x7c, 0x74, 0x7d,
    /* 0x88 */ 0x7e, 0x7b, 0x75, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0x90 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0x98 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0xa0 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0xa8 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0xb0 */ NOOP, NOOP, NOOP, 0xb6, 0xb7, NOOP, NOOP, 0x68,
    /* 0xb8 */ 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70,
    /* 0xc0 */ 0x71, 0x72, 0x73, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0xc8 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0xd0 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0xd8 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0xe0 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0xe8 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0xf0 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
    /* 0xf8 */ NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP, NOOP,
#undef NOOP
};

static const struct uhid_event uhid_event_create2 = {
    .type = UHID_CREATE2,
    .u.create2.name = "Virtual input device",
    .u.create2.bus = BUS_USB,
    .u.create2.rd_size = 108,
    .u.create2.rd_data =
        {
            0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19,
            0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
            0x81, 0x02, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xdd, 0x00,
            0x05, 0x07, 0x19, 0x00, 0x29, 0xdd, 0x81, 0x00, 0xc0, 0x05, 0x01,
            0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00, 0x05,
            0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05,
            0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x05,
            0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f,
            0x75, 0x10, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25,
            0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
        },
};

struct InputStream {
  int fd;
  unsigned button_state;
  uint64_t key_state[4];
};

static bool Drain(int fd, const void* data, size_t size) {
  for (const uint8_t* ptr = data; size;) {
    ssize_t result = write(fd, ptr, size);
    if (result > 0) {
      ptr += result;
      size -= (size_t)result;
      continue;
    }
    if (errno != EINTR) {
      LOG("Failed to write data (%s)", strerror(errno));
      return false;
    }
  }
  return true;
}

struct InputStream* InputStreamCreate(int fd) {
  struct InputStream* input_stream = malloc(sizeof(struct InputStream));
  if (!input_stream) {
    LOG("Failed to allocate input stream (%s)", strerror(errno));
    return NULL;
  }
  *input_stream = (struct InputStream){
      .fd = fd,
  };

  size_t size = offsetof(struct uhid_event, u.create2.rd_data) +
                uhid_event_create2.u.create2.rd_size;
  if (!Drain(fd, &uhid_event_create2, size)) {
    LOG("Failed to drain create2 event");
    goto rollback_input_stream;
  }
  return input_stream;

rollback_input_stream:
  free(input_stream);
  return NULL;
}

static void RecordKeyCode(struct uhid_input2_req* input2, uint8_t code) {
  if (!code) return;
  if (input2->size == 1) {
    input2->data[1] = 0;
    input2->size = 2;
  }
  if (code >= 0xe0) {
    uint8_t shift = code - 0xe0;
    input2->data[1] |= 1 << shift;
  } else if (input2->size < 8) {
    input2->data[input2->size] = code;
    input2->size++;
  }
}

static size_t InputStreamFormatKeyboard(const struct InputStream* input_stream,
                                        struct uhid_event* uhid_event) {
  uhid_event->type = UHID_INPUT2;
  uhid_event->u.input2.data[0] = 1;
  uhid_event->u.input2.size = 1;
  size_t code_counter = 0;
  for (size_t row = 0; row < LENGTH(input_stream->key_state); row++) {
    for (uint64_t shift = 0; shift < sizeof(uint64_t) * 8; shift++) {
      if (input_stream->key_state[row] & (1ull << shift))
        RecordKeyCode(&uhid_event->u.input2, evdev_to_hid[code_counter]);
      code_counter++;
    }
  }

  // TODO(mburakov): Remove this?
  memset(uhid_event->u.input2.data + uhid_event->u.input2.size, 0,
         8 - uhid_event->u.input2.size);
  uhid_event->u.input2.size = 8;

  return offsetof(struct uhid_event, u.input2.data) + uhid_event->u.input2.size;
}

bool InputStreamKeyPress(struct InputStream* input_stream, unsigned evdev_code,
                         bool pressed) {
  size_t key_state_row = evdev_code >> 6 & 0x3;
  uint64_t key_state_shift = evdev_code & 0x3f;
  uint64_t key_state =
      (input_stream->key_state[key_state_row] & ~(1ull << key_state_shift)) |
      (((uint64_t) !!pressed) << key_state_shift);
  if (key_state == input_stream->key_state[key_state_row]) return true;
  input_stream->key_state[key_state_row] = key_state;

  struct uhid_event uhid_event_input2;
  size_t size = InputStreamFormatKeyboard(input_stream, &uhid_event_input2);
  bool result = Drain(input_stream->fd, &uhid_event_input2, size);
  if (!result) LOG("Failed to drain keypress");
  return result;
}

static size_t InputStreamFormatMouse(const struct InputStream* input_stream,
                                     struct uhid_event* uhid_event, int dx,
                                     int dy, int wheel) {
  uhid_event->type = UHID_INPUT2;
  uhid_event->u.input2.data[0] = 2;
  uhid_event->u.input2.data[1] = input_stream->button_state & 0xff;
  uhid_event->u.input2.data[2] = dx & 0xff;
  uhid_event->u.input2.data[3] = dx >> 8 & 0xff;
  uhid_event->u.input2.data[4] = dy & 0xff;
  uhid_event->u.input2.data[5] = dy >> 8 & 0xff;
  uhid_event->u.input2.data[6] = wheel & 0xff;
  uhid_event->u.input2.size = 7;
  return offsetof(struct uhid_event, u.input2.data) + uhid_event->u.input2.size;
}

bool InputStreamMouseMove(struct InputStream* input_stream, int dx, int dy) {
  struct uhid_event uhid_event_input2;
  size_t size =
      InputStreamFormatMouse(input_stream, &uhid_event_input2, dx, dy, 0);
  bool result = Drain(input_stream->fd, &uhid_event_input2, size);
  if (!result) LOG("Failed to drain mousemove");
  return result;
}

bool InputStreamMouseButton(struct InputStream* input_stream, unsigned button,
                            bool pressed) {
  unsigned button_shift;
  switch (button) {
    case BTN_LEFT:
      button_shift = 0;
      break;
    case BTN_RIGHT:
      button_shift = 1;
      break;
    case BTN_MIDDLE:
      button_shift = 2;
      break;
    default:
      // mburakov: Ignore unknown buttons...
      return true;
  }

  unsigned button_state = (input_stream->button_state & ~(1u << button_shift)) |
                          (((unsigned)!!pressed) << button_shift);
  if (button_state == input_stream->button_state) return true;
  input_stream->button_state = button_state;

  struct uhid_event uhid_event_input2;
  size_t size =
      InputStreamFormatMouse(input_stream, &uhid_event_input2, 0, 0, 0);
  bool result = Drain(input_stream->fd, &uhid_event_input2, size);
  if (!result) LOG("Failed to drain mousebutton");
  return result;
}

bool InputStreamMouseWheel(struct InputStream* input_stream, int delta) {
  struct uhid_event uhid_event_input2;
  size_t size =
      InputStreamFormatMouse(input_stream, &uhid_event_input2, 0, 0, delta);
  bool result = Drain(input_stream->fd, &uhid_event_input2, size);
  if (!result) LOG("Failed to drain mousewheel");
  return result;
}

bool InputStreamHandsoff(struct InputStream* input_stream) {
  memset(input_stream->key_state, 0, sizeof(input_stream->key_state));
  static const struct uhid_event uhid_event_input2 = {
      .type = UHID_INPUT2,
  };
  bool result = Drain(input_stream->fd, &uhid_event_input2,
                      sizeof(uhid_event_input2.type));
  if (!result) LOG("Failed to drain handsoff");
  return result;
}

void InputStreamDestroy(struct InputStream* input_stream) {
  static const struct uhid_event uhid_event_destroy = {
      .type = UHID_DESTROY,
  };
  Drain(input_stream->fd, &uhid_event_destroy, sizeof(uhid_event_destroy.type));
  free(input_stream);
}
