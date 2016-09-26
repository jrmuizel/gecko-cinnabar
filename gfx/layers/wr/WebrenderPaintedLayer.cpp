/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebRenderPaintedLayer.h"

#include "mozilla/ArrayUtils.h"
#include "gfxUtils.h"

namespace mozilla {
namespace layers {

using namespace mozilla::gfx;

static Color
PointerToColor(void* aPtr)
{
  Color colors[] = {
    Color(0 / 255.0f, 136 / 255.0f, 204 / 255.0f, 1),
    Color(91 / 255.0f, 95 / 255.0f, 255 / 255.0f, 1),
    Color(184 / 255.0f, 46 / 255.0f, 229 / 255.0f, 1),
    Color(237 / 255.0f, 38 / 255.0f, 85 / 255.0f, 1),
    Color(241 / 255.0f, 60 / 255.0f, 0 / 255.0f, 1),
    Color(217 / 255.0f, 126 / 255.0f, 0 / 255.0f, 1),
    Color(44 / 255.0f, 187 / 255.0f, 15 / 255.0f, 1),
    Color(0 / 255.0f, 114 / 255.0f, 171 / 255.0f, 1)
  };
  uintptr_t number = reinterpret_cast<uintptr_t>(aPtr);
  srand(number);
  size_t index = rand() % MOZ_ARRAY_LENGTH(colors);
  return colors[index];
}

void
WebRenderPaintedLayer::RenderLayer(wrstate* aWRState)
{
  auto visibleRegion = GetVisibleRegion();
  auto size = visibleRegion.GetBounds().Size();
  if (size.IsEmpty()) {
      printf("Empty region\n");
      return;
  } else {
      printf("have size: %d %d\n", size.width, size.height);
  }
  RefPtr<DrawTarget> target = gfx::Factory::CreateDrawTarget(gfx::BackendType::CAIRO, size.ToUnknownSize(), SurfaceFormat::B8G8R8A8);

  RefPtr<gfxContext> ctx = gfxContext::CreatePreservingTransformOrNull(target);
  MOZ_ASSERT(ctx); // already checked the target above

  Manager()->GetPaintedLayerCallback()(this,
                                       ctx,
                                       visibleRegion.ToUnknownRegion(), visibleRegion.ToUnknownRegion(),
                                       DrawRegionClip::DRAW, nsIntRegion(), Manager()->GetPaintedLayerCallbackData());
  Rect transformedBounds = GetEffectiveTransform().TransformBounds(Rect(GetVisibleRegion().GetBounds().ToUnknownRect()));
  static int count;
  char buf[400];
  sprintf(buf, "wrout%d.png", count++);
  //gfxUtils::WriteAsPNG(target, buf);
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
  wr_dp_push_image(aWRState, transformedBounds.x, transformedBounds.y, transformedBounds.width, transformedBounds.height,
                  key);
}

} // namespace layers
} // namespace mozilla
