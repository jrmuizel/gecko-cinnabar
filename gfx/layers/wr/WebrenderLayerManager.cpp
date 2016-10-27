/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrenderLayerManager.h"

#include "GLContext.h"
#include "GLContextProvider.h"
#include "mozilla/layers/APZCTreeManager.h"
#include "mozilla/layers/CompositorBridgeParent.h"
#include "mozilla/widget/CompositorWidget.h"
#include "mozilla/widget/PlatformWidgetTypes.h"
#include "nsThreadUtils.h"
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

Rect
WebRenderLayer::RelativeToVisible(Rect aRect)
{
  IntRect bounds = GetLayer()->GetVisibleRegion().GetBounds().ToUnknownRect();
  aRect.MoveBy(-bounds.x, -bounds.y);
  return aRect;
}

Rect
WebRenderLayer::RelativeToTransformedVisible(Rect aRect)
{
  IntRect bounds = GetLayer()->GetVisibleRegion().GetBounds().ToUnknownRect();
  Rect transformed = GetLayer()->GetTransform().TransformBounds(IntRectToRect(bounds));
  aRect.MoveBy(-transformed.x, -transformed.y);
  return aRect;
}

Rect
WebRenderLayer::RelativeToParent(Rect aRect)
{
  IntRect parentBounds;
  if (GetLayer()->GetParent()) {
    parentBounds = GetLayer()->GetParent()->GetVisibleRegion().GetBounds().ToUnknownRect();
  }
  aRect.MoveBy(-parentBounds.x, -parentBounds.y);
  return aRect;
}

Rect
WebRenderLayer::TransformedVisibleBoundsRelativeToParent()
{
  IntRect bounds = GetLayer()->GetVisibleRegion().GetBounds().ToUnknownRect();
  Rect transformed = GetLayer()->GetTransform().TransformBounds(IntRectToRect(bounds));
  return RelativeToParent(transformed);
}


WebRenderLayerManager::WebRenderLayerManager(nsIWidget* aWidget,
                                             uint64_t aLayersId,
                                             APZCTreeManager* aAPZC)
  : mWRState(nullptr)
  , mLayersId(aLayersId)
  , mAPZC(aAPZC)
  , mIsFirstPaint(false)
{
  CompositorWidgetInitData initData;
  aWidget->GetCompositorWidgetInitData(&initData);
  mWidget = CompositorWidget::CreateLocal(initData, aWidget);
  mGLContext = GLContextProvider::CreateForWindow(aWidget, true);

  // ewwwwww, using extern to access a faraway object because I don't want to
  // expose it properly via an API! Good thing this is just a hacky prototype
  // and I can get away with it :)
  extern std::map<uint64_t, CompositorBridgeParent::LayerTreeState> sIndirectLayerTrees;
  sIndirectLayerTrees[mLayersId].mWRManager = this;
}

void
WebRenderLayerManager::Destroy()
{
  if (mAPZC) {
    mAPZC->ClearTree();
    mAPZC = nullptr;
  }
}

WebRenderLayerManager::~WebRenderLayerManager()
{
  DiscardImages();
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
  if (mAPZC) {
    mAPZC->UpdateHitTestingTree(mLayersId, GetRoot(), mIsFirstPaint, mLayersId, 0);
  }

  DiscardImages();

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

  // Since we don't do repeat transactions right now, just set the time
  mAnimationReadyTime = TimeStamp::Now();
}

void
WebRenderLayerManager::Composite()
{
  if (!mWRState) {
    return;
  }
  printf("WR Compositing\n");
  mGLContext->MakeCurrent();
  wr_composite(mWRState);
  mGLContext->SwapBuffers();
}

void
WebRenderLayerManager::ScheduleRenderOnCompositorThread()
{
  NS_DispatchToMainThread(NewRunnableMethod(this, &WebRenderLayerManager::Composite));
}

void
WebRenderLayerManager::AddImageKeyForDiscard(WRImageKey key)
{
  mImageKeys.push_back(key);
}

void
WebRenderLayerManager::DiscardImages()
{
  for (auto key : mImageKeys) {
      wr_delete_image(mWRState, key);
  }
  mImageKeys.clear();
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
