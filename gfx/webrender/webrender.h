/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Simple class for computing SHA1. */

#ifndef WR_h
#define WR_h
extern "C" {
enum WRImageFormat {
    Invalid,
    A8,
    RGB8,
    RGBA8,
    RGBAF32
};

struct WRImageKey {
  uint32_t a;
  uint32_t b;
};

struct wrstate;

wrstate* wr_create(uint32_t width, uint32_t height, uint32_t counter);
void wr_render(wrstate* wrstate);
void wr_destroy(wrstate* wrstate);
WRImageKey wr_add_image(wrstate* wrstate, uint32_t width, uint32_t height,
                        WRImageFormat format, uint8_t *bytes, size_t size);
void wr_delete_image(wrstate* wrstate, WRImageKey key);


void wr_dp_begin(wrstate* wrState, uint32_t width, uint32_t height);
void wr_dp_end(wrstate* wrState);
void wr_dp_push_rect(wrstate* wrState, float x, float y, float w, float h, float r, float g, float b, float a);
void wr_dp_push_image(wrstate* wrState, float x, float y, float w, float h, WRImageKey key);

}
#endif
