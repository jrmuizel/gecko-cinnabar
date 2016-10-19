/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GLManager.h"
#include "CompositorOGL.h"              // for CompositorOGL
#include "GLContext.h"                  // for GLContext
#include "mozilla/Attributes.h"         // for override
#include "mozilla/RefPtr.h"             // for RefPtr
#include "mozilla/layers/Compositor.h"  // for Compositor
#include "mozilla/layers/LayerManagerComposite.h"
#include "mozilla/layers/WebrenderLayerManager.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/mozalloc.h"           // for operator new, etc

using namespace mozilla::gl;

namespace mozilla {
namespace layers {

class GLManagerCompositor : public GLManager
{
public:
  explicit GLManagerCompositor(CompositorOGL* aCompositor)
    : mImpl(aCompositor)
  {}

  virtual GLContext* gl() const override
  {
    return mImpl->gl();
  }

  virtual void ActivateProgram(ShaderProgramOGL *aProg) override
  {
    mImpl->ActivateProgram(aProg);
  }

  virtual ShaderProgramOGL* GetProgram(GLenum aTarget, gfx::SurfaceFormat aFormat) override
  {
    ShaderConfigOGL config = ShaderConfigFromTargetAndFormat(aTarget, aFormat);
    return mImpl->GetShaderProgramFor(config);
  }

  virtual const gfx::Matrix4x4& GetProjMatrix() const override
  {
    return mImpl->GetProjMatrix();
  }

  virtual void BindAndDrawQuad(ShaderProgramOGL *aProg,
                               const gfx::Rect& aLayerRect,
                               const gfx::Rect& aTextureRect) override
  {
    mImpl->BindAndDrawQuad(aProg, aLayerRect, aTextureRect);
  }

private:
  RefPtr<CompositorOGL> mImpl;
};

class GLManagerGLContext : public GLManager
{
public:
  explicit GLManagerGLContext(GLContext* aGLContext)
    : mGLContext(aGLContext)
  {}

  virtual GLContext* gl() const override { return mGLContext; }

  virtual void ActivateProgram(ShaderProgramOGL *aProg) override {}
  virtual ShaderProgramOGL* GetProgram(GLenum aTarget, gfx::SurfaceFormat aFormat) override { return nullptr; }
  virtual const gfx::Matrix4x4& GetProjMatrix() const override { return mMatrix; }
  virtual void BindAndDrawQuad(ShaderProgramOGL *aProg,
                               const gfx::Rect& aLayerRect,
                               const gfx::Rect& aTextureRect) override {}

private:
  RefPtr<GLContext> mGLContext;
  gfx::Matrix4x4 mMatrix;
};

/* static */ GLManager*
GLManager::CreateGLManager(LayerManagerComposite* aManager)
{
  if (aManager && aManager->GetCompositor()->GetBackendType() == LayersBackend::LAYERS_OPENGL) {
    return new GLManagerCompositor(aManager->GetCompositor()->AsCompositorOGL());
  }
  return nullptr;
}

/* static */ GLManager*
GLManager::CreateGLManager(WebRenderLayerManager* aManager)
{
  if (aManager) {
    return new GLManagerGLContext(aManager->gl());
  }
  return nullptr;
}

/* static */ GLManager*
GLManager::CreateGLManager(LayerManager* aManager)
{
  if (aManager) {
    if (aManager->AsLayerManagerComposite()) {
      return CreateGLManager(aManager->AsLayerManagerComposite());
    }
    if (aManager->GetBackendType() == LayersBackend::LAYERS_WR) {
      return CreateGLManager(static_cast<WebRenderLayerManager*>(aManager));
    }
  }
  return nullptr;
}


} // namespace layers
} // namespace mozilla
