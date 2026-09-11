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

#ifndef LIBREPCB_EDITOR_BOARDPNSPREVIEW_H
#define LIBREPCB_EDITOR_BOARDPNSPREVIEW_H

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include <librepcb/core/project/board/boardpnsrouter.h>

#include <QtCore>
#include <QtWidgets>

#include <memory>
#include <utility>
#include <vector>

/*******************************************************************************
 *  Namespace / Forward Declarations
 ******************************************************************************/
namespace librepcb {

class BI_Device;
class BI_NetLine;
class BI_Pad;
class BI_Via;
class Layer;

namespace editor {

class BoardGraphicsScene;
class GraphicsLayer;
class GraphicsLayerList;
class PrimitiveCircleGraphicsItem;
class PrimitivePathGraphicsItem;

/*******************************************************************************
 *  Class BoardPnsPreviewItems
 ******************************************************************************/

/**
 * @brief Draws a ::librepcb::BoardPnsPreview into a board graphics scene
 *
 * The router hands its host a whole frame on every event, so #update() makes
 * the scene show exactly that frame and nothing else. Mouse moves produce one
 * frame each, so the graphics items are pooled by index and only their
 * geometry is reset; a surplus item is parked instead of deleted.
 *
 * The class draws with ::librepcb::editor::PrimitivePathGraphicsItem and
 * ::librepcb::editor::PrimitiveCircleGraphicsItem rather than with the
 * `BGI_*` items, because each of those is bound to a board item by reference
 * while the route being placed has no board item until it is committed.
 *
 * A frame also names the board objects the host has to stop drawing, which is
 * how a shoved trace is shown at its new place rather than at both. Those are
 * made transparent for the lifetime of the frame and restored by the first
 * frame which does not name them, by #clear() and by the destructor.
 *
 * A frame of a footprint drag names the devices it is moving instead of
 * drawing their copper itself, because the router has no geometry for a pad
 * and the host already owns it. The device, its pads and its texts are drawn
 * at the offset for the lifetime of the frame and put back the same way the
 * hidden objects are, so the footprint follows the cursor while the traces
 * hanging off its pads are drawn as any other dragged trace is.
 *
 * @note The scene owns every ::QGraphicsItem added to it and deletes what is
 *       left in it. Board2dTab destroys the scene before the tool state which
 *       owns this class leaves, so the destructor has to cope with a scene
 *       which is already gone.
 */
class BoardPnsPreviewItems final {
public:
  // Constructors / Destructor
  BoardPnsPreviewItems() = delete;
  BoardPnsPreviewItems(const BoardPnsPreviewItems& other) = delete;

  /**
   * @brief Start drawing previews into a scene
   *
   * @param scene   The scene to add the graphics items to. May be destroyed
   *                before this object is.
   * @param layers  The layer list to take the colours from. Must outlive
   *                this object.
   */
  BoardPnsPreviewItems(BoardGraphicsScene& scene,
                       const GraphicsLayerList& layers) noexcept;
  ~BoardPnsPreviewItems() noexcept;

  // Getters

  /**
   * @brief Get how many path items were allocated so far
   *
   * The pool never shrinks, so this is the largest frame seen so far, not
   * what the last frame drew.
   */
  int getPathItemPoolSize() const noexcept {
    return static_cast<int>(mPathItems.size());
  }

  /**
   * @brief Get how many circle items were allocated so far
   */
  int getCircleItemPoolSize() const noexcept {
    return static_cast<int>(mCircleItems.size());
  }

  /**
   * @brief Get how many path items the last frame used
   */
  int getUsedPathItemCount() const noexcept { return mUsedPathItems; }

  /**
   * @brief Get how many circle items the last frame used
   */
  int getUsedCircleItemCount() const noexcept { return mUsedCircleItems; }

  /**
   * @brief Get how many board objects are hidden right now
   */
  int getHiddenBoardItemCount() const noexcept;

  /**
   * @brief Get how many graphics items are drawn at an offset right now
   */
  int getMovedBoardItemCount() const noexcept {
    return static_cast<int>(mMovedItems.size());
  }

  // General Methods

  /**
   * @brief Show exactly one frame, replacing whatever was shown before
   */
  void update(const BoardPnsPreview& preview) noexcept;

