/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=4 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/WebRenderBridgeParent.h"

#include "mozilla/Unused.h"

namespace mozilla {
namespace layers {

WebRenderBridgeParent::WebRenderBridgeParent(const uint64_t& aPipelineId)
  : mPipelineId(aPipelineId)
{
  Unused << mPipelineId;
}

WebRenderBridgeParent::~WebRenderBridgeParent()
{
}

} // namespace layers
} // namespace mozilla
