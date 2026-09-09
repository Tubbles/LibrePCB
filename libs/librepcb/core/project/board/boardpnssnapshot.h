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

#ifndef LIBREPCB_CORE_BOARDPNSSNAPSHOT_H
#define LIBREPCB_CORE_BOARDPNSSNAPSHOT_H

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include "../../types/length.h"
#include "../../utils/rusthandle.h"

#include <QtCore>

/*******************************************************************************
 *  Namespace / Forward Declarations
 ******************************************************************************/
namespace librepcb {

class BI_Hole;
class BI_NetLine;
class BI_Pad;
class BI_Polygon;
class BI_Via;
class Board;
class Layer;
class NetClass;
class NetSignal;
class Path;

namespace rs {
struct PnsSnapshot;
}

/*******************************************************************************
 *  Struct BoardPnsHostRef
 ******************************************************************************/

/**
 * @brief The board object one router item came from
 *
 * Exactly one of the pointers is set. The router speaks in host IDs, which
 * are indices into ::librepcb::BoardPnsSnapshot::getHostRefs(), so this is
 * how a router answer is mapped back onto the board.
 */
struct BoardPnsHostRef final {
  const BI_NetLine* netLine = nullptr;
  const BI_Via* via = nullptr;
  const BI_Pad* pad = nullptr;
  const BI_Hole* hole = nullptr;
  const BI_Polygon* polygon = nullptr;
};

/*******************************************************************************
 *  Class BoardPnsSnapshot
 ******************************************************************************/

/**
 * @brief Everything the push and shove router needs to know about a board
 *
 * Walks a ::librepcb::Board once and pushes it into the Rust side of the
 * router FFI. The result is a snapshot handle plus the table that maps the
 * host IDs the router speaks back onto board objects.
 *
 * The snapshot is a plain copy of the board, in the same spirit as
 * ::librepcb::BoardDesignRuleCheckData, so the router never reads the board
 * again while it is routing.
 *
 * What is synced: traces, vias, pads (one solid per copper layer), board
 * holes, copper polygons and the board outline. What is deliberately not
 * synced: planes, air wires, zones, stroke texts and everything on a non
 * copper layer. The router routes through a plane and the caller rebuilds
 * it afterwards.
 *
 * @note This is just a wrapper around its Rust implementation.
 */
class BoardPnsSnapshot final {
public:
  // Constructors / Destructor
  BoardPnsSnapshot() = delete;
  BoardPnsSnapshot(const BoardPnsSnapshot& other) = delete;

  /**
   * @brief Build the snapshot of a board
   *
   * @param board   The board to copy. Not referenced after the constructor
   *                returns.
   *
   * @throws ::librepcb::RuntimeError if the board carries a coordinate
   *         outside the range the router supports, which is about plus or
   *         minus 2 metres.
   */
  explicit BoardPnsSnapshot(const Board& board);
  ~BoardPnsSnapshot() noexcept;

  // Getters

  /**
   * @brief Get the number of copper layers of the board
   */
  int getCopperLayerCount() const noexcept { return mCopperLayerCount; }

  /**
   * @brief Get the board object of each host ID
   *
   * Index 0 is an all null entry, because host ID 0 is reserved as a null
   * value in the FFI structs.
   */
  const QVector<BoardPnsHostRef>& getHostRefs() const noexcept {
    return mHostRefs;
  }

  /**
   * @brief Get the host ID a board object was given, or 0 if it has none
   */
  quint64 getHostId(const BI_NetLine& netLine) const noexcept;
  quint64 getHostId(const BI_Via& via) const noexcept;
  quint64 getHostId(const BI_Pad& pad) const noexcept;
  quint64 getHostId(const BI_Hole& hole) const noexcept;

  /**
   * @brief Get the FFI net number of a net signal, or 0 for "no net"
   */
  quint32 getNetNumber(const NetSignal* net) const noexcept;

