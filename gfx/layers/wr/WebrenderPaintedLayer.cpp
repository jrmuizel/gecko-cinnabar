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
      key = wr_add_image(aWRState, size.width, size.height, stride,
                         RGBA8, data, size.height * stride);
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

  WRImageMask* mask = nullptr;
  WRImageMask imageMask;
  Layer* maskLayer = GetMaskLayer();

  if (maskLayer) {
    RefPtr<SourceSurface> surface = WebRenderLayer::ToWebRenderLayer(maskLayer)->GetAsSourceSurface();
    if (surface) {
      Matrix transform;
      Matrix4x4 effectiveTransform = maskLayer->GetEffectiveTransform();
      DebugOnly<bool> maskIs2D = effectiveTransform.CanDraw2D(&transform);
      NS_ASSERTION(maskIs2D, "How did we end up with a 3D transform here?!");
      //transform.PostTranslate(-aDeviceOffset.x, -aDeviceOffset.y);
      {
          RefPtr<DataSourceSurface> dataSurface = surface->GetDataSurface();
          DataSourceSurface::ScopedMap map(dataSurface, DataSourceSurface::MapType::READ);
          gfx::IntSize size = surface->GetSize();
          MOZ_RELEASE_ASSERT(surface->GetFormat() == SurfaceFormat::A8, "bad format");
          MOZ_RELEASE_ASSERT(size.width == map.GetStride(), "bad stride");
          WRImageKey maskKey = wr_add_image(aWRState, size.width, size.height, map.GetStride(),
                                            A8, map.GetData(), size.height * map.GetStride());

          imageMask.image = maskKey;
          imageMask.rect = toWrRect(rect);
          imageMask.repeat = false;
          Manager()->AddImageKeyForDiscard(maskKey);
          mask = &imageMask;
      }
    }
  }

  if (gfxPrefs::LayersDump()) printf_stderr("PaintedLayer %p using rect:%s clip:%s\n", this, Stringify(rect).c_str(), Stringify(clip).c_str());
  wr_dp_push_image(aWRState, toWrRect(rect), toWrRect(clip), mask, key);
  Manager()->AddImageKeyForDiscard(key);

  Rect relBounds = TransformedVisibleBoundsRelativeToParent();
  Matrix4x4 transform;// = GetTransform();
  if (gfxPrefs::LayersDump()) printf_stderr("PaintedLayer %p using %s as bounds/overflow, %s for transform\n", this, Stringify(relBounds).c_str(), Stringify(transform).c_str());
  wr_pop_dl_builder(aWRState, toWrRect(relBounds), toWrRect(relBounds), &transform.components[0], FrameMetrics::NULL_SCROLL_ID);
}

} // namespace layers
} // namespace mozilla
