/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebRenderLayerManager.h"

#include "GLContext.h"
#include "GLContextProvider.h"
#include "mozilla/widget/CompositorWidget.h"
#include "mozilla/widget/PlatformWidgetTypes.h"
#include "WebRenderCanvasLayer.h"
#include "WebRenderColorLayer.h"
#include "WebRenderContainerLayer.h"
#include "WebRenderImageLayer.h"
#include "WebRenderPaintedLayer.h"
#include "webrender.h"

namespace mozilla {

using namespace gl;
using namespace widget;

namespace layers {

WebRenderLayerManager::WebRenderLayerManager(nsIWidget* aWidget)
  : mWRState(nullptr)
{
  static uint32_t counter = 0;
  mCounter = counter++;
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
  printf("WebRenderLayerManager::EndTransaction with mCounter %d\n", int(mCounter));
  if (mCounter != 0) {
    return;
  }

  LayoutDeviceIntSize size = mWidget->GetClientSize();
  if (!mWRState) {
    mGLContext->MakeCurrent();
    mWRState = wr_create(size.width, size.height, mCounter);
  }
 
  mWidget->PreRender(this);
  mGLContext->MakeCurrent();
  static int frame = 0;
  printf("WR Beginning\n");
  wr_dp_begin(mWRState, size.width, size.height);

  WebRenderLayer::ToWebRenderLayer(mRoot)->RenderLayer(mWRState);

  wr_dp_push_rect(mWRState, frame % 100, frame % 100, 100, 100, 1.f, 0.f, 0.f, 1.f);
  frame += 2;

  printf("WR Ending\n");
  wr_dp_end(mWRState);
  mGLContext->SwapBuffers();
  mWidget->PostRender(this);
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
