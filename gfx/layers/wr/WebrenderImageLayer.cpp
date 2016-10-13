/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebRenderImageLayer.h"

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

  RefPtr<DataSourceSurface> dataSurface = surface->GetDataSurface();
  DataSourceSurface::ScopedMap map(dataSurface, DataSourceSurface::MapType::READ);
  //XXX
  MOZ_RELEASE_ASSERT(surface->GetFormat() == SurfaceFormat::B8G8R8X8 ||
                     surface->GetFormat() == SurfaceFormat::B8G8R8A8, "bad format");

  gfx::IntSize size = surface->GetSize();


  WRImageKey key;
  key = wr_add_image(aWRState, size.width, size.height, RGBA8, map.GetData(), size.height * map.GetStride());

  auto transform = GetTransform();
  Rect rect(0, 0, size.width, size.height);

  Rect clip;
  auto combinedClip = GetCombinedClipRect();
  if (combinedClip.isSome()) {
      printf("some clip\n");
      clip = IntRectToRect(combinedClip.ref().ToUnknownRect());
  } else {
      clip = rect;
  }
  wr_push_dl_builder(aWRState);
  wr_push_dl_builder(aWRState);
  wr_dp_push_image(aWRState, toWrRect(rect), toWrRect(rect), key);
  Manager()->AddImageKeyForDiscard(key);
  printf("clip, %f %f %f %f\n", clip.x, clip.y, clip.width, clip.height);
  wr_pop_dl_builder(aWRState, 0, 0, rect.width, rect.height, &transform.components[0]);
  Matrix4x4 identity;
  wr_pop_dl_builder(aWRState, clip.x, clip.y, 0, 0, &identity.components[0]);
  //mContainer->SetImageFactory(originalIF);
}

} // namespace layers
} // namespace mozilla
