/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrenderPaintedLayer.h"

#include "mozilla/ArrayUtils.h"
#include "gfxUtils.h"

namespace mozilla {
namespace layers {

using namespace mozilla::gfx;

void
WebRenderPaintedLayer::RenderLayer(wrstate* aWRState)
{
  auto visibleRegion = GetVisibleRegion();
  auto bounds = visibleRegion.GetBounds();
  auto size = bounds.Size();
  if (size.IsEmpty()) {
      printf("Empty region\n");
      return;
  }

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
      key = wr_add_image(aWRState, size.width, size.height, RGBA8, data, size.height * stride);

      target->ReleaseBits(data);
  }
  auto transform = GetTransform();
  Rect rect(bounds.x, bounds.y, bounds.width, bounds.height);
  Rect clip;
  auto combinedClip = GetCombinedClipRect();
  if (combinedClip.isSome()) {
      clip = IntRectToRect(combinedClip.ref().ToUnknownRect());
  } else {
      clip = rect;
  }
  wr_dp_push_image(aWRState, toWrRect(rect), toWrRect(clip), NULL, key);
  Manager()->AddImageKeyForDiscard(key);
  wr_pop_dl_builder(aWRState, bounds.x, bounds.y, bounds.width + bounds.x, bounds.height + bounds.y, &transform.components[0]);
}

} // namespace layers
} // namespace mozilla