  /**
   * @brief Get the net signal of an FFI net number
   *
   * The inverse of #getNetNumber(), which the routing session needs to turn
   * the nets the router answers with back into circuit objects.
   *
   * @return The net signal, or `nullptr` for net number 0 and for a number
   *         this snapshot never handed out. The router's internal orphan
   *         net, which a route placed in free space gets, is one of the
   *         latter.
   */
  const NetSignal* getNetSignal(quint32 netNumber) const noexcept;

  // General Methods

  /**
   * @brief Access the Rust snapshot object
   *
   * Follows ::librepcb::RustHandle and yields a pointer, which is what the
   * FFI functions take.
   */
  rs::PnsSnapshot* operator*() noexcept { return *mHandle; }

  /**
   * @brief Hand the Rust snapshot object over to a routing session
   *
   * The routing session consumes the snapshot, so this object must not
   * delete it any more. Afterwards this object is empty and only the host
   * ID table is still usable.
   *
   * @return The snapshot object, which the caller now owns.
   */
  rs::PnsSnapshot* release() noexcept;

  // Operator Overloadings
  BoardPnsSnapshot& operator=(const BoardPnsSnapshot& rhs) = delete;

  // Static Methods

  /**
   * @brief Map a board layer to the dense copper layer index of the router
   *
   * ::librepcb::Layer::getCopperNumber() is not dense: the bottom layer
   * always reports ::librepcb::Layer::innerCopperCount() + 1 no matter how
   * many inner layers the board actually has. The router wants
   * `0 .. copperLayerCount - 1`, so the bottom layer is moved down.
   *
   * @param layer             Any board layer.
   * @param innerLayerCount   ::librepcb::Board::getInnerLayerCount().
   *
   * @return The dense index, or -1 if the layer is not a copper layer of
   *         such a board.
   */
  static int toDenseLayerIndex(const Layer& layer,
                               int innerLayerCount) noexcept;

  /**
   * @brief Map a dense copper layer index back to a board layer
   *
   * The inverse of #toDenseLayerIndex().
   *
   * @param index             A dense copper layer index.
   * @param innerLayerCount   ::librepcb::Board::getInnerLayerCount().
   *
   * @return The layer, or `nullptr` if the index is out of range.
   */
  static const Layer* fromDenseLayerIndex(int index,
                                          int innerLayerCount) noexcept;

private:  // Methods
  void addRules(const Board& board);
  void addNets(const Board& board);
  void addTracesAndVias(const Board& board);
  void addPads(const Board& board);
  void addHoles(const Board& board);
  void addPolygons(const Board& board);

  /**
   * @brief Add one pad as one solid per copper layer
   *
   * The pad's drill rides on exactly one of those solids, which is then
   * widened to the whole copper stack. Putting the drill on every layer's
   * solid instead would make the pad collide with itself under the drill to
   * drill clearance rule.
   */
  void addPad(const BI_Pad& pad, int innerLayerCount);

  /**
   * @brief Add one board outline or cutout, edge by edge
   *
   * A board edge keeps copper away from itself but is not an area: the
   * router's polygon shape is filled, so emitting the outline as one polygon
   * would make the whole board interior an obstacle and no route could ever
   * start. Each edge becomes its own zero width obstacle instead, which is
   * how KiCad syncs its own board edges.
   */
  void addBoardEdge(const BI_Polygon& polygon, quint64 id);

  /**
   * @brief Reserve the next host ID for a board object
   */
  quint64 addHostRef(const BoardPnsHostRef& ref) noexcept;

  /**
   * @brief Turn an FFI result code into an exception
   */
  static void check(int result, const QString& item);

private:  // Data
  int mInnerLayerCount;
  int mCopperLayerCount;
  RustHandle<rs::PnsSnapshot> mHandle;
  QVector<BoardPnsHostRef> mHostRefs;
  QHash<const void*, quint64> mHostIds;
  QHash<const NetSignal*, quint32> mNetNumbers;
  QVector<const NetSignal*> mNetSignals;
};

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace librepcb

#endif
