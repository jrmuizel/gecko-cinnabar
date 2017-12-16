/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebRenderCommandBuilder.h"

#include "BasicLayers.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Types.h"
#include "mozilla/gfx/DrawEventRecorder.h"
#include "mozilla/layers/ImageClient.h"
#include "mozilla/layers/WebRenderBridgeChild.h"
#include "mozilla/layers/WebRenderLayerManager.h"
#include "mozilla/layers/IpcResourceUpdateQueue.h"
#include "mozilla/layers/ScrollingLayersHelper.h"
#include "mozilla/layers/StackingContextHelper.h"
#include "mozilla/layers/UpdateImageHelper.h"
#include "gfxEnv.h"
#include "nsDisplayListInvalidation.h"
#include "WebRenderCanvasRenderer.h"
#include "LayersLogging.h"
#include "LayerTreeInvalidation.h"

namespace mozilla {
namespace layers {

using namespace gfx;


//XXX: problems:
// - How do we deal with scrolling while having only a single invalidation rect?
// We can have a valid rect and an invalid rect. As we scroll the valid rect will move
// and the invalid rect will be the new area

// who should own the data? A: the HashTable?
//mDestructorWithFrame() can clear out the weak pointer
struct BlobItemData;
void RemoveFrameFromBlobGroup(nsTArray<BlobItemData*>* aArray);
NS_DECLARE_FRAME_PROPERTY_WITH_DTOR(BlobGroupDataProperty,
                                    nsTArray<BlobItemData*>,
                                    RemoveFrameFromBlobGroup);
// XXX: check that all members are initialized
struct BlobItemData {
  nsIFrame *mFrame;
  IntRect mRect;
  // It would be nice to not need this. We need to be able to call ComputeInvalidationRegion.
  // ComputeInvalidationRegion will sometimes reach into parent style structs to get information
  // that can change the invalidation region
  UniquePtr<nsDisplayItemGeometry> mGeometry;
  DisplayItemClip mClip; // this can change
  uint32_t        mDisplayItemKey;
  bool mUsed;
  /**
    * Temporary storage of the display item being referenced, only valid between
    * BeginUpdate and EndUpdate.
    */
  //nsDisplayItem *mItem;
  BlobItemData(nsDisplayItem *aItem) {
    mDisplayItemKey = aItem->GetPerFrameKey();
    AddFrame(aItem->Frame());
  }

  //XXX: matwoodrow why is there a mFrameList instead of just a single Frame?
  // mFrameList is to deal with merged frames
  void
  AddFrame(nsIFrame* aFrame)
  {
    mFrame = aFrame;
    //mFrameList.AppendElement(aFrame);

    nsTArray<BlobItemData*>* array =
      aFrame->GetProperty(BlobGroupDataProperty());
    if (!array) {
      array = new nsTArray<BlobItemData*>();
      aFrame->SetProperty(BlobGroupDataProperty(), array);
    }
    array->AppendElement(this);
  }

  ~BlobItemData() {
    if (mFrame) {
      // Delete weak pointer on frame
      nsTArray<BlobItemData*> *array =
        reinterpret_cast<nsTArray<BlobItemData*>*>(mFrame->GetProperty(BlobGroupDataProperty()));
      array->RemoveElement(this);

      // drop the entire property if nothing's left in the array
      if (array->IsEmpty()) {
        mFrame->DeleteProperty(BlobGroupDataProperty());
      }
    }
  }
};

BlobItemData*
GetBlobItemData(nsIFrame* aFrame, uint32_t aKey)
{
  const nsTArray<BlobItemData*>* array =
    aFrame->GetProperty(BlobGroupDataProperty());
  if (array) {
    for (BlobItemData *item : *array) {
      if (item->mDisplayItemKey == aKey
          // XXX: do we need something like this? && item->mLayer->Manager() == mRetainingManager
          ) {
        return item;
      }
    }
  }
  return nullptr;
}
// We keep around the BlobItemData so that when we invalidate it get properly included in the rect
void RemoveFrameFromBlobGroup(nsTArray<BlobItemData*>* aArray) {
  for (BlobItemData* item : *aArray) {
    item->mFrame = nullptr;
  }
  delete aArray;
}

struct DIGroup;
struct Grouper {
  Grouper(ScrollingLayersHelper& aScrollingHelper)
   : mScrollingHelper(aScrollingHelper)
  {}

  int32_t mAppUnitsPerDevPixel;
  std::vector<nsDisplayItem*> mItemStack;
  nsDisplayListBuilder* mDisplayListBuilder;
  ScrollingLayersHelper& mScrollingHelper;
  Matrix mTransform;
  void PushParent(DIGroup* aGroup, nsDisplayItem* aItem, gfxContext* ctx, DrawEventRecorderMemory* aRecorder);
  void PopParent(DIGroup* aGroup, nsDisplayItem* aItem, gfxContext* ctx, DrawEventRecorderMemory* aRecorder);
  void ConstructGroups(WebRenderCommandBuilder* aCommandBuilder,
                       wr::DisplayListBuilder& aBuilder,
                       wr::IpcResourceUpdateQueue& aResources,
                       DIGroup* aGroup, nsDisplayList* aList,
                       const StackingContextHelper& aSc);
  void ConstructGroupsInsideInactive(WebRenderCommandBuilder* aCommandBuilder,
                                       wr::DisplayListBuilder& aBuilder,
                                       wr::IpcResourceUpdateQueue& aResources,
                                       DIGroup* aGroup, nsDisplayList* aList,
                                       const StackingContextHelper& aSc);
};

// layers free 

struct DIGroup {
  // should we just store these things in the hashtable?
  // XXX: instead of a hash table we should just use a Vec
  // we'll iterate, remove old items and compact in one pass.
  // To avoid the copying we can have a vec that's the backing store.
  // but a separate pointer array that we use to iterate. It may
  // make sense to move some of the less frequently accessed data out of line
  // so we can avoid copying it around...
  //
  // We should just be using a linked list for this stuff.
  // That we can iterate over only the used items.
  // We remove from the unused list and add to the used list
  // when we see an item.
  //
  // we allocate using a free list.
  nsTHashtable<nsPtrHashKey<BlobItemData>> mDisplayItems;

  nsPoint mAnimatedGeometryRootOrigin;
  nsPoint mLastAnimatedGeometryRootOrigin;
  IntRect mInvalidRect;
  nsRect mGroupBounds;
  IntPoint mGroupOffset;
  Maybe<wr::ImageKey> mKey;

  void InvalidateRect(IntRect aRect) {
    mInvalidRect = mInvalidRect.Union(aRect);
  }

  IntRect ItemBounds(nsDisplayItem *item)
  {
    BlobItemData* data = GetBlobItemData(item->Frame(), item->GetPerFrameKey());
    return data->mRect;
  }

