/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=4 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_WebRenderBridgeChild_h
#define mozilla_layers_WebRenderBridgeChild_h

#include "mozilla/layers/PWebRenderBridgeChild.h"

namespace mozilla {
namespace layers {

class WebRenderBridgeParent;

class WebRenderBridgeChild final : public PWebRenderBridgeChild
{
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WebRenderBridgeChild)

public:
  WebRenderBridgeChild(const uint64_t& aPipelineId);
protected:
  ~WebRenderBridgeChild() {}

public:
  void CallCreate(const uint32_t& aWidth,
                  const uint32_t& aHeight);
  void CallDestroy();
  void CallAddImage(const uint32_t& aWidth,
                    const uint32_t& aHeight,
                    const uint32_t& aStride,
                    const WRImageFormat& aFormat,
                    const ByteBuffer& aBuffer,
                    WRImageKey* aOutImageKey);
  void CallUpdateImage(const WRImageKey& aImageKey,
                       const uint32_t& aWidth,
                       const uint32_t& aHeight,
                       const WRImageFormat& aFormat,
                       const ByteBuffer& aBuffer);
  void CallDeleteImage(const WRImageKey& aImageKey);
  void CallPushDLBuilder();
  void CallPopDLBuilder(const WRRect& aBounds,
                        const WRRect& aOverflow,
                        const gfx::Matrix4x4& aMatrix,
                        const uint64_t& aScrollId);
  void CallDPBegin(const uint32_t& aWidth, const uint32_t& aHeight);
  void CallDPEnd();
  void CallDPPushRect(const WRRect& aBounds,
                      const WRRect& aClip,
                      const float& r, const float& g, const float& b, const float& a);
  void CallDPPushImage(const WRRect& aBounds,
                       const WRRect& aClip,
                       const Maybe<WRImageMask>& aMask,
                       const WRImageKey& aKey);
  void CallDPPushIframe(const WRRect& aBounds,
                        const WRRect& aClip,
                        const uint64_t& aLayersId);

private:
  RefPtr<WebRenderBridgeParent> mWRParent;
};

} // namespace layers
} // namespace mozilla

#endif // mozilla_layers_WebRenderBridgeChild_h
