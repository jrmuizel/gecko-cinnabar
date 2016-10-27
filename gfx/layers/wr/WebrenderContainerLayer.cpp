/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrenderContainerLayer.h"
#include "LayersLogging.h"

namespace mozilla {
namespace layers {

void
WebRenderContainerLayer::RenderLayer(wrstate* aWRState)
{
  WRScrollFrameStackingContextGenerator scrollFrames(aWRState, this);

  AutoTArray<Layer*, 12> children;
  SortChildrenBy3DZOrder(children);

  Rect relBounds = TransformedVisibleBoundsRelativeToParent();
  Matrix4x4 transform;// = GetTransform();
  if (gfxPrefs::LayersDump()) printf_stderr("ContainerLayer %p using %s as bounds/overflow, %s as transform\n", this, Stringify(relBounds).c_str(), Stringify(transform).c_str());

  wr_push_dl_builder(aWRState);
  for (Layer* child : children) {
    ToWebRenderLayer(child)->RenderLayer(aWRState);
  }
  wr_pop_dl_builder(aWRState, toWrRect(relBounds), toWrRect(relBounds), &transform.components[0], FrameMetrics::NULL_SCROLL_ID);
}

} // namespace layers
} // namespace mozilla
