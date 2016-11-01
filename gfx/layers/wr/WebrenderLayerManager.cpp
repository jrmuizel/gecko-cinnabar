/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrenderLayerManager.h"

#include "apz/src/AsyncPanZoomController.h"
#include "GLContext.h"
#include "GLContextProvider.h"
#include "LayersLogging.h"
#include "mozilla/layers/APZCTreeManager.h"
#include "mozilla/layers/AsyncCompositionManager.h"
#include "mozilla/layers/CompositorBridgeParent.h"
#include "mozilla/widget/CompositorWidget.h"
#include "mozilla/widget/PlatformWidgetTypes.h"
#include "nsThreadUtils.h"
#include "TreeTraversal.h"
#include "WebrenderCanvasLayer.h"
#include "WebrenderColorLayer.h"
#include "WebrenderContainerLayer.h"
#include "WebrenderImageLayer.h"
#include "WebrenderPaintedLayer.h"
#include "webrender.h"

namespace mozilla {

using namespace gfx;
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
WebRenderLayer::ParentStackingContextBounds(size_t aScrollMetadataIndex)
{
  // Walk up to find the parent stacking context. This will be created either
  // by the nearest scrollable metrics, or by the parent layer which must be a
  // ContainerLayer.
  Layer* layer = GetLayer();
  for (size_t i = aScrollMetadataIndex + 1; i < layer->GetScrollMetadataCount(); i++) {
    if (layer->GetFrameMetrics(i).IsScrollable()) {
      return layer->GetFrameMetrics(i).CalculateCompositedRectInCssPixels().ToUnknownRect();
    }
  }
  if (layer->GetParent()) {
    return IntRectToRect(layer->GetParent()->GetVisibleRegion().GetBounds().ToUnknownRect());
  }
  return Rect();
}

Rect
WebRenderLayer::RelativeToParent(Rect aRect)
{
  Rect parentBounds = ParentStackingContextBounds(-1);
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

WRScrollFrameStackingContextGenerator::WRScrollFrameStackingContextGenerator(
        wrstate* aWRState,
        WebRenderLayer* aLayer)
  : mWRState(aWRState)
  , mLayer(aLayer)
{
  Layer* layer = mLayer->GetLayer();
  for (size_t i = layer->GetScrollMetadataCount(); i > 0; i--) {
    const FrameMetrics& fm = layer->GetFrameMetrics(i - 1);
    if (!fm.IsScrollable()) {
      continue;
    }
    if (gfxPrefs::LayersDump()) printf_stderr("Pushing stacking context id %" PRIu64"\n", fm.GetScrollId());
    wr_push_dl_builder(mWRState);
  }
}

WRScrollFrameStackingContextGenerator::~WRScrollFrameStackingContextGenerator()
{
  Matrix4x4 identity;
  Layer* layer = mLayer->GetLayer();
  for (size_t i = 0; i < layer->GetScrollMetadataCount(); i++) {
    const FrameMetrics& fm = layer->GetFrameMetrics(i);
    if (!fm.IsScrollable()) {
      continue;
    }
    CSSRect bounds = fm.CalculateCompositedRectInCssPixels();
    CSSRect overflow = fm.GetExpandedScrollableRect();
    CSSPoint scrollPos = fm.GetScrollOffset();
    Rect parentBounds = mLayer->ParentStackingContextBounds(i);
    bounds.MoveBy(-parentBounds.x, -parentBounds.y);
    // Subtract the MT scroll position from the overflow here so that the WR
    // scroll offset (which is the APZ async scroll component) always fits in
    // the available overflow. If we didn't do this and WR did bounds checking
    // on the scroll offset, we'd fail those checks.
    overflow.MoveBy(bounds.x - scrollPos.x, bounds.y - scrollPos.y);
    if (gfxPrefs::LayersDump()) {
      printf_stderr("Popping stacking context id %" PRIu64 " with bounds=%s overflow=%s\n",
        fm.GetScrollId(), Stringify(bounds).c_str(), Stringify(overflow).c_str());
    }
    wr_pop_dl_builder(mWRState, toWrRect(bounds), toWrRect(overflow), &identity.components[0], fm.GetScrollId());
  }
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

  CompositorBridgeParent::SetWRLayerManager(aLayersId, this);
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

bool
WebRenderLayerManager::BeginTransactionWithTarget(gfxContext* aTarget)
{
  return BeginTransaction();
}

bool
WebRenderLayerManager::BeginTransaction()
{
  return true;
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
  ApplyAPZOffsets();
  mGLContext->MakeCurrent();

  printf("WR Ending\n");
  wr_dp_end(mWRState);
  mGLContext->SwapBuffers();
  mWidget->PostRender(this);

  // Since we don't do repeat transactions right now, just set the time
  mAnimationReadyTime = TimeStamp::Now();
}

void
WebRenderLayerManager::ApplyAPZOffsets()
{
  // This will probably set the same scroll offset multiple times because
  // multiple layers will have the same scrollIds. TODO: We should just keep
  // a list with unique ScrollMetadatas somewhere.
  ForEachNode<ForwardIterator>(mRoot.get(), [this](Layer* aLayer) {
    for (size_t i = 0; i < aLayer->GetScrollMetadataCount(); i++) {
      AsyncPanZoomController* apzc = aLayer->GetAsyncPanZoomController(i);
      if (apzc) {
        ParentLayerPoint offset = apzc->GetCurrentAsyncTransform(AsyncPanZoomController::RESPECT_FORCE_DISABLE).mTranslation;
        wr_set_async_scroll(mWRState, apzc->GetGuid().mScrollId, offset.x, offset.y);
        if (gfxPrefs::LayersDump()) {
          printf("Setting async scroll %s for guid %s\n",
            Stringify(offset).c_str(), Stringify(apzc->GetGuid()).c_str());
        }
      }
    }
  });
}

void
WebRenderLayerManager::Composite()
{
  if (!mWRState) {
    return;
  }
  printf("WR Compositing\n");

  ApplyAPZOffsets();

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
