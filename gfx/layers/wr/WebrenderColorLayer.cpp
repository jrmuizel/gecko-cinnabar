/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrenderColorLayer.h"
#include "webrender.h"

namespace mozilla {
namespace layers {

void
WebRenderColorLayer::RenderLayer(wrstate* aWRState)
{
  printf("rendering color layer %p\n", this);
  wr_dp_push_rect(aWRState, mBounds.x, mBounds.y, mBounds.width, mBounds.height,
                  mColor.r, mColor.g, mColor.b, mColor.a);
}

} // namespace layers
} // namespace mozilla
