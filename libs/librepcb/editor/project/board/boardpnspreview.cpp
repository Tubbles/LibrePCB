/*
 * LibrePCB - Professional EDA for everyone!
 * Copyright (C) 2013 LibrePCB Developers, see AUTHORS.md for contributors.
 * https://librepcb.org/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include "boardpnspreview.h"

#include "../../graphics/graphicslayer.h"
#include "../../graphics/graphicslayerlist.h"
#include "../../graphics/primitivecirclegraphicsitem.h"
#include "../../graphics/primitivepathgraphicsitem.h"
#include "boardgraphicsscene.h"
#include "graphicsitems/bgi_netline.h"
#include "graphicsitems/bgi_pad.h"
#include "graphicsitems/bgi_stroketext.h"
#include "graphicsitems/bgi_via.h"

#include <librepcb/core/project/board/items/bi_device.h>
#include <librepcb/core/project/board/items/bi_pad.h>
#include <librepcb/core/types/point.h>
#include <librepcb/core/workspace/colorrole.h>

#include <QtCore>
#include <QtWidgets>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {

// BoardGraphicsScene::ZValue_AirWires is the highest z value the board scene
// hands out, so everything above it is above every board item.
static const qreal sZValueRatline = BoardGraphicsScene::ZValue_AirWires + 1;
static const qreal sZValueTrace = BoardGraphicsScene::ZValue_AirWires + 2;
static const qreal sZValueVia = BoardGraphicsScene::ZValue_AirWires + 3;

// How much a lightened style is lightened at least, the value BGI_Via uses
// for its net label.
static const int sLighterMinAlpha = 150;

// The opacity a board object the router asks to hide is drawn with, and the
// one it gets back.
static const qreal sHiddenOpacity = 0.0;
static const qreal sVisibleOpacity = 1.0;

// Set the opacity of the graphics items of some board objects. A board object
// which has no graphics item any more is skipped, which is what happens to an
// object a commit deleted.
template <typename TBoardItem, typename TGraphicsItem>
static void setOpacityOfBoardItems(
    const QSet<const TBoardItem*>& boardItems,
    const QHash<TBoardItem*, std::shared_ptr<TGraphicsItem>>& graphicsItems,
    qreal opacity) noexcept {
  foreach (const TBoardItem* boardItem, boardItems) {
    if (auto graphicsItem =
            graphicsItems.value(const_cast<TBoardItem*>(boardItem))) {
      graphicsItem->setOpacity(opacity);
    }
  }
}

/*******************************************************************************
 *  Constructors / Destructor
 ******************************************************************************/

BoardPnsPreviewItems::BoardPnsPreviewItems(
    BoardGraphicsScene& scene, const GraphicsLayerList& layers) noexcept
  : mScene(&scene),
    mLayers(layers),
    mViaLayer(layers.get(ColorRole::boardVias())),
    mAirWireLayer(layers.get(ColorRole::boardAirWires())),
    mPathItems(),
    mCircleItems(),
    mUsedPathItems(0),
    mUsedCircleItems(0),
    mHiddenNetLines(),
    mHiddenVias(),
    mHiddenPads(),
    mMovedItems() {
}

BoardPnsPreviewItems::~BoardPnsPreviewItems() noexcept {
  if (mScene) {
    clear();  // Let the board draw its own objects again.
    for (auto& item : mPathItems) {
      mScene->removeItem(*item);
    }
    for (auto& item : mCircleItems) {
      mScene->removeItem(*item);
    }
  } else {
    // The scene was destroyed first, which deleted every graphics item it
    // still held, so the pool must not delete them a second time.
    for (auto& item : mPathItems) {
      (void)item.release();
    }
    for (auto& item : mCircleItems) {
      (void)item.release();
    }
  }
}

/*******************************************************************************
 *  Getters
 ******************************************************************************/

int BoardPnsPreviewItems::getHiddenBoardItemCount() const noexcept {
  return mHiddenNetLines.count() + mHiddenVias.count() + mHiddenPads.count();
}

/*******************************************************************************
 *  General Methods
 ******************************************************************************/

void BoardPnsPreviewItems::update(const BoardPnsPreview& preview) noexcept {
  if (!mScene) {
    return;  // The scene is gone, there is nothing left to draw into.
  }

  mUsedPathItems = 0;
  mUsedCircleItems = 0;

  foreach (const BoardPnsPreviewItem& item, preview.items) {
    drawItem(item);
  }
  drawRatline(preview.ratline);
  // Both are empty unless a differential pair is being routed, in which
  // case the two lanes bring one rat line and one via each.
  drawRatline(preview.ratlineN);
  foreach (const BoardPnsPreviewVia& via, preview.fixedVias) {
    drawVia(via);
  }
  if (preview.via) {
    drawVia(*preview.via);
  }
  if (preview.viaN) {
    drawVia(*preview.viaN);
  }

  parkUnusedItems();
  applyMovedBoardItems(preview);
  applyHiddenBoardItems(preview);
}