  /**
   * @brief Show nothing and let the board draw all of its own objects again
   *
   * Must be called before the board is edited, because the hidden objects are
   * tracked by pointer and a commit can delete them.
   */
  void clear() noexcept;

  // Operator Overloadings
  BoardPnsPreviewItems& operator=(const BoardPnsPreviewItems& rhs) = delete;

private:  // Methods
  /**
   * @brief Get the next path item of the pool, allocating one if needed
   */
  PrimitivePathGraphicsItem& takePathItem() noexcept;

  /**
   * @brief Get the next circle item of the pool, allocating one if needed
   */
  PrimitiveCircleGraphicsItem& takeCircleItem() noexcept;

  /**
   * @brief Draw one polyline of a frame
   */
  void drawItem(const BoardPnsPreviewItem& item) noexcept;

  /**
   * @brief Draw one via of a frame
   */
  void drawVia(const BoardPnsPreviewVia& via) noexcept;

  /**
   * @brief Draw the rat line of a frame
   */
  void drawRatline(const QVector<Point>& ratline) noexcept;

  /**
   * @brief Park every pool item the current frame did not use
   */
  void parkUnusedItems() noexcept;

  /**
   * @brief Hide what the frame asks for and restore everything else
   */
  void applyHiddenBoardItems(const BoardPnsPreview& preview) noexcept;

  /**
   * @brief Draw the devices a footprint drag moves at their offset
   *
   * Puts everything the last frame moved back first, so a frame which
   * moves nothing restores the board's own layout.
   */
  void applyMovedBoardItems(const BoardPnsPreview& preview) noexcept;

  /**
   * @brief Get the graphics layer one preview style is drawn on
   *
   * @param style   The style the router asked for.
   * @param layer   The copper layer of the item, may be `nullptr`.
   *
   * @return The layer, or `nullptr` if the item cannot be drawn.
   */
  std::shared_ptr<const GraphicsLayer> getLayerOfStyle(
      BoardPnsPreviewStyle style, const Layer* layer) const noexcept;

  /**
   * @brief Get the minimum alpha one preview style is lightened to
   *
   * @return 0 to draw in the layer's own colour.
   */
  static int getMinAlphaOfStyle(BoardPnsPreviewStyle style) noexcept;

  /**
   * @brief Build the painter path of a polyline
   *
   * @return An empty path for a polyline of less than two points, which
   *         cannot be stroked.
   */
  static QPainterPath toPainterPath(const QVector<Point>& points) noexcept;

private:  // Data
  /// The scene the items live in. Cleared by Qt if the scene is destroyed
  /// first, in which case the scene has already deleted the pooled items.
  QPointer<BoardGraphicsScene> mScene;

  /// The copper layers of the traces are looked up here on every frame,
  /// because a frame may switch layers.
  const GraphicsLayerList& mLayers;

  /// The layer the vias are drawn on, the one ::librepcb::editor::BGI_Via
  /// uses.
  std::shared_ptr<const GraphicsLayer> mViaLayer;

  /// The layer the rat line and every collision are drawn on, the one
  /// ::librepcb::editor::BGI_AirWire uses.
  std::shared_ptr<const GraphicsLayer> mAirWireLayer;

  std::vector<std::unique_ptr<PrimitivePathGraphicsItem>> mPathItems;
  std::vector<std::unique_ptr<PrimitiveCircleGraphicsItem>> mCircleItems;
  int mUsedPathItems;  ///< how many path items the last frame used
  int mUsedCircleItems;  ///< how many circle items the last frame used

  /// The board objects which are transparent right now. Held by pointer and
  /// never dereferenced, so a deleted object is looked up and not found
  /// rather than followed.
  QSet<const BI_NetLine*> mHiddenNetLines;
  QSet<const BI_Via*> mHiddenVias;
  QSet<const BI_Pad*> mHiddenPads;

  /// The graphics items which are drawn somewhere else right now, each
  /// with the position it had before. Held as shared pointers rather than
  /// as board objects, because a device, its pads and its texts are three
  /// unrelated types with one thing in common, and because an item the
  /// board dropped in the meantime is still there to be put back.
  std::vector<std::pair<std::shared_ptr<QGraphicsItem>, QPointF>> mMovedItems;
};

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb

#endif
