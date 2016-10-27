/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrenderImageLayer.h"

namespace mozilla {
namespace layers {

using namespace gfx;

void
WebRenderImageLayer::RenderLayer(wrstate* aWRState)
{
  //RefPtr<ImageFactory> originalIF = mContainer->GetImageFactory();
  //mContainer->SetImageFactory(BasicManager()->GetImageFactory());

  AutoLockImage autoLock(mContainer);
  Image *image = autoLock.GetImage();
  if (!image) {
    //mContainer->SetImageFactory(originalIF);
    return;
  }
  RefPtr<gfx::SourceSurface> surface = image->GetAsSourceSurface();
  if (!surface || !surface->IsValid()) {
    //mContainer->SetImageFactory(originalIF);
    return;
  }

  WRScrollFrameStackingContextGenerator scrollFrames(aWRState, this);

  RefPtr<DataSourceSurface> dataSurface = surface->GetDataSurface();
  DataSourceSurface::ScopedMap map(dataSurface, DataSourceSurface::MapType::READ);
  //XXX
  MOZ_RELEASE_ASSERT(surface->GetFormat() == SurfaceFormat::B8G8R8X8 ||
                     surface->GetFormat() == SurfaceFormat::B8G8R8A8, "bad format");

  gfx::IntSize size = surface->GetSize();


  WRImageKey key;
  key = wr_add_image(aWRState, size.width, size.height, RGBA8, map.GetData(), size.height * map.GetStride());

  Rect rect(0, 0, size.width, size.height);

  Rect clip;
  if (GetClipRect().isSome()) {
      clip = RelativeToTransformedVisible(IntRectToRect(GetClipRect().ref().ToUnknownRect()));
  } else {
      clip = rect;
  }
  if (gfxPrefs::LayersDump()) printf_stderr("ImageLayer %p using rect:%s clip:%s\n", this, Stringify(rect).c_str(), Stringify(clip).c_str());
  wr_push_dl_builder(aWRState);
  wr_dp_push_image(aWRState, toWrRect(rect), toWrRect(clip), NULL, key);
  Manager()->AddImageKeyForDiscard(key);

  Rect relBounds = TransformedVisibleBoundsRelativeToParent();
  Matrix4x4 transform;// = GetTransform();
  if (gfxPrefs::LayersDump()) printf_stderr("ImageLayer %p using %s as bounds/overflow, %s for transform\n", this, Stringify(relBounds).c_str(), Stringify(transform).c_str());
  wr_pop_dl_builder(aWRState, toWrRect(relBounds), toWrRect(relBounds), &transform.components[0], FrameMetrics::NULL_SCROLL_ID);

  //mContainer->SetImageFactory(originalIF);
}

} // namespace layers
} // namespace mozilla
