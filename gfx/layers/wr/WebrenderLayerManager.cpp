/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrenderLayerManager.h"

#include "GLContext.h"
#include "GLContextProvider.h"
#include "mozilla/widget/CompositorWidget.h"
#include "mozilla/widget/PlatformWidgetTypes.h"
#include "WebrenderCanvasLayer.h"
#include "WebrenderColorLayer.h"
#include "WebrenderContainerLayer.h"
#include "WebrenderImageLayer.h"
#include "WebrenderPaintedLayer.h"
#include "webrender.h"

namespace mozilla {

using namespace gl;
using namespace widget;

namespace layers {

WebRenderLayerManager::WebRenderLayerManager(nsIWidget* aWidget)
  : mWRState(nullptr)
{
  CompositorWidgetInitData initData;
  aWidget->GetCompositorWidgetInitData(&initData);
  mWidget = CompositorWidget::CreateLocal(initData, aWidget);
  mGLContext = GLContextProvider::CreateForWindow(aWidget, true);
}

void
WebRenderLayerManager::Destroy()
{

}

WebRenderLayerManager::~WebRenderLayerManager()
{

}

widget::CompositorWidgetDelegate*
WebRenderLayerManager::GetCompositorWidgetDelegate()
{
  return mWidget->AsDelegate();
}

int32_t
WebRenderLayerManager::GetMaxTextureSize() const
{
  return 4096;
}

void
WebRenderLayerManager::BeginTransactionWithTarget(gfxContext* aTarget)
{
  BeginTransaction();
}

void
WebRenderLayerManager::BeginTransaction()
{

}

bool
WebRenderLayerManager::EndEmptyTransaction(EndTransactionFlags aFlags)
{
  return false;
}

void
WebRenderLayerManager::EndTransaction(DrawPaintedLayerCallback aCallback,
                                      void* aCallbackData,
                                      EndTransactionFlags aFlags)
{

  mPaintedLayerCallback = aCallback;
  mPaintedLayerCallbackData = aCallbackData;

  LayoutDeviceIntSize size = mWidget->GetClientSize();
  if (!mWRState) {
    mGLContext->MakeCurrent();
    mWRState = wr_create(size.width, size.height, mCounter);
  }

  if (gfxPrefs::LayersDump()) {
    this->Dump();
  }

  mWidget->PreRender(this);
  mGLContext->MakeCurrent();
  printf("WR Beginning size %i %i\n", size.width, size.height);
  wr_dp_begin(mWRState, size.width, size.height);

  WebRenderLayer::ToWebRenderLayer(mRoot)->RenderLayer(mWRState);
  mGLContext->MakeCurrent();

  printf("WR Ending\n");
  wr_dp_end(mWRState);
  mGLContext->SwapBuffers();
  mWidget->PostRender(this);

  for (auto key : mImageKeys) {
      wr_delete_image(mWRState, key);
  }
  mImageKeys.clear();

  // Since we don't do repeat transactions right now, just set the time
  mAnimationReadyTime = TimeStamp::Now();
}

void
WebRenderLayerManager::AddImageKeyForDiscard(WRImageKey key)
{
  mImageKeys.push_back(key);
}

void
WebRenderLayerManager::SetRoot(Layer* aLayer)
{
  mRoot = aLayer;
}

already_AddRefed<PaintedLayer>
WebRenderLayerManager::CreatePaintedLayer()
{
  return MakeAndAddRef<WebRenderPaintedLayer>(this);
}

already_AddRefed<ContainerLayer>
WebRenderLayerManager::CreateContainerLayer()
{
  return MakeAndAddRef<WebRenderContainerLayer>(this);
}

already_AddRefed<ImageLayer>
WebRenderLayerManager::CreateImageLayer()
{
  return MakeAndAddRef<WebRenderImageLayer>(this);
}

already_AddRefed<CanvasLayer>
WebRenderLayerManager::CreateCanvasLayer()
{
  return MakeAndAddRef<WebRenderCanvasLayer>(this);
}

already_AddRefed<ReadbackLayer>
WebRenderLayerManager::CreateReadbackLayer()
{
  return nullptr;
}

already_AddRefed<ColorLayer>
WebRenderLayerManager::CreateColorLayer()
{
  return MakeAndAddRef<WebRenderColorLayer>(this);
}

already_AddRefed<RefLayer>
WebRenderLayerManager::CreateRefLayer()
{
  return MakeAndAddRef<WebRenderRefLayer>(this);
}

} // namespace layers
} // namespace mozilla
