/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_WEBRENDERLAYERMANAGER_H
#define GFX_WEBRENDERLAYERMANAGER_H

#include "Layers.h"
#include "mozilla/layers/CompositorController.h"
#include "webrender.h"

struct wrstate;
class nsIWidget;

namespace mozilla {
namespace gl {
class GLContext;
}
namespace widget {
class CompositorWidget;
class CompositorWidgetDelegate;
}
namespace layers {

static inline WRRect toWrRect(gfx::Rect rect)
{
    WRRect r;
    r.x = rect.x;
    r.y = rect.y;
    r.width = rect.width;
    r.height = rect.height;
    return r;
}


class WebRenderLayerManager;

class WebRenderLayer
{
public:
  virtual void RenderLayer(wrstate* aWRState) = 0;

  static inline WebRenderLayer*
  ToWebRenderLayer(Layer* aLayer)
  {
    return static_cast<WebRenderLayer*>(aLayer->ImplData());
  }
};

class WebRenderLayerManager final : public LayerManager, public CompositorController
{
public:
  explicit WebRenderLayerManager(nsIWidget* aWidget,
                                 uint64_t aLayersId);

  virtual void Destroy() override;

protected:
  virtual ~WebRenderLayerManager();

public:

  gl::GLContext* gl() const { return mGLContext; }

  virtual int32_t GetMaxTextureSize() const override;

  virtual void BeginTransactionWithTarget(gfxContext* aTarget) override;
  virtual void BeginTransaction() override;
  virtual bool EndEmptyTransaction(EndTransactionFlags aFlags = END_DEFAULT) override;
  virtual void EndTransaction(DrawPaintedLayerCallback aCallback,
                              void* aCallbackData,
                              EndTransactionFlags aFlags = END_DEFAULT) override;
  virtual void Composite() override;

  virtual LayersBackend GetBackendType() override { return LayersBackend::LAYERS_WR; }
  virtual void GetBackendName(nsAString& name) override { name.AssignLiteral("WebRender"); }
  virtual const char* Name() const override { return "WebRender"; }

  virtual void SetRoot(Layer* aLayer) override;

  virtual already_AddRefed<PaintedLayer> CreatePaintedLayer() override;
  virtual already_AddRefed<ContainerLayer> CreateContainerLayer() override;
  virtual already_AddRefed<ImageLayer> CreateImageLayer() override;
  virtual already_AddRefed<CanvasLayer> CreateCanvasLayer() override;
  virtual already_AddRefed<ReadbackLayer> CreateReadbackLayer() override;
  virtual already_AddRefed<ColorLayer> CreateColorLayer() override;
  virtual already_AddRefed<RefLayer> CreateRefLayer() override;

  virtual bool NeedsWidgetInvalidation() override { return true; }

  widget::CompositorWidgetDelegate* GetCompositorWidgetDelegate();

  DrawPaintedLayerCallback GetPaintedLayerCallback() const
  { return mPaintedLayerCallback; }

  void* GetPaintedLayerCallbackData() const
  { return mPaintedLayerCallbackData; }

  // adds an imagekey to a list of keys that will be discarded on the next
  // transaction or destruction
  void AddImageKeyForDiscard(WRImageKey);
  void DiscardImages();

  // CompositorController
  NS_IMETHOD_(MozExternalRefCountType) AddRef() override { return LayerManager::AddRef(); }
  NS_IMETHOD_(MozExternalRefCountType) Release() override { return LayerManager::Release(); }
  void ScheduleRenderOnCompositorThread() override;
  void ScheduleHideAllPluginWindows() override {}
  void ScheduleShowAllPluginWindows() override {}

private:
  RefPtr<widget::CompositorWidget> mWidget;
  RefPtr<gl::GLContext> mGLContext;
  wrstate* mWRState;
  uint32_t mCounter;
  std::vector<WRImageKey> mImageKeys;

  /* PaintedLayer callbacks; valid at the end of a transaciton,
   * while rendering */
  DrawPaintedLayerCallback mPaintedLayerCallback;
  void *mPaintedLayerCallbackData;

  // APZ stuff
  uint64_t mLayersId;
};

} // namespace layers
} // namespace mozilla

#endif /* GFX_WEBRENDERLAYERMANAGER_H */