void BoardPnsPreviewItems::clear() noexcept {
  mUsedPathItems = 0;
  mUsedCircleItems = 0;
  if (mScene) {
    parkUnusedItems();
  }
  applyMovedBoardItems(BoardPnsPreview());
  applyHiddenBoardItems(BoardPnsPreview());
}

/*******************************************************************************
 *  Private Methods
 ******************************************************************************/

PrimitivePathGraphicsItem& BoardPnsPreviewItems::takePathItem() noexcept {
  if (mUsedPathItems >= static_cast<int>(mPathItems.size())) {
    auto item = std::make_unique<PrimitivePathGraphicsItem>();
    item->setFlag(QGraphicsItem::ItemIsSelectable, false);
    item->setShapeMode(PrimitivePathGraphicsItem::ShapeMode::None);
    mScene->addItem(*item);
    mPathItems.push_back(std::move(item));
  }
  return *mPathItems.at(mUsedPathItems++);
}

PrimitiveCircleGraphicsItem& BoardPnsPreviewItems::takeCircleItem() noexcept {
  if (mUsedCircleItems >= static_cast<int>(mCircleItems.size())) {
    auto item = std::make_unique<PrimitiveCircleGraphicsItem>();
    item->setFlag(QGraphicsItem::ItemIsSelectable, false);
    item->setShapeMode(PrimitiveCircleGraphicsItem::ShapeMode::FilledOutline);
    mScene->addItem(*item);
    mCircleItems.push_back(std::move(item));
  }
  return *mCircleItems.at(mUsedCircleItems++);
}

void BoardPnsPreviewItems::drawItem(const BoardPnsPreviewItem& item) noexcept {
  const QPainterPath path = toPainterPath(item.path);
  const std::shared_ptr<const GraphicsLayer> layer =
      getLayerOfStyle(item.style, item.layer);
  if (path.isEmpty() || (!layer)) {
    return;  // Nothing to draw, so do not consume a pool item either.
  }

  // Note: No fill layer. ::librepcb::editor::BGI_NetLine strokes a line with
  // a round cap and no brush, so a trace drawn the same way looks like a
  // trace rather than like an outline.
  PrimitivePathGraphicsItem& graphicsItem = takePathItem();
  graphicsItem.setPath(path);
  graphicsItem.setLineWidth(positiveToUnsigned(item.width));
  graphicsItem.setLighterColorsWithMinAlpha(getMinAlphaOfStyle(item.style));
  graphicsItem.setLineLayer(layer);
  graphicsItem.setZValue(sZValueTrace);
}

void BoardPnsPreviewItems::drawVia(const BoardPnsPreviewVia& via) noexcept {
  std::shared_ptr<const GraphicsLayer> layer = mViaLayer;
  if (via.style == BoardPnsPreviewStyle::Collision) {
    layer = mAirWireLayer;
  }
  if (!layer) {
    return;
  }

  PrimitiveCircleGraphicsItem& graphicsItem = takeCircleItem();
  graphicsItem.setPosition(via.position);
  graphicsItem.setDiameter(positiveToUnsigned(via.diameter));
  graphicsItem.setLineLayer(layer);
  graphicsItem.setFillLayer(layer);
  graphicsItem.setZValue(sZValueVia);
}

void BoardPnsPreviewItems::drawRatline(const QVector<Point>& ratline) noexcept {
  const QPainterPath path = toPainterPath(ratline);
  if (path.isEmpty() || (!mAirWireLayer)) {
    return;
  }

  // A zero line width is a cosmetic pen, which is what
  // ::librepcb::editor::BGI_AirWire draws with: the rat line stays one pixel
  // wide at every zoom level and cannot be mistaken for a trace.
  PrimitivePathGraphicsItem& graphicsItem = takePathItem();
  graphicsItem.setPath(path);
  graphicsItem.setLineWidth(UnsignedLength(0));
  graphicsItem.setLighterColorsWithMinAlpha(0);
  graphicsItem.setLineLayer(mAirWireLayer);
  graphicsItem.setZValue(sZValueRatline);
}

void BoardPnsPreviewItems::parkUnusedItems() noexcept {
  // Note: Clearing the layers rather than just hiding the item detaches it
  // from the layer signals, so a layer visibility change cannot bring a
  // parked item back.
  for (std::size_t i = static_cast<std::size_t>(mUsedPathItems);
       i < mPathItems.size(); ++i) {
    mPathItems.at(i)->setLineLayer(nullptr);
    mPathItems.at(i)->setFillLayer(nullptr);
    mPathItems.at(i)->setPath(QPainterPath());
  }
  for (std::size_t i = static_cast<std::size_t>(mUsedCircleItems);
       i < mCircleItems.size(); ++i) {
    mCircleItems.at(i)->setLineLayer(nullptr);
    mCircleItems.at(i)->setFillLayer(nullptr);
  }
}

