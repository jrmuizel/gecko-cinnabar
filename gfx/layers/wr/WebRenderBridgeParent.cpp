/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=4 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/WebRenderBridgeParent.h"

#include "GLContext.h"
#include "GLContextProvider.h"
#include "mozilla/widget/CompositorWidget.h"

namespace mozilla {
namespace layers {

WebRenderBridgeParent::WebRenderBridgeParent(const uint64_t& aPipelineId,
                                             widget::CompositorWidget* aWidget,
                                             gl::GLContext* aGlContext)
  : mPipelineId(aPipelineId)
  , mWidget(aWidget)
  , mWRState(nullptr)
  , mGLContext(aGlContext)
{
  MOZ_ASSERT(mGLContext);
}

bool
WebRenderBridgeParent::RecvCreate(const uint32_t& aWidth,
                                  const uint32_t& aHeight)
{
  if (mWRState) {
    return true;
  }
  mGLContext->MakeCurrent();
  mWRState = wr_create(aWidth, aHeight, mPipelineId);
  return true;
}

bool
WebRenderBridgeParent::RecvDestroy()
{
  MOZ_ASSERT(mWRState);
  wr_destroy(mWRState);
  mWRState = nullptr;
  return true;
}

bool
WebRenderBridgeParent::RecvAddImage(const uint32_t& aWidth,
                                    const uint32_t& aHeight,
                                    const uint32_t& aStride,
                                    const WRImageFormat& aFormat,
                                    const ByteBuffer& aBuffer,
                                    WRImageKey* aOutImageKey)
{
  MOZ_ASSERT(mWRState);
  *aOutImageKey = wr_add_image(mWRState, aWidth, aHeight, aStride, aFormat,
                               aBuffer.mData, aBuffer.mLength);
  return true;
}

bool
WebRenderBridgeParent::RecvUpdateImage(const WRImageKey& aImageKey,
                                       const uint32_t& aWidth,
                                       const uint32_t& aHeight,
                                       const WRImageFormat& aFormat,
                                       const ByteBuffer& aBuffer)
{
  MOZ_ASSERT(mWRState);
  wr_update_image(mWRState, aImageKey, aWidth, aHeight, aFormat,
                  aBuffer.mData, aBuffer.mLength);
  return true;
}

bool
WebRenderBridgeParent::RecvDeleteImage(const WRImageKey& aImageKey)
{
  MOZ_ASSERT(mWRState);
  wr_delete_image(mWRState, aImageKey);
  return true;
}

bool
WebRenderBridgeParent::RecvPushDLBuilder()
{
  MOZ_ASSERT(mWRState);
  wr_push_dl_builder(mWRState);
  return true;
}

bool
WebRenderBridgeParent::RecvPopDLBuilder(const WRRect& aBounds,
                                        const WRRect& aOverflow,
                                        const gfx::Matrix4x4& aMatrix,
                                        const uint64_t& aScrollId)
{
  MOZ_ASSERT(mWRState);
  wr_pop_dl_builder(mWRState, aBounds, aOverflow, &aMatrix.components[0], aScrollId);
  return true;
}

bool
WebRenderBridgeParent::RecvDPBegin(const uint32_t& aWidth,
                                   const uint32_t& aHeight)
{
  MOZ_ASSERT(mWRState);
  if (mWidget) {
    mozilla::widget::WidgetRenderingContext widgetContext;
#if defined(XP_MACOSX)
    widgetContext.mGL = mGLContext;
#endif
    mWidget->PreRender(&widgetContext);
  }
  mGLContext->MakeCurrent();
  wr_dp_begin(mWRState, aWidth, aHeight);
  return true;
}

bool
WebRenderBridgeParent::RecvDPEnd()
{
  MOZ_ASSERT(mWRState);
  mGLContext->MakeCurrent();
  wr_dp_end(mWRState);
  mGLContext->SwapBuffers();
  if (mWidget) {
    mozilla::widget::WidgetRenderingContext widgetContext;
#if defined(XP_MACOSX)
    widgetContext.mGL = mGLContext;
#endif
    mWidget->PostRender(&widgetContext);
  }
  return true;
}

bool
WebRenderBridgeParent::RecvDPPushRect(const WRRect& aBounds,
                                      const WRRect& aClip,
                                      const float& r, const float& g,
                                      const float& b, const float& a)
{
  MOZ_ASSERT(mWRState);
  wr_dp_push_rect(mWRState, aBounds, aClip, r, g, b, a);
  return true;
}

bool
WebRenderBridgeParent::RecvDPPushImage(const WRRect& aBounds,
                                       const WRRect& aClip,
                                       const Maybe<WRImageMask>& aMask,
                                       const WRImageKey& aKey)
{
  MOZ_ASSERT(mWRState);
  wr_dp_push_image(mWRState, aBounds, aClip, aMask.ptrOr(nullptr), aKey);
  return true;
}

bool
WebRenderBridgeParent::RecvDPPushIframe(const WRRect& aBounds,
                                        const WRRect& aClip,
                                        const uint64_t& aLayersId)
{
  MOZ_ASSERT(mWRState);
  wr_dp_push_iframe(mWRState, aBounds, aClip, aLayersId);
  return true;
}

WebRenderBridgeParent::~WebRenderBridgeParent()
{
}

} // namespace layers
} // namespace mozilla
