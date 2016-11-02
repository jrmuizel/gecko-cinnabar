/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=4 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_WebRenderBridgeParent_h
#define mozilla_layers_WebRenderBridgeParent_h

#include "mozilla/layers/PWebRenderBridgeParent.h"

namespace mozilla {
namespace layers {

class WebRenderBridgeParent final : public PWebRenderBridgeParent
{
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WebRenderBridgeParent)

public:
  WebRenderBridgeParent(const uint64_t& aPipelineId);
  uint64_t PipelineId() { return mPipelineId; }

  void ActorDestroy(ActorDestroyReason aWhy) override {}

protected:
  virtual ~WebRenderBridgeParent();

private:
  uint64_t mPipelineId;
};

} // namespace layers
} // namespace mozilla

#endif // mozilla_layers_WebRenderBridgeParent_h
