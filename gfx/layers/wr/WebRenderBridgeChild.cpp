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
  : mIsInTransaction(false)
{
}

void
WebRenderBridgeChild::PushDLBuilder()
{
  MOZ_ASSERT(mIsInTransaction);
  mCommands.AppendElement(OpPushDLBuilder());
}

void
WebRenderBridgeChild::PopDLBuilder(const WRRect& aBounds,
                                   const WRRect& aOverflow,
                                   const Matrix4x4& aMatrix,
                                   uint64_t aScrollId)
{
  MOZ_ASSERT(mIsInTransaction);
  mCommands.AppendElement(OpPopDLBuilder(aBounds, aOverflow, aMatrix, aScrollId));
}

void
WebRenderBridgeChild::DPPushRect(const WRRect& aBounds,
                                 const WRRect& aClip,
                                 float r,
                                 float g,
                                 float b,
                                 float a)
{
  MOZ_ASSERT(mIsInTransaction);
  mCommands.AppendElement(OpDPPushRect(aBounds, aClip, r, g, b, a));
}

void
WebRenderBridgeChild::DPPushImage(const WRRect& aBounds,
                                  const WRRect& aClip,
                                  const MaybeImageMask& aMask,
                                  const WRImageKey& aKey)
{
  MOZ_ASSERT(mIsInTransaction);
  mCommands.AppendElement(OpDPPushImage(aBounds, aClip, aMask, aKey));
}

void
WebRenderBridgeChild::DPPushIframe(const WRRect& aBounds,
                                   const WRRect& aClip,
                                   uint64_t aLayersId)
{
  MOZ_ASSERT(mIsInTransaction);
  mCommands.AppendElement(OpDPPushIframe(aBounds, aClip, aLayersId));
}

bool
WebRenderBridgeChild::DPBegin(uint32_t aWidth, uint32_t aHeight)
{
  MOZ_ASSERT(!mIsInTransaction);
  bool success = false;
  this->SendDPBegin(aWidth, aHeight, &success);
  if (!success) {
    return false;
  }

  mIsInTransaction = true;
  return true;
}


void
WebRenderBridgeChild::DPEnd()
{
  MOZ_ASSERT(mIsInTransaction);
  this->SendDPEnd(mCommands);
  mCommands.Clear();
  mIsInTransaction = false;
}

} // namespace layers
} // namespace mozilla