  void ComputeGeometryChange(nsDisplayItem *item, BlobItemData *aData, Matrix &mMatrix, nsDisplayListBuilder *builder) {
    // If the frame is marked as invalidated, and didn't specify a rect to invalidate then we want to
    // invalidate both the old and new bounds, otherwise we only want to invalidate the changed areas.
    // If we do get an invalid rect, then we want to add this on top of the change areas.
    nsRect invalid;
    nsRegion combined;
    bool notifyRenderingChanged = true;
    const DisplayItemClip& clip = item->GetClip();

    nsPoint shift = mAnimatedGeometryRootOrigin - mLastAnimatedGeometryRootOrigin;

    int32_t appUnitsPerDevPixel = item->Frame()->PresContext()->AppUnitsPerDevPixel();
    // XXX: we need to compute this properly. This basically just matches the
    // computation for regular fallback. We should be more disciplined.
    LayoutDeviceRect bounds = LayoutDeviceRect::FromAppUnits(mGroupBounds, appUnitsPerDevPixel);
    LayoutDeviceIntPoint offset = RoundedToInt(bounds.TopLeft());
    auto layoutImageRect = LayoutDeviceIntRect(offset, LayoutDeviceIntSize(RoundedToInt(bounds.Size())));
    MOZ_RELEASE_ASSERT(mGroupOffset.x == layoutImageRect.x && mGroupOffset.y == layoutImageRect.y);
    IntRect imageRect(0, 0, layoutImageRect.width, layoutImageRect.height);

    /*if (item->IsReused() && aData->mGeometry) {
      return;
    }*/

    printf("pre mInvalidRect: %d %d %d %d\n", mInvalidRect.x, mInvalidRect.y, mInvalidRect.width, mInvalidRect.height);
    if (true || !aData->mGeometry) {
      // This item is being added for the first time, invalidate its entire area.
      UniquePtr<nsDisplayItemGeometry> geometry(item->AllocateGeometry(builder));
      combined = clip.ApplyNonRoundedIntersection(geometry->ComputeInvalidationRegion());
      aData->mGeometry = Move(geometry);
      nsRect bounds = combined.GetBounds();

      auto transBounds = nsLayoutUtils::MatrixTransformRect(bounds,
                                                            Matrix4x4::From2D(mMatrix),
                                                            float(appUnitsPerDevPixel));

      IntRect transformedRect = RoundedOut(mMatrix.TransformBounds(ToRect(nsLayoutUtils::RectToGfxRect(combined.GetBounds(), appUnitsPerDevPixel)))) - mGroupOffset;
      aData->mRect = transformedRect.Intersect(imageRect);
      printf("CGC %s %d %d %d %d\n", item->Name(), bounds.x, bounds.y, bounds.width, bounds.height);
      printf("transBounds %d %d %d %d\n", transBounds.x, transBounds.y, transBounds.width, transBounds.height);
      printf("%d %d,  %f %f\n", mGroupOffset.x, mGroupOffset.y, mMatrix._11, mMatrix._22);
      printf("mRect %d %d %d %d\n", aData->mRect.x, aData->mRect.y, aData->mRect.width, aData->mRect.height);
      InvalidateRect(aData->mRect);
    } else if (/*aData->mIsInvalid || XXX: handle image load invalidation */ (item->IsInvalid(invalid) && invalid.IsEmpty())) {
      UniquePtr<nsDisplayItemGeometry> geometry(item->AllocateGeometry(builder));
      combined = aData->mClip.ApplyNonRoundedIntersection(aData->mGeometry->ComputeInvalidationRegion());
      combined.MoveBy(shift);
      combined.Or(combined, clip.ApplyNonRoundedIntersection(geometry->ComputeInvalidationRegion()));
      aData->mGeometry = Move(geometry);
      nsRect bounds = combined.GetBounds();
      auto transBounds = nsLayoutUtils::MatrixTransformRect(bounds,
                                                            Matrix4x4::From2D(mMatrix),
                                                            float(appUnitsPerDevPixel));


      printf("matrix: %f %f\n", mMatrix._31, mMatrix._32); 
      printf("transBounds %d %d %d %d\n", transBounds.x, transBounds.y, transBounds.width, transBounds.height);
      printf("frame invalid invalidate: %s\n", item->Name());
      printf("old rect: %d %d %d %d\n",
             aData->mRect.x,
             aData->mRect.y,
             aData->mRect.width,
             aData->mRect.height);
      InvalidateRect(aData->mRect);
      // We want to snap to outside pixels. When should we multiply by the matrix?
      // XXX: TransformBounds is expensive. We should avoid doing it if we have no transform
      IntRect transformedRect = RoundedOut(mMatrix.TransformBounds(ToRect(nsLayoutUtils::RectToGfxRect(combined.GetBounds(), appUnitsPerDevPixel)))) - mGroupOffset;
      aData->mRect = transformedRect.Intersect(imageRect);
      InvalidateRect(aData->mRect);
      printf("new rect: %d %d %d %d\n",
             aData->mRect.x,
             aData->mRect.y,
             aData->mRect.width,
             aData->mRect.height);
    } else {
      printf("else invalidate: %s\n", item->Name());
      // this includes situations like reflow changing the position
      item->ComputeInvalidationRegion(builder, aData->mGeometry.get(), &combined);
      if (!combined.IsEmpty()) {
        // InvalidateRect(aData->mRect); Do we need to invalidate the old rect?
        // Probably not.
        IntRect transformedRect = RoundedOut(mMatrix.TransformBounds(ToRect(nsLayoutUtils::RectToGfxRect(combined.GetBounds(), appUnitsPerDevPixel)))) - mGroupOffset;
        aData->mRect = transformedRect.Intersect(imageRect);
        // invalidate the invalidated area.
        InvalidateRect(aData->mRect);
      } else {
        printf("NoChange: %s\n", item->Name());
      }
    }
    printf("post mInvalidRect: %d %d %d %d\n", mInvalidRect.x, mInvalidRect.y, mInvalidRect.width, mInvalidRect.height);
  }

  void EndGroup(WebRenderLayerManager* aWrManager,
                wr::DisplayListBuilder& aBuilder,
                wr::IpcResourceUpdateQueue& aResources,
                Grouper* aGrouper,
                nsDisplayItem* aStartItem,
                nsDisplayItem* aEndItem) {

    mLastAnimatedGeometryRootOrigin = mAnimatedGeometryRootOrigin;
    printf("EndGroup\n");

    // Invalidate any unused items
    for (auto iter = mDisplayItems.Iter(); !iter.Done(); iter.Next()) {
      BlobItemData* data = iter.Get()->GetKey();
      // XXX: If we had two hash tables we could actually move items from one into the other
      // and then only iterate over the old items. However, this is probably more expensive
      // because doing an insert is more expensive than doing a lookup because of resizing?
      if (!data->mUsed) {
        InvalidateRect(data->mRect);
        iter.Remove();
      } else {
        data->mUsed = false;
      }
    }

    LayoutDeviceRect bounds = LayoutDeviceRect::FromAppUnits(mGroupBounds, aGrouper->mAppUnitsPerDevPixel);
    IntSize size = mGroupBounds.Size().ToNearestPixels(aGrouper->mAppUnitsPerDevPixel);
    // We only care about invalidations inside our drawing area.
    mInvalidRect = mInvalidRect.Intersect(IntRect(IntPoint(0, 0), size));

    if (mInvalidRect.IsEmpty()) {
      printf("Not painting group because it's empty\n");
      return;
    }

    gfx::SurfaceFormat format = gfx::SurfaceFormat::B8G8R8A8;
    RefPtr<gfx::DrawEventRecorderMemory> recorder =
      MakeAndAddRef<gfx::DrawEventRecorderMemory>(
        [&](MemStream &aStream, std::vector<RefPtr<UnscaledFont>> &aUnscaledFonts) {
          size_t count = aUnscaledFonts.size();
          aStream.write((const char*)&count, sizeof(count));
          for (auto unscaled : aUnscaledFonts) {
            wr::FontKey key = aWrManager->WrBridge()->GetFontKeyForUnscaledFont(unscaled);
            aStream.write((const char*)&key, sizeof(key));
          }
        });

    RefPtr<gfx::DrawTarget> dummyDt =
      gfx::Factory::CreateDrawTarget(gfx::BackendType::SKIA, gfx::IntSize(1, 1), format);

    RefPtr<gfx::DrawTarget> dt = gfx::Factory::CreateRecordingDrawTarget(recorder, dummyDt, size);
    // Setup the gfxContext
    RefPtr<gfxContext> context = gfxContext::CreateOrNull(dt);
    printf("ctx-offset %f %f\n", bounds.x, bounds.y);
    context->SetMatrix(Matrix::Translation(-bounds.x, -bounds.y));

    printf("mInvalidRect: %d %d %d %d\n", mInvalidRect.x, mInvalidRect.y, mInvalidRect.width, mInvalidRect.height);
    // Chase the invalidator and paint any invalid items.

    PaintItemRange(aGrouper, aStartItem, aEndItem, context, recorder);

    if (!mKey) {
      dt->FillRect(gfx::Rect(0, 0, size.width, size.height), gfx::ColorPattern(gfx::Color(0., 1., 0., 0.5)));
      dt->FlushItem(IntRect(IntPoint(0, 0), size));
    }
    bool isOpaque = false; // XXX: set this correctly
    //assert(end or active);

    recorder->Finish();
    Range<uint8_t> bytes((uint8_t*)recorder->mOutputStream.mData, recorder->mOutputStream.mLength);
    if (!mKey) {
      wr::ImageKey key = aWrManager->WrBridge()->GetNextImageKey();
      printf("No previous key making new one %d\n", key.mHandle);
      wr::ImageDescriptor descriptor(size, 0, dt->GetFormat(), isOpaque);
      if (!aResources.AddBlobImage(key, descriptor, bytes)) {
        return;
      }
      mKey = Some(key);
    } else {
      wr::ImageDescriptor descriptor(size, 0, dt->GetFormat(), isOpaque);
      auto bottomRight = mInvalidRect.BottomRight();
      MOZ_RELEASE_ASSERT(bottomRight.x <= size.width && bottomRight.y <= size.height);
      printf("Update Blob %d %d %d %d\n", mInvalidRect.x, mInvalidRect.y, mInvalidRect.width, mInvalidRect.height);
      if (!aResources.UpdateBlobImage(mKey.value(), descriptor, bytes, ViewAs<ImagePixel>(mInvalidRect))) {
        return;
      }
    }
    mInvalidRect.SetEmpty();
    wr::LayoutRect dest = wr::ToLayoutRect(bounds);
    printf("PushImage: %f %f %f %f\n", dest.origin.x, dest.origin.y, dest.size.width, dest.size.height);
    gfx::SamplingFilter sampleFilter = gfx::SamplingFilter::LINEAR; //nsLayoutUtils::GetSamplingFilterForFrame(aItem->Frame());
    bool backfaceHidden = false;
    aBuilder.PushImage(dest, dest, !backfaceHidden,
                       wr::ToImageRendering(sampleFilter),
                       mKey.value());
  }

  void PaintItemRange(Grouper* aGrouper,
                      nsDisplayItem* aStartItem,
                      nsDisplayItem* aEndItem,
                      gfxContext* aContext,
                      gfx::DrawEventRecorderMemory* aRecorder) {
    for (nsDisplayItem* item = aStartItem; item != aEndItem; item = item->GetAbove()) {
      IntRect bounds = ItemBounds(item);
      printf("Trying %s %d %d %d %d\n", item->Name(), bounds.x, bounds.y, bounds.width, bounds.height);
      // skip items not in inside the invalidation bounds
      if (!mInvalidRect.Intersects(bounds)) {
        printf("Passing\n");
        continue;
      }

      // XXX: will the DeviceBounds of nsDisplayTransform be correct?

      nsDisplayList* children = item->GetChildren();
      if (children) {
        printf("doing children in EndGroup\n");
        aGrouper->PushParent(this, item, aContext, aRecorder);
        PaintItemRange(aGrouper, children->GetBottom(), nullptr, aContext, aRecorder);
        aGrouper->PopParent(this, item, aContext, aRecorder);
      } else {
        // XXX: what's this for? flush_item(blobData.mRect);
        // We need to set the clip here.
        // What should the clip settting strategy be? We can set the full clip everytime.
        // this is probably easiest for now. An alternative would be to put the push and the pop
        // into separate items and let invalidation handle it that way.
        DisplayItemClip currentClip = item->GetClip();

        aContext->Save();
        int commonClipCount = 0; // Don't share any clips, always apply all clips.
        if (currentClip.HasClip()) {
          currentClip.ApplyTo(aContext, aGrouper->mAppUnitsPerDevPixel, commonClipCount);
        }
        aContext->NewPath();
        printf("painting %s\n", item->Name());
        item->Paint(aGrouper->mDisplayListBuilder, aContext);
        aContext->Restore();
        aContext->GetDrawTarget()->FlushItem(bounds);
      }
    }
  }
};

inline
void Grouper::PushParent(DIGroup *aGroup, nsDisplayItem *aItem, gfxContext *ctx, gfx::DrawEventRecorderMemory* aRecorder) {
    mItemStack.push_back(aItem);
    switch (aItem->GetType()) {
      case DisplayItemType::TYPE_TRANSFORM: {
                                              ctx->Save();
                                              auto transformItem = static_cast<nsDisplayTransform*>(aItem);
                                              auto trans = transformItem->GetTransform();
                                              Matrix m;
                                              MOZ_RELEASE_ASSERT(trans.Is2D(&m));
                                              ctx->Multiply(ThebesMatrix(m));
                                              //aGroup.Pushransform();
                                              break;
                                            }
      case DisplayItemType::TYPE_OPACITY: {
                                            //aGroup.PushGroup();
                                            ctx->GetDrawTarget()->FlushItem(aGroup->ItemBounds(aItem));
                                            break;
                                          }
      default: { break; }
    }
  }

inline
void Grouper::PopParent(DIGroup *aGroup, nsDisplayItem *aItem, gfxContext *ctx, gfx::DrawEventRecorderMemory* aRecorder) {
    switch (aItem->GetType()) {
      case DisplayItemType::TYPE_TRANSFORM: {
                                              ctx->Restore();
                                              //aGroup.PopTransform();
                                              break;
                                            }
      case DisplayItemType::TYPE_OPACITY: {
                                            //aGroup.PopGroup();
                                            ctx->GetDrawTarget()->FlushItem(aGroup->ItemBounds(aItem));
                                            break;
                                          }

      default: { break; }
    }
  }


// how do we drop mDisplayItems? A: It will be stored in a WebRenderUserData
//SVGItem() {

class WebRenderGroupData : public WebRenderUserData
{
public:
  explicit WebRenderGroupData(WebRenderLayerManager* aWRManager, nsDisplayItem* aItem);
  virtual ~WebRenderGroupData();

  virtual WebRenderGroupData* AsGroupData() override { return this; }
  virtual UserDataType GetType() override { return UserDataType::eGroupSplit; }
  static UserDataType Type() { return UserDataType::eGroupSplit; }

  DIGroup mSubGroup;
  DIGroup mFollowingGroup;
};

// We can't easily use GetLayerState because it wants a bunch of layers related information.
static bool
IsItemProbablyActive(nsDisplayItem* aItem, nsDisplayListBuilder* aDisplayListBuilder)
{
  return false;
  if (aItem->GetType() == DisplayItemType::TYPE_TRANSFORM) {
    nsDisplayTransform* transformItem = static_cast<nsDisplayTransform*>(aItem);
    Matrix4x4 t = transformItem->GetTransform();
    Matrix t2d;
    bool is2D = t.Is2D(&t2d);
    return transformItem->MayBeAnimated(aDisplayListBuilder) || !is2D;
  }
  // TODO: handle opacity etc.
  return false;
}

// This does a pass over the display lists and will join the display items
// into groups as well as paint them
void
Grouper::ConstructGroups(WebRenderCommandBuilder* aCommandBuilder,
                         wr::DisplayListBuilder& aBuilder,
                         wr::IpcResourceUpdateQueue& aResources,
                         DIGroup* aGroup, nsDisplayList* aList,
                         const StackingContextHelper& aSc)
{
  DIGroup* currentGroup = aGroup;

  nsDisplayItem* item = aList->GetBottom();
  nsDisplayItem* startOfCurrentGroup = item;
  while (item) {
    nsDisplayList* children = item->GetChildren();
    if (IsItemProbablyActive(item, mDisplayListBuilder)) {
      currentGroup->EndGroup(aCommandBuilder->mManager, aBuilder, aResources, this, startOfCurrentGroup, item);
      // Note: this call to CreateWebRenderCommands can recurse back into
      // this function.
      mScrollingHelper.BeginItem(item, aSc);
      bool createdWRCommands =
        item->CreateWebRenderCommands(aBuilder, aResources, aSc, aCommandBuilder->mManager,
                                      mDisplayListBuilder);
      MOZ_RELEASE_ASSERT(createdWRCommands, "active transforms should always succeed at creating WebRender commands");

      RefPtr<WebRenderGroupData> groupData =
        aCommandBuilder->CreateOrRecycleWebRenderUserData<WebRenderGroupData>(item);

      // Initialize groupData->mFollowingGroup
      // TODO: compute the group bounds post-grouping, so that they can be
      // tighter for just the sublist that made it into this group.
      groupData->mFollowingGroup.mGroupBounds = currentGroup->mGroupBounds;
      groupData->mFollowingGroup.mGroupOffset = currentGroup->mGroupOffset;

      currentGroup = &groupData->mFollowingGroup;

      startOfCurrentGroup = item->GetAbove();
    } else { // inactive item

      if (item->GetType() == DisplayItemType::TYPE_TRANSFORM) {
        nsDisplayTransform* transformItem = static_cast<nsDisplayTransform*>(item);
        Matrix4x4 t = transformItem->GetTransform();
        Matrix t2d;
        bool is2D = t.Is2D(&t2d);
        MOZ_RELEASE_ASSERT(is2D, "Non-2D transforms should be treated as active");

        //XXX what's this here for?
        // clear_display_items();
        Matrix m = mTransform;

        printf("t2d: %f %f\n", t2d._31, t2d._32); 
        mTransform.PreMultiply(t2d);
        printf("mTransform: %f %f\n", mTransform._31, mTransform._32); 
        ConstructGroupsInsideInactive(aCommandBuilder, aBuilder, aResources, currentGroup, transformItem->GetChildren(), aSc);
        mTransform = m;
      } else if (children) {
        ConstructGroupsInsideInactive(aCommandBuilder, aBuilder, aResources, currentGroup, children, aSc);
      }

      printf("Including %s\n", item->Name());

      BlobItemData* data = GetBlobItemData(item->Frame(), item->GetPerFrameKey());
      // Iterate over display items looking up their BlobItemData
      if (!data) {
        currentGroup->mDisplayItems.PutEntry(new BlobItemData(item));
        data = GetBlobItemData(item->Frame(), item->GetPerFrameKey());
      }
      data->mUsed = true;
      currentGroup->ComputeGeometryChange(item, data, mTransform, mDisplayListBuilder); // we compute the geometry change here because we have the transform around still

    }

    item = item->GetAbove();
  }

  currentGroup->EndGroup(aCommandBuilder->mManager, aBuilder, aResources, this, startOfCurrentGroup, nullptr);
}

// This does a pass over the display lists and will join the display items
// into groups as well as paint them
void
Grouper::ConstructGroupsInsideInactive(WebRenderCommandBuilder* aCommandBuilder,
                                       wr::DisplayListBuilder& aBuilder,
                                       wr::IpcResourceUpdateQueue& aResources,
                                       DIGroup* aGroup, nsDisplayList* aList,
                                       const StackingContextHelper& aSc)
{
  DIGroup* currentGroup = aGroup;

  nsDisplayItem* item = aList->GetBottom();
  while (item) {
    nsDisplayList* children = item->GetChildren();

    if (item->GetType() == DisplayItemType::TYPE_TRANSFORM) {
      nsDisplayTransform* transformItem = static_cast<nsDisplayTransform*>(item);
      Matrix4x4 t = transformItem->GetTransform();
      Matrix t2d;
      bool is2D = t.Is2D(&t2d);
      MOZ_RELEASE_ASSERT(is2D, "Non-2D transforms should be treated as active");

      //XXX what's this here for?
      // clear_display_items();
      Matrix m = mTransform;

      printf("t2d: %f %f\n", t2d._31, t2d._32); 
      mTransform.PreMultiply(t2d);
      printf("mTransform: %f %f\n", mTransform._31, mTransform._32); 
      ConstructGroupsInsideInactive(aCommandBuilder, aBuilder, aResources, currentGroup, transformItem->GetChildren(), aSc);
      mTransform = m;
    } else if (children) {
      ConstructGroupsInsideInactive(aCommandBuilder, aBuilder, aResources, currentGroup, children, aSc);
    }

    printf("Including %s\n", item->Name());

    BlobItemData* data = GetBlobItemData(item->Frame(), item->GetPerFrameKey());
    // Iterate over display items looking up their BlobItemData
    if (!data) {
      currentGroup->mDisplayItems.PutEntry(new BlobItemData(item));
      data = GetBlobItemData(item->Frame(), item->GetPerFrameKey());
    }
    data->mUsed = true;
    currentGroup->ComputeGeometryChange(item, data, mTransform, mDisplayListBuilder); // we compute the geometry change here because we have the transform around still

    item = item->GetAbove();
  }
}

void
WebRenderCommandBuilder::DoGroupingForDisplayList(nsDisplayList* aList,
                                                  nsDisplayItem* aWrappingItem,
                                                  nsDisplayListBuilder* aDisplayListBuilder,
                                                  const StackingContextHelper& aSc,
                                                  wr::DisplayListBuilder& aBuilder,
                                                  wr::IpcResourceUpdateQueue& aResources)
{
  if (!aList->GetBottom()) {
    return;
  }

  // mScrollingHelper.BeginList();
  Grouper g(mScrollingHelper);
  g.mAppUnitsPerDevPixel = aWrappingItem->Frame()->PresContext()->AppUnitsPerDevPixel();
  printf("DoGroupingForDisplayList\n");

  g.mDisplayListBuilder = aDisplayListBuilder;
  RefPtr<WebRenderGroupData> groupData = CreateOrRecycleWebRenderUserData<WebRenderGroupData>(aWrappingItem);
  bool snapped;
  nsRect groupBounds = aWrappingItem->GetBounds(aDisplayListBuilder, &snapped);
  DIGroup& group = groupData->mSubGroup;
  if (!group.mGroupBounds.IsEqualEdges(groupBounds)) {
    // The bounds have changed so we need to discard the old image and add all
    // the commands again.
    auto p = group.mGroupBounds;
    auto q = groupBounds;
    printf("Bounds change: %d %d %d %d vs %d %d %d %d\n", p.x, p.y, p.width, p.height, q.x, q.y, q.width, q.height);
    if (group.mKey) {
      IntSize size = groupBounds.Size().ToNearestPixels(g.mAppUnitsPerDevPixel);
      group.mInvalidRect = IntRect(IntPoint(0, 0), size);
      mManager->AddImageKeyForDiscard(group.mKey.value());
      group.mKey = Nothing();
    }
  }
  group.mGroupBounds = groupBounds;
  group.mGroupOffset = group.mGroupBounds.TopLeft().ToNearestPixels(g.mAppUnitsPerDevPixel);
  g.ConstructGroups(this, aBuilder, aResources, &group, aList, aSc);
  // mScrollingHelper.EndList();
}

// transform becomes inactive
// old BlobItem datas are removed
// new BlobItem data are created
//
//
// how to deal with off-by-ones? i.e. the first transform is active? Probably easiest
// to just check if the blob image is empty and then not send it.

// we need to deal with transforms. We could do that in computeGeometryChange by comparing post transformed rects.
// This is probably the simplest approach.

// to support multiple blob images with multiple passes over the tree we have some different options:
// 1. Do a prepass to split things up
// 2. Have some kind of lagging tree traversal
//    - this is easy. You just duplicate the iterator state and have it lag and have the ability for it to catch up
/*      curItem = aDisplayList->GetBottom();
      curItem = curItem->GetAbove();
      curItem != nullptr;

      but not so easy afterall because we don't have multiple stacks needed.
      we'll synthesize it with a Vec for items and a Vec for matrices
*/
// 3.


// XXX: we can't have the chasing work unless we have two stacks:
//   invalidate_push();
//   invalidate_push();
//   active();
//   paint_push();
//   paint_push();
//   invalidate_pop();
//   invalidate_pop();
//   paint_pop();
//   paint_pop();


void WebRenderCommandBuilder::Destroy()
{
  mLastCanvasDatas.Clear();
  RemoveUnusedAndResetWebRenderUserData();
}

void
WebRenderCommandBuilder::EmptyTransaction()
{
  // We need to update canvases that might have changed.
  for (auto iter = mLastCanvasDatas.Iter(); !iter.Done(); iter.Next()) {
    RefPtr<WebRenderCanvasData> canvasData = iter.Get()->GetKey();
    WebRenderCanvasRendererAsync* canvas = canvasData->GetCanvasRenderer();
    if (canvas) {
      canvas->UpdateCompositableClient();
    }
  }
}

bool
WebRenderCommandBuilder::NeedsEmptyTransaction()
{
  return !mLastCanvasDatas.IsEmpty();
}

void
WebRenderCommandBuilder::BuildWebRenderCommands(wr::DisplayListBuilder& aBuilder,
                                                wr::IpcResourceUpdateQueue& aResourceUpdates,
                                                nsDisplayList* aDisplayList,
                                                nsDisplayListBuilder* aDisplayListBuilder,
                                                WebRenderScrollData& aScrollData,
                                                wr::LayoutSize& aContentSize)
{
  { // scoping for StackingContextHelper RAII

    StackingContextHelper sc;
    mParentCommands.Clear();
    aScrollData = WebRenderScrollData(mManager);
    MOZ_ASSERT(mLayerScrollData.empty());
    mLastCanvasDatas.Clear();
    mLastAsr = nullptr;
    mScrollingHelper.BeginBuild(mManager, aBuilder);

    {
      StackingContextHelper pageRootSc(sc, aBuilder);
      CreateWebRenderCommandsFromDisplayList(aDisplayList, nullptr, aDisplayListBuilder,
                                             pageRootSc, aBuilder, aResourceUpdates);
    }

    // Make a "root" layer data that has everything else as descendants
    mLayerScrollData.emplace_back();
    mLayerScrollData.back().InitializeRoot(mLayerScrollData.size() - 1);
    auto callback = [&aScrollData](FrameMetrics::ViewID aScrollId) -> bool {
      return aScrollData.HasMetadataFor(aScrollId).isSome();
    };
    if (Maybe<ScrollMetadata> rootMetadata = nsLayoutUtils::GetRootMetadata(
          aDisplayListBuilder, mManager, ContainerLayerParameters(), callback)) {
      mLayerScrollData.back().AppendScrollMetadata(aScrollData, rootMetadata.ref());
    }
    // Append the WebRenderLayerScrollData items into WebRenderScrollData
    // in reverse order, from topmost to bottommost. This is in keeping with
    // the semantics of WebRenderScrollData.
    for (auto i = mLayerScrollData.crbegin(); i != mLayerScrollData.crend(); i++) {
      aScrollData.AddLayerData(*i);
    }
    mLayerScrollData.clear();
    mScrollingHelper.EndBuild();

    // Remove the user data those are not displayed on the screen and
    // also reset the data to unused for next transaction.
    RemoveUnusedAndResetWebRenderUserData();
  }

  mManager->WrBridge()->AddWebRenderParentCommands(mParentCommands);
}

void
WebRenderCommandBuilder::CreateWebRenderCommandsFromDisplayList(nsDisplayList* aDisplayList,
                                                                nsDisplayItem* aWrappingItem,
                                                                nsDisplayListBuilder* aDisplayListBuilder,
                                                                const StackingContextHelper& aSc,
                                                                wr::DisplayListBuilder& aBuilder,
                                                                wr::IpcResourceUpdateQueue& aResources)
{
  if (mDoGrouping) {
    MOZ_RELEASE_ASSERT(aWrappingItem, "Only the root list should have a null wrapping item, and mDoGrouping should never be true for the root list.");
    printf("actually entering the grouping code\n");
    DoGroupingForDisplayList(aDisplayList, aWrappingItem, aDisplayListBuilder, aSc, aBuilder, aResources);
    return;
  }
  mScrollingHelper.BeginList();

  bool apzEnabled = mManager->AsyncPanZoomEnabled();
  EventRegions eventRegions;

  FlattenedDisplayItemIterator iter(aDisplayListBuilder, aDisplayList);
  while (nsDisplayItem* i = iter.GetNext()) {
    nsDisplayItem* item = i;
    DisplayItemType itemType = item->GetType();

    // If the item is a event regions item, but is empty (has no regions in it)
    // then we should just throw it out
    if (itemType == DisplayItemType::TYPE_LAYER_EVENT_REGIONS) {
      nsDisplayLayerEventRegions* eventRegions =
        static_cast<nsDisplayLayerEventRegions*>(item);
      if (eventRegions->IsEmpty()) {
        continue;
      }
    }

    // Peek ahead to the next item and try merging with it or swapping with it
    // if necessary.
    AutoTArray<nsDisplayItem*, 1> mergedItems;
    mergedItems.AppendElement(item);
    while (nsDisplayItem* peek = iter.PeekNext()) {
      if (!item->CanMerge(peek)) {
        break;
      }

      mergedItems.AppendElement(peek);

      // Move the iterator forward since we will merge this item.
      i = iter.GetNext();
    }

    if (mergedItems.Length() > 1) {
      item = aDisplayListBuilder->MergeItems(mergedItems);
      MOZ_ASSERT(item && itemType == item->GetType());
    }

    bool forceNewLayerData = false;
    size_t layerCountBeforeRecursing = mLayerScrollData.size();
    if (apzEnabled) {
      // For some types of display items we want to force a new
      // WebRenderLayerScrollData object, to ensure we preserve the APZ-relevant
      // data that is in the display item.
      forceNewLayerData = item->UpdateScrollData(nullptr, nullptr);

      // Anytime the ASR changes we also want to force a new layer data because
      // the stack of scroll metadata is going to be different for this
      // display item than previously, so we can't squash the display items
      // into the same "layer".
      const ActiveScrolledRoot* asr = item->GetActiveScrolledRoot();
      if (asr != mLastAsr) {
        mLastAsr = asr;
        forceNewLayerData = true;
      }

      // If we're creating a new layer data then flush whatever event regions
      // we've collected onto the old layer.
      if (forceNewLayerData && !eventRegions.IsEmpty()) {
        // If eventRegions is non-empty then we must have a layer data already,
        // because we (below) force one if we encounter an event regions item
        // with an empty layer data list. Additionally, the most recently
        // created layer data must have been created from an item whose ASR
        // is the same as the ASR on the event region items that were collapsed
        // into |eventRegions|. This is because any ASR change causes us to force
        // a new layer data which flushes the eventRegions.
        MOZ_ASSERT(!mLayerScrollData.empty());
        mLayerScrollData.back().AddEventRegions(eventRegions);
        eventRegions.SetEmpty();
      }

      // Collapse event region data into |eventRegions|, which will either be
      // empty, or filled with stuff from previous display items with the same
      // ASR.
      if (itemType == DisplayItemType::TYPE_LAYER_EVENT_REGIONS) {
        nsDisplayLayerEventRegions* regionsItem =
            static_cast<nsDisplayLayerEventRegions*>(item);
        int32_t auPerDevPixel = item->Frame()->PresContext()->AppUnitsPerDevPixel();
        EventRegions regions(
            regionsItem->HitRegion().ScaleToOutsidePixels(1.0f, 1.0f, auPerDevPixel),
            regionsItem->MaybeHitRegion().ScaleToOutsidePixels(1.0f, 1.0f, auPerDevPixel),
            regionsItem->DispatchToContentHitRegion().ScaleToOutsidePixels(1.0f, 1.0f, auPerDevPixel),
            regionsItem->NoActionRegion().ScaleToOutsidePixels(1.0f, 1.0f, auPerDevPixel),
            regionsItem->HorizontalPanRegion().ScaleToOutsidePixels(1.0f, 1.0f, auPerDevPixel),
            regionsItem->VerticalPanRegion().ScaleToOutsidePixels(1.0f, 1.0f, auPerDevPixel));

        eventRegions.OrWith(regions);
        if (mLayerScrollData.empty()) {
          // If we don't have a layer data yet then create one because we will
          // need it to store this event region information.
          forceNewLayerData = true;
        }
      }

      // If we're going to create a new layer data for this item, stash the
      // ASR so that if we recurse into a sublist they will know where to stop
      // walking up their ASR chain when building scroll metadata.
      if (forceNewLayerData) {
        mAsrStack.push_back(asr);
      }
    }

    mScrollingHelper.BeginItem(item, aSc);

    if (itemType != DisplayItemType::TYPE_LAYER_EVENT_REGIONS) {
      AutoRestore<bool> restoreDoGrouping(mDoGrouping);
      if (itemType == DisplayItemType::TYPE_SVG_WRAPPER) {
        // Inside an <svg>, all display items that are not LAYER_ACTIVE wrapper
        // display items (like animated transforms / opacity) share the same
        // animated geometry root, so we can combine subsequent items of that
        // type into the same image.
        mDoGrouping = true;
        printf("attempting to enter the grouping code\n");
      }/* else if (itemType == DisplayItemType::TYPE_FOREIGN_OBJECT) {
        // We do not want to apply grouping inside <foreignObject>.
        // TODO: TYPE_FOREIGN_OBJECT does not exist yet, make it exist
        mDoGrouping = false;
      }*/

      // Note: this call to CreateWebRenderCommands can recurse back into
      // this function if the |item| is a wrapper for a sublist.
      bool createdWRCommands =
        item->CreateWebRenderCommands(aBuilder, aResources, aSc, mManager,
                                      aDisplayListBuilder);
      if (!createdWRCommands) {
        PushItemAsImage(item, aBuilder, aResources, aSc, aDisplayListBuilder);
      }
    }

    if (apzEnabled) {
      if (forceNewLayerData) {
        // Pop the thing we pushed before the recursion, so the topmost item on
        // the stack is enclosing display item's ASR (or the stack is empty)
        mAsrStack.pop_back();
        const ActiveScrolledRoot* stopAtAsr =
            mAsrStack.empty() ? nullptr : mAsrStack.back();

        int32_t descendants = mLayerScrollData.size() - layerCountBeforeRecursing;

        mLayerScrollData.emplace_back();
        mLayerScrollData.back().Initialize(mManager->GetScrollData(), item, descendants, stopAtAsr);
      } else if (mLayerScrollData.size() != layerCountBeforeRecursing &&
                 !eventRegions.IsEmpty()) {
        // We are not forcing a new layer for |item|, but we did create some
        // layers while recursing. In this case, we need to make sure any
        // event regions that we were carrying end up on the right layer. So we
        // do an event region "flush" but retroactively; i.e. the event regions
        // end up on the layer that was mLayerScrollData.back() prior to the
        // recursion.
        MOZ_ASSERT(layerCountBeforeRecursing > 0);
        mLayerScrollData[layerCountBeforeRecursing - 1].AddEventRegions(eventRegions);
        eventRegions.SetEmpty();
      }
    }
  }

  // If we have any event region info left over we need to flush it before we
  // return. Again, at this point the layer data list must be non-empty, and
  // the most recently created layer data will have been created by an item
  // with matching ASRs.
  if (!eventRegions.IsEmpty()) {
    MOZ_ASSERT(apzEnabled);
    MOZ_ASSERT(!mLayerScrollData.empty());
    mLayerScrollData.back().AddEventRegions(eventRegions);
  }

  mScrollingHelper.EndList();
}

Maybe<wr::ImageKey>
WebRenderCommandBuilder::CreateImageKey(nsDisplayItem* aItem,
                                        ImageContainer* aContainer,
                                        mozilla::wr::DisplayListBuilder& aBuilder,
                                        mozilla::wr::IpcResourceUpdateQueue& aResources,
                                        const StackingContextHelper& aSc,
                                        gfx::IntSize& aSize,
                                        const Maybe<LayoutDeviceRect>& aAsyncImageBounds)
{
  RefPtr<WebRenderImageData> imageData = CreateOrRecycleWebRenderUserData<WebRenderImageData>(aItem);
  MOZ_ASSERT(imageData);

  if (aContainer->IsAsync()) {
    MOZ_ASSERT(aAsyncImageBounds);

    LayoutDeviceRect rect = aAsyncImageBounds.value();
    LayoutDeviceRect scBounds(LayoutDevicePoint(0, 0), rect.Size());
    gfx::MaybeIntSize scaleToSize;
    if (!aContainer->GetScaleHint().IsEmpty()) {
      scaleToSize = Some(aContainer->GetScaleHint());
    }
    // TODO!
    // We appear to be using the image bridge for a lot (most/all?) of
    // layers-free image handling and that breaks frame consistency.
    imageData->CreateAsyncImageWebRenderCommands(aBuilder,
                                                 aContainer,
                                                 aSc,
                                                 rect,
                                                 scBounds,
                                                 gfx::Matrix4x4(),
                                                 scaleToSize,
                                                 wr::ImageRendering::Auto,
                                                 wr::MixBlendMode::Normal,
                                                 !aItem->BackfaceIsHidden());
    return Nothing();
  }

  AutoLockImage autoLock(aContainer);
  if (!autoLock.HasImage()) {
    return Nothing();
  }
  mozilla::layers::Image* image = autoLock.GetImage();
  aSize = image->GetSize();

  return imageData->UpdateImageKey(aContainer, aResources);
}

bool
WebRenderCommandBuilder::PushImage(nsDisplayItem* aItem,
                                   ImageContainer* aContainer,
                                   mozilla::wr::DisplayListBuilder& aBuilder,
                                   mozilla::wr::IpcResourceUpdateQueue& aResources,
                                   const StackingContextHelper& aSc,
                                   const LayoutDeviceRect& aRect)
{
  gfx::IntSize size;
  Maybe<wr::ImageKey> key = CreateImageKey(aItem, aContainer,
                                           aBuilder, aResources,
                                           aSc, size, Some(aRect));
  if (aContainer->IsAsync()) {
    // Async ImageContainer does not create ImageKey, instead it uses Pipeline.
    MOZ_ASSERT(key.isNothing());
    return true;
  }
  if (!key) {
    return false;
  }

  auto r = aSc.ToRelativeLayoutRect(aRect);
  gfx::SamplingFilter sampleFilter = nsLayoutUtils::GetSamplingFilterForFrame(aItem->Frame());
  aBuilder.PushImage(r, r, !aItem->BackfaceIsHidden(), wr::ToImageRendering(sampleFilter), key.value());

  return true;
}

static bool
PaintByLayer(nsDisplayItem* aItem,
             nsDisplayListBuilder* aDisplayListBuilder,
             const RefPtr<BasicLayerManager>& aManager,
             gfxContext* aContext,
             const gfx::Size& aScale,
             const std::function<void()>& aPaintFunc)
{
  UniquePtr<LayerProperties> props;
  if (aManager->GetRoot()) {
    props = Move(LayerProperties::CloneFrom(aManager->GetRoot()));
  }
  FrameLayerBuilder* layerBuilder = new FrameLayerBuilder();
  layerBuilder->Init(aDisplayListBuilder, aManager, nullptr, true);
  layerBuilder->DidBeginRetainedLayerTransaction(aManager);

  aManager->SetDefaultTarget(aContext);
  aManager->BeginTransactionWithTarget(aContext);
  bool isInvalidated = false;

  ContainerLayerParameters param(aScale.width, aScale.height);
  RefPtr<Layer> root = aItem->BuildLayer(aDisplayListBuilder, aManager, param);

  if (root) {
    aManager->SetRoot(root);
    layerBuilder->WillEndTransaction();

    aPaintFunc();

    // Check if there is any invalidation region.
    nsIntRegion invalid;
    if (props) {
      props->ComputeDifferences(root, invalid, nullptr);
      if (!invalid.IsEmpty()) {
        isInvalidated = true;
      }
    } else {
      isInvalidated = true;
    }
  }

#ifdef MOZ_DUMP_PAINTING
  if (gfxUtils::DumpDisplayList() || gfxEnv::DumpPaint()) {
    fprintf_stderr(gfxUtils::sDumpPaintFile, "Basic layer tree for painting contents of display item %s(%p):\n", aItem->Name(), aItem->Frame());
    std::stringstream stream;
    aManager->Dump(stream, "", gfxEnv::DumpPaintToFile());
    fprint_stderr(gfxUtils::sDumpPaintFile, stream);  // not a typo, fprint_stderr declared in LayersLogging.h
  }
#endif

  if (aManager->InTransaction()) {
    aManager->AbortTransaction();
  }

  aManager->SetTarget(nullptr);
  aManager->SetDefaultTarget(nullptr);

  return isInvalidated;
}

static bool
PaintItemByDrawTarget(nsDisplayItem* aItem,
                      gfx::DrawTarget* aDT,
                      const LayerRect& aImageRect,
                      const LayoutDevicePoint& aOffset,
                      nsDisplayListBuilder* aDisplayListBuilder,
                      const RefPtr<BasicLayerManager>& aManager,
                      const gfx::Size& aScale,
                      Maybe<gfx::Color>& aHighlight)
{
  MOZ_ASSERT(aDT);

  bool isInvalidated = false;
  aDT->ClearRect(aImageRect.ToUnknownRect());
  RefPtr<gfxContext> context = gfxContext::CreateOrNull(aDT);
  MOZ_ASSERT(context);

  switch (aItem->GetType()) {
  case DisplayItemType::TYPE_MASK:
    context->SetMatrix(context->CurrentMatrix().PreScale(aScale.width, aScale.height).PreTranslate(-aOffset.x, -aOffset.y));
    static_cast<nsDisplayMask*>(aItem)->PaintMask(aDisplayListBuilder, context);
    isInvalidated = true;
    break;
  case DisplayItemType::TYPE_SVG_WRAPPER:
    {
      context->SetMatrix(context->CurrentMatrix().PreTranslate(-aOffset.x, -aOffset.y));
      isInvalidated = PaintByLayer(aItem, aDisplayListBuilder, aManager, context, aScale, [&]() {
        aManager->EndTransaction(FrameLayerBuilder::DrawPaintedLayer, aDisplayListBuilder);
      });
      break;
    }

  case DisplayItemType::TYPE_FILTER:
    {
      context->SetMatrix(context->CurrentMatrix().PreTranslate(-aOffset.x, -aOffset.y));
      isInvalidated = PaintByLayer(aItem, aDisplayListBuilder, aManager, context, aScale, [&]() {
        static_cast<nsDisplayFilter*>(aItem)->PaintAsLayer(aDisplayListBuilder,
                                                           context, aManager);
      });
      break;
    }

  default:
    context->SetMatrix(context->CurrentMatrix().PreScale(aScale.width, aScale.height).PreTranslate(-aOffset.x, -aOffset.y));
    aItem->Paint(aDisplayListBuilder, context);
    isInvalidated = true;
    break;
  }

  if (aItem->GetType() != DisplayItemType::TYPE_MASK) {
    // Apply highlight fills, if the appropriate prefs are set.
    // We don't do this for masks because we'd be filling the A8 mask surface,
    // which isn't very useful.
    if (aHighlight) {
      aDT->SetTransform(gfx::Matrix());
      aDT->FillRect(gfx::Rect(0, 0, aImageRect.Width(), aImageRect.Height()), gfx::ColorPattern(aHighlight.value()));
    }
    if (aItem->Frame()->PresContext()->GetPaintFlashing() && isInvalidated) {
      aDT->SetTransform(gfx::Matrix());
      float r = float(rand()) / RAND_MAX;
      float g = float(rand()) / RAND_MAX;
      float b = float(rand()) / RAND_MAX;
      aDT->FillRect(gfx::Rect(0, 0, aImageRect.Width(), aImageRect.Height()), gfx::ColorPattern(gfx::Color(r, g, b, 0.5)));
    }
  }

  return isInvalidated;
}

already_AddRefed<WebRenderFallbackData>
WebRenderCommandBuilder::GenerateFallbackData(nsDisplayItem* aItem,
                                              wr::DisplayListBuilder& aBuilder,
                                              wr::IpcResourceUpdateQueue& aResources,
                                              const StackingContextHelper& aSc,
                                              nsDisplayListBuilder* aDisplayListBuilder,
                                              LayoutDeviceRect& aImageRect)
{
  bool useBlobImage = gfxPrefs::WebRenderBlobImages() && !aItem->MustPaintOnContentSide();
  Maybe<gfx::Color> highlight = Nothing();
  if (gfxPrefs::WebRenderHighlightPaintedLayers()) {
    highlight = Some(useBlobImage ? gfx::Color(1.0, 0.0, 0.0, 0.5)
                                  : gfx::Color(1.0, 1.0, 0.0, 0.5));
  }

  RefPtr<WebRenderFallbackData> fallbackData = CreateOrRecycleWebRenderUserData<WebRenderFallbackData>(aItem);

  bool snap;
  nsRect itemBounds = aItem->GetBounds(aDisplayListBuilder, &snap);

  // Blob images will only draw the visible area of the blob so we don't need to clip
  // them here and can just rely on the webrender clipping.
  // TODO We also don't clip native themed widget to avoid over-invalidation during scrolling.
  // it would be better to support a sort of straming/tiling scheme for large ones but the hope
  // is that we should not have large native themed items.
  nsRect paintBounds = itemBounds;
  if (useBlobImage || aItem->MustPaintOnContentSide()) {
    paintBounds = itemBounds;
  } else {
    paintBounds = aItem->GetClippedBounds(aDisplayListBuilder);
  }

  // nsDisplayItem::Paint() may refer the variables that come from ComputeVisibility().
  // So we should call ComputeVisibility() before painting. e.g.: nsDisplayBoxShadowInner
  // uses mVisibleRegion in Paint() and mVisibleRegion is computed in
  // nsDisplayBoxShadowInner::ComputeVisibility().
  nsRegion visibleRegion(paintBounds);
  aItem->SetVisibleRect(paintBounds, false);
  aItem->ComputeVisibility(aDisplayListBuilder, &visibleRegion);

  const int32_t appUnitsPerDevPixel = aItem->Frame()->PresContext()->AppUnitsPerDevPixel();
  LayoutDeviceRect bounds = LayoutDeviceRect::FromAppUnits(paintBounds, appUnitsPerDevPixel);

  gfx::Size scale = aSc.GetInheritedScale();
  gfx::Size oldScale = fallbackData->GetScale();
  // This scale determination should probably be done using
  // ChooseScaleAndSetTransform but for now we just fake it.
  // We tolerate slight changes in scale so that we don't, for example,
  // rerasterize on MotionMark
  bool differentScale = gfx::FuzzyEqual(scale.width, oldScale.width, 1e-6f) &&
                        gfx::FuzzyEqual(scale.height, oldScale.height, 1e-6f);

  // XXX not sure if paintSize should be in layer or layoutdevice pixels, it
  // has some sort of scaling applied.
  LayerIntSize paintSize = RoundedToInt(LayerSize(bounds.width * scale.width, bounds.height * scale.height));
  if (paintSize.width == 0 || paintSize.height == 0) {
    return nullptr;
  }

  bool needPaint = true;
  LayoutDeviceIntPoint offset = RoundedToInt(bounds.TopLeft());
  aImageRect = LayoutDeviceRect(offset, LayoutDeviceSize(RoundedToInt(bounds.Size())));
  LayerRect paintRect = LayerRect(LayerPoint(0, 0), LayerSize(paintSize));
  nsDisplayItemGeometry* geometry = fallbackData->GetGeometry();

  // nsDisplayFilter is rendered via BasicLayerManager which means the invalidate
  // region is unknown until we traverse the displaylist contained by it.
  if (geometry && !fallbackData->IsInvalid() &&
      aItem->GetType() != DisplayItemType::TYPE_FILTER &&
      aItem->GetType() != DisplayItemType::TYPE_SVG_WRAPPER &&
      differentScale) {
    nsRect invalid;
    nsRegion invalidRegion;

    if (aItem->IsInvalid(invalid)) {
      invalidRegion.OrWith(paintBounds);
    } else {
      nsPoint shift = itemBounds.TopLeft() - geometry->mBounds.TopLeft();
      geometry->MoveBy(shift);
      aItem->ComputeInvalidationRegion(aDisplayListBuilder, geometry, &invalidRegion);

      nsRect lastBounds = fallbackData->GetBounds();
      lastBounds.MoveBy(shift);

      if (!lastBounds.IsEqualInterior(paintBounds)) {
        invalidRegion.OrWith(lastBounds);
        invalidRegion.OrWith(paintBounds);
      }
    }
    needPaint = !invalidRegion.IsEmpty();
  }

  if (needPaint || !fallbackData->GetKey()) {
    nsAutoPtr<nsDisplayItemGeometry> newGeometry;
    newGeometry = aItem->AllocateGeometry(aDisplayListBuilder);
    fallbackData->SetGeometry(Move(newGeometry));

    gfx::SurfaceFormat format = aItem->GetType() == DisplayItemType::TYPE_MASK ?
                                                      gfx::SurfaceFormat::A8 : gfx::SurfaceFormat::B8G8R8A8;
    if (useBlobImage) {
      bool snapped;
      bool isOpaque = aItem->GetOpaqueRegion(aDisplayListBuilder, &snapped).Contains(paintBounds);

      RefPtr<gfx::DrawEventRecorderMemory> recorder = MakeAndAddRef<gfx::DrawEventRecorderMemory>([&] (MemStream &aStream, std::vector<RefPtr<UnscaledFont>> &aUnscaledFonts) {
          size_t count = aUnscaledFonts.size();
          aStream.write((const char*)&count, sizeof(count));
          for (auto unscaled : aUnscaledFonts) {
            wr::FontKey key = mManager->WrBridge()->GetFontKeyForUnscaledFont(unscaled);
            aStream.write((const char*)&key, sizeof(key));
          }
        });
      RefPtr<gfx::DrawTarget> dummyDt =
        gfx::Factory::CreateDrawTarget(gfx::BackendType::SKIA, gfx::IntSize(1, 1), format);
      RefPtr<gfx::DrawTarget> dt = gfx::Factory::CreateRecordingDrawTarget(recorder, dummyDt, paintSize.ToUnknownSize());
      if (!fallbackData->mBasicLayerManager) {
        fallbackData->mBasicLayerManager = new BasicLayerManager(BasicLayerManager::BLM_INACTIVE);
      }
      bool isInvalidated = PaintItemByDrawTarget(aItem, dt, paintRect, offset, aDisplayListBuilder,
                                                 fallbackData->mBasicLayerManager, scale, highlight);
      recorder->FlushItem(IntRect(0, 0, paintSize.width, paintSize.height));
      recorder->Finish();

      if (isInvalidated) {
        Range<uint8_t> bytes((uint8_t *)recorder->mOutputStream.mData, recorder->mOutputStream.mLength);
        wr::ImageKey key = mManager->WrBridge()->GetNextImageKey();
        wr::ImageDescriptor descriptor(paintSize.ToUnknownSize(), 0, dt->GetFormat(), isOpaque);
        if (!aResources.AddBlobImage(key, descriptor, bytes)) {
          return nullptr;
        }
        fallbackData->SetKey(key);
      } else {
        // If there is no invalidation region and we don't have a image key,
        // it means we don't need to push image for the item.
        if (!fallbackData->GetKey().isSome()) {
          return nullptr;
        }
      }
    } else {
      fallbackData->CreateImageClientIfNeeded();
      RefPtr<ImageClient> imageClient = fallbackData->GetImageClient();
      RefPtr<ImageContainer> imageContainer = LayerManager::CreateImageContainer();
      bool isInvalidated = false;

      {
        UpdateImageHelper helper(imageContainer, imageClient, paintSize.ToUnknownSize(), format);
        {
          RefPtr<gfx::DrawTarget> dt = helper.GetDrawTarget();
          if (!dt) {
            return nullptr;
          }
          if (!fallbackData->mBasicLayerManager) {
            fallbackData->mBasicLayerManager = new BasicLayerManager(mManager->GetWidget());
          }
          isInvalidated = PaintItemByDrawTarget(aItem, dt, paintRect, offset,
                                               aDisplayListBuilder,
                                               fallbackData->mBasicLayerManager, scale,
                                               highlight);
        }

        if (isInvalidated) {
          // Update image if there it's invalidated.
          if (!helper.UpdateImage()) {
            return nullptr;
          }
        } else {
          // If there is no invalidation region and we don't have a image key,
          // it means we don't need to push image for the item.
          if (!fallbackData->GetKey().isSome()) {
            return nullptr;
          }
        }
      }

      // Force update the key in fallback data since we repaint the image in this path.
      // If not force update, fallbackData may reuse the original key because it
      // doesn't know UpdateImageHelper already updated the image container.
      if (isInvalidated && !fallbackData->UpdateImageKey(imageContainer, aResources, true)) {
        return nullptr;
      }
    }

    fallbackData->SetScale(scale);
    fallbackData->SetInvalid(false);
  }

  // Update current bounds to fallback data
  fallbackData->SetBounds(paintBounds);

  MOZ_ASSERT(fallbackData->GetKey());

  return fallbackData.forget();
}

Maybe<wr::WrImageMask>
WebRenderCommandBuilder::BuildWrMaskImage(nsDisplayItem* aItem,
                                          wr::DisplayListBuilder& aBuilder,
                                          wr::IpcResourceUpdateQueue& aResources,
                                          const StackingContextHelper& aSc,
                                          nsDisplayListBuilder* aDisplayListBuilder,
                                          const LayoutDeviceRect& aBounds)
{
  LayoutDeviceRect imageRect;
  RefPtr<WebRenderFallbackData> fallbackData = GenerateFallbackData(aItem, aBuilder, aResources,
                                                                    aSc, aDisplayListBuilder,
                                                                    imageRect);
  if (!fallbackData) {
    return Nothing();
  }

  wr::WrImageMask imageMask;
  imageMask.image = fallbackData->GetKey().value();
  imageMask.rect = aSc.ToRelativeLayoutRect(aBounds);
  imageMask.repeat = false;
  return Some(imageMask);
}

bool
WebRenderCommandBuilder::PushItemAsImage(nsDisplayItem* aItem,
                                         wr::DisplayListBuilder& aBuilder,
                                         wr::IpcResourceUpdateQueue& aResources,
                                         const StackingContextHelper& aSc,
                                         nsDisplayListBuilder* aDisplayListBuilder)
{
  LayoutDeviceRect imageRect;
  RefPtr<WebRenderFallbackData> fallbackData = GenerateFallbackData(aItem, aBuilder, aResources,
                                                                    aSc, aDisplayListBuilder,
                                                                    imageRect);
  if (!fallbackData) {
    return false;
  }

  wr::LayoutRect dest = aSc.ToRelativeLayoutRect(imageRect);
  gfx::SamplingFilter sampleFilter = nsLayoutUtils::GetSamplingFilterForFrame(aItem->Frame());
  aBuilder.PushImage(dest,
                     dest,
                     !aItem->BackfaceIsHidden(),
                     wr::ToImageRendering(sampleFilter),
                     fallbackData->GetKey().value());
  return true;
}

void
WebRenderCommandBuilder::RemoveUnusedAndResetWebRenderUserData()
{
  for (auto iter = mWebRenderUserDatas.Iter(); !iter.Done(); iter.Next()) {
    WebRenderUserData* data = iter.Get()->GetKey();
    if (!data->IsUsed()) {
      nsIFrame* frame = data->GetFrame();

      MOZ_ASSERT(frame->HasProperty(nsIFrame::WebRenderUserDataProperty()));

      nsIFrame::WebRenderUserDataTable* userDataTable =
        frame->GetProperty(nsIFrame::WebRenderUserDataProperty());

      MOZ_ASSERT(userDataTable->Count());

      userDataTable->Remove(data->GetDisplayItemKey());

      if (!userDataTable->Count()) {
        frame->RemoveProperty(nsIFrame::WebRenderUserDataProperty());
      }

      if (data->GetType() == WebRenderUserData::UserDataType::eCanvas) {
        mLastCanvasDatas.RemoveEntry(data->AsCanvasData());
      }

      iter.Remove();
      continue;
    }

    data->SetUsed(false);
  }
}

void
WebRenderCommandBuilder::ClearCachedResources()
{
  for (auto iter = mWebRenderUserDatas.Iter(); !iter.Done(); iter.Next()) {
    WebRenderUserData* data = iter.Get()->GetKey();
    data->ClearCachedResources();
  }
}



WebRenderGroupData::WebRenderGroupData(WebRenderLayerManager* aWRManager, nsDisplayItem* aItem)
  : WebRenderUserData(aWRManager, aItem)
{
}

WebRenderGroupData::~WebRenderGroupData()
{
}



} // namespace layers
} // namespace mozilla
