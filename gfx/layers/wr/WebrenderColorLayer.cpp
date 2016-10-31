/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrenderColorLayer.h"

#include "LayersLogging.h"
#include "webrender.h"

namespace mozilla {
namespace layers {

 using namespace mozilla::gfx;

void
WebRenderColorLayer::RenderLayer(wrstate* aWRState)
{
  WRScrollFrameStackingContextGenerator scrollFrames(aWRState, this);

  gfx::Rect rect = RelativeToParent(GetTransform().TransformBounds(IntRectToRect(mBounds)));
  gfx::Rect clip;
  if (GetClipRect().isSome()) {
      clip = RelativeToParent(IntRectToRect(GetClipRect().ref().ToUnknownRect()));
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
  //        Manager()->AddImageKeyForDiscard(maskKey);
          mask = &imageMask;
      }
    }
  }


  if (gfxPrefs::LayersDump()) printf_stderr("ColorLayer %p using rect:%s clip:%s\n", this, Stringify(rect).c_str(), Stringify(clip).c_str());
  wr_dp_push_rect(aWRState, toWrRect(rect), toWrRect(clip), mask,
                  mColor.r, mColor.g, mColor.b, mColor.a);
}

} // namespace layers
} // namespace mozilla
