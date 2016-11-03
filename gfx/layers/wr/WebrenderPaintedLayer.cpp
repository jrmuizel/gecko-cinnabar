/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrenderPaintedLayer.h"

#include "LayersLogging.h"
#include "mozilla/ArrayUtils.h"
#include "gfxUtils.h"

namespace mozilla {
namespace layers {

using namespace mozilla::gfx;

void
WebRenderPaintedLayer::RenderLayer(wrstate* aWRState)
{
  LayerIntRegion visibleRegion = GetVisibleRegion();
  LayerIntRect bounds = visibleRegion.GetBounds();
  LayerIntSize size = bounds.Size();
  if (size.IsEmpty()) {
      printf("Empty region\n");
      return;
  }

  WRScrollFrameStackingContextGenerator scrollFrames(aWRState, this);
  wr_push_dl_builder(aWRState);

  RefPtr<DrawTarget> target = gfx::Factory::CreateDrawTarget(gfx::BackendType::SKIA, size.ToUnknownSize(), SurfaceFormat::B8G8R8A8);
  target->SetTransform(Matrix().PreTranslate(-bounds.x, -bounds.y));
  RefPtr<gfxContext> ctx = gfxContext::CreatePreservingTransformOrNull(target);
  MOZ_ASSERT(ctx); // already checked the target above

  Manager()->GetPaintedLayerCallback()(this,
                                       ctx,
                                       visibleRegion.ToUnknownRegion(), visibleRegion.ToUnknownRegion(),
                                       DrawRegionClip::DRAW, nsIntRegion(), Manager()->GetPaintedLayerCallbackData());
#if 0
  static int count;
  char buf[400];
  sprintf(buf, "wrout%d.png", count++);
  gfxUtils::WriteAsPNG(target, buf);
#endif

  WRImageKey key;
  {
      unsigned char* data;
      IntSize size;
      int32_t stride;
      SurfaceFormat format;
      target->LockBits(&data, &size, &stride, &format);
      key = wr_add_image(aWRState, size.width, size.height, RGBA8, 1, data, size.height * stride);

      target->ReleaseBits(data);
  }

  // Since we are creating a stacking context below using the visible region of
  // this layer, we need to make sure the image display item has coordinates
  // relative to the visible region.
  Rect rect = RelativeToVisible(IntRectToRect(bounds.ToUnknownRect()));
  Rect clip;
  if (GetClipRect().isSome()) {
      clip = RelativeToTransformedVisible(IntRectToRect(GetClipRect().ref().ToUnknownRect()));
  } else {
      clip = rect;
  }
  if (gfxPrefs::LayersDump()) printf_stderr("PaintedLayer %p using rect:%s clip:%s\n", this, Stringify(rect).c_str(), Stringify(clip).c_str());
  wr_dp_push_image(aWRState, toWrRect(rect), toWrRect(clip), NULL, key);
  Manager()->AddImageKeyForDiscard(key);

  Rect relBounds = TransformedVisibleBoundsRelativeToParent();
  Matrix4x4 transform;// = GetTransform();
  if (gfxPrefs::LayersDump()) printf_stderr("PaintedLayer %p using %s as bounds/overflow, %s for transform\n", this, Stringify(relBounds).c_str(), Stringify(transform).c_str());
  wr_pop_dl_builder(aWRState, toWrRect(relBounds), toWrRect(relBounds), &transform.components[0], FrameMetrics::NULL_SCROLL_ID);
}

} // namespace layers
} // namespace mozilla