void BoardPnsPreviewItems::applyHiddenBoardItems(
    const BoardPnsPreview& preview) noexcept {
  // The pads of a footprint drag are hidden and moved at once by the
  // router, which has no geometry to draw them with; here they are drawn
  // at the offset by applyMovedBoardItems() instead, so hiding them too
  // would take the copper of the dragged footprint off the screen.
  QSet<const BI_Device*> movedDevices;
  foreach (const BoardPnsMovedDevice& moved, preview.movedDevices) {
    movedDevices.insert(moved.device);
  }

  QSet<const BI_NetLine*> netLines;
  QSet<const BI_Via*> vias;
  QSet<const BI_Pad*> pads;
  auto collect = [&netLines, &vias, &pads,
                  &movedDevices](const BoardPnsHostRef& ref) {
    if (ref.netLine) netLines.insert(ref.netLine);
    if (ref.via) vias.insert(ref.via);
    // The reference comes from the frame which is being drawn, so the pad
    // is a live board object and asking it for its device is safe.
    if (ref.pad && (!movedDevices.contains(ref.pad->getDevice()))) {
      pads.insert(ref.pad);
    }
  };
  foreach (const BoardPnsHostRef& ref, preview.hidden) {
    collect(ref);
  }
  foreach (const BoardPnsViolation& violation, preview.violations) {
    // A violation which asks for the original to be hidden is exactly the
    // hidden case, the collision styled item takes its place.
    if (violation.hideOriginal) {
      collect(violation.host);
    }
  }

  if (mScene) {
    // Note: The opacity is set instead of the visibility because the `BGI_*`
    // items own their own visibility, which follows their layer, and would
    // fight a restore.
    setOpacityOfBoardItems(mHiddenNetLines - netLines, mScene->getNetLines(),
                           sVisibleOpacity);
    setOpacityOfBoardItems(mHiddenVias - vias, mScene->getVias(),
                           sVisibleOpacity);
    setOpacityOfBoardItems(mHiddenPads - pads, mScene->getPads(),
                           sVisibleOpacity);
    setOpacityOfBoardItems(netLines, mScene->getNetLines(), sHiddenOpacity);
    setOpacityOfBoardItems(vias, mScene->getVias(), sHiddenOpacity);
    setOpacityOfBoardItems(pads, mScene->getPads(), sHiddenOpacity);
  }
  mHiddenNetLines = netLines;
  mHiddenVias = vias;
  mHiddenPads = pads;
}

void BoardPnsPreviewItems::applyMovedBoardItems(
    const BoardPnsPreview& preview) noexcept {
  // Put back what the last frame moved, then move what this one asks for.
  // Restoring everything first keeps the bookkeeping to one list: a frame
  // is a whole replacement, so there is nothing to carry over.
  for (const auto& moved : mMovedItems) {
    moved.first->setPos(moved.second);
  }
  mMovedItems.clear();
  if (!mScene) {
    return;  // The scene is gone, so there is nothing left to move.
  }

  auto move = [this](std::shared_ptr<QGraphicsItem> item,
                     const QPointF& offset) {
    if (!item) return;
    mMovedItems.push_back(std::make_pair(item, item->pos()));
    item->setPos(item->pos() + offset);
  };
  foreach (const BoardPnsMovedDevice& moved, preview.movedDevices) {
    if (!moved.device) continue;
    // Note: The pads and the texts of a device are graphics items of their
    // own rather than children of its item, so each one has to follow the
    // device by itself.
    const QPointF offset = moved.offset.toPxQPointF();
    move(mScene->getDevices().value(moved.device), offset);
    foreach (BI_Pad* pad, moved.device->getPads()) {
      move(mScene->getPads().value(pad), offset);
    }
    foreach (BI_StrokeText* text, moved.device->getStrokeTexts()) {
      move(mScene->getStrokeTexts().value(text), offset);
    }
  }
}

std::shared_ptr<const GraphicsLayer> BoardPnsPreviewItems::getLayerOfStyle(
    BoardPnsPreviewStyle style, const Layer* layer) const noexcept {
  if (style == BoardPnsPreviewStyle::Collision) {
    return mAirWireLayer;
  }
  return layer ? mLayers.get(*layer) : nullptr;
}

int BoardPnsPreviewItems::getMinAlphaOfStyle(
    BoardPnsPreviewStyle style) noexcept {
  switch (style) {
    case BoardPnsPreviewStyle::Tail:
    case BoardPnsPreviewStyle::SemiSolid:
      return sLighterMinAlpha;
    default:
      // Head, Collision, and Hover which the router never emits.
      return 0;
  }
}

QPainterPath BoardPnsPreviewItems::toPainterPath(
    const QVector<Point>& points) noexcept {
  QPainterPath path;
  if (points.count() < 2) {
    return path;  // A single point has no stroke.
  }
  path.moveTo(points.first().toPxQPointF());
  for (int i = 1; i < points.count(); ++i) {
    path.lineTo(points.at(i).toPxQPointF());
  }
  return path;
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb
