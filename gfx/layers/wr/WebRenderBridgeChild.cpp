/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=4 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/WebRenderBridgeChild.h"

#include "mozilla/layers/WebRenderBridgeParent.h"

namespace mozilla {
namespace layers {

WebRenderBridgeChild::WebRenderBridgeChild(const uint64_t& aPipelineId)
  : mWRParent(new WebRenderBridgeParent(aPipelineId))
{
}

void
WebRenderBridgeChild::CallCreate(const uint32_t& aWidth,
                                 const uint32_t& aHeight)
{
  mWRParent->RecvCreate(aWidth, aHeight);
}

void
WebRenderBridgeChild::CallDestroy()
{
  mWRParent->RecvDestroy();
}

void
WebRenderBridgeChild::CallAddImage(const uint32_t& aWidth,
                                   const uint32_t& aHeight,
                                   const uint32_t& aStride,
                                   const WRImageFormat& aFormat,
                                   const ByteBuffer& aBuffer,
                                   WRImageKey* aOutImageKey)
{
  mWRParent->RecvAddImage(aWidth, aHeight, aStride, aFormat, aBuffer, aOutImageKey);
}

void
WebRenderBridgeChild::CallUpdateImage(const WRImageKey& aImageKey,
                                      const uint32_t& aWidth,
                                      const uint32_t& aHeight,
                                      const WRImageFormat& aFormat,
                                      const ByteBuffer& aBuffer)
{
  mWRParent->RecvUpdateImage(aImageKey, aWidth, aHeight, aFormat, aBuffer);
}

void
WebRenderBridgeChild::CallDeleteImage(const WRImageKey& aImageKey)
{
  mWRParent->RecvDeleteImage(aImageKey);
}

void
WebRenderBridgeChild::CallPushDLBuilder()
{
  mWRParent->RecvPushDLBuilder();
}

void
WebRenderBridgeChild::CallPopDLBuilder(const WRRect& aBounds,
                                       const WRRect& aOverflow,
                                       const gfx::Matrix4x4& aMatrix,
                                       const uint64_t& aScrollId)
{
  mWRParent->RecvPopDLBuilder(aBounds, aOverflow, aMatrix, aScrollId);
}

void
WebRenderBridgeChild::CallDPBegin(const uint32_t& aWidth,
                                  const uint32_t& aHeight)
{
  mWRParent->RecvDPBegin(aWidth, aHeight);
}

void
WebRenderBridgeChild::CallDPEnd()
{
  mWRParent->RecvDPEnd();
}

void
WebRenderBridgeChild::CallDPPushRect(const WRRect& aBounds,
                                     const WRRect& aClip,
                                     const float& r, const float& g,
                                     const float& b, const float& a)
{
  mWRParent->RecvDPPushRect(aBounds, aClip, r, g, b, a);
}

void
WebRenderBridgeChild::CallDPPushImage(const WRRect& aBounds,
                                      const WRRect& aClip,
                                      const Maybe<WRImageMask>& aMask,
                                      const WRImageKey& aKey)
{
  mWRParent->RecvDPPushImage(aBounds, aClip, aMask, aKey);
}

void
WebRenderBridgeChild::CallDPPushIframe(const WRRect& aBounds,
                                       const WRRect& aClip,
                                       const uint64_t& aLayersId)
{
  mWRParent->RecvDPPushIframe(aBounds, aClip, aLayersId);
}

} // namespace layers
} // namespace mozilla
