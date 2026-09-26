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

#ifndef LIBREPCB_EDITOR_CMDBOARDAPPLYPNSCOMMIT_H
#define LIBREPCB_EDITOR_CMDBOARDAPPLYPNSCOMMIT_H

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include "../../undocommandgroup.h"

#include <librepcb/core/project/board/boardpnsrouter.h>
#include <librepcb/core/types/length.h>
#include <librepcb/core/types/point.h>

#include <QtCore>

/*******************************************************************************
 *  Namespace / Forward Declarations
 ******************************************************************************/
namespace librepcb {

class BI_Device;
class BI_NetLine;
class BI_NetLineAnchor;
class BI_NetPoint;
class BI_NetSegment;
class BI_Via;
class Board;
class Layer;
class NetSignal;

namespace editor {

class CmdBoardNetSegmentAddElements;

/*******************************************************************************
 *  Class CmdBoardApplyPnsCommit
 ******************************************************************************/

/**
 * @brief Undo command to apply a ::librepcb::BoardPnsCommit to a board
 *
 * The push and shove router speaks geometry: its commit is a list of free
 * floating segments and vias with no notion of a junction, plus the board
 * objects it superseded. This command turns that back into the topology
 * LibrePCB requires, which is a ::librepcb::BI_NetSegment holding
 * ::librepcb::BI_NetLine objects between ::librepcb::BI_NetLineAnchor
 * objects. One whole route is one undo entry.
 *
 * Everything happens in #performExecute(), in one pass, with each child
 * command executed as it is appended. That is essential rather than
 * convenient: an endpoint is resolved against the board as it is at that
 * moment, so a later endpoint sees the net points, vias and split points
 * the earlier ones added. Resolving the endpoints up front and only then
 * building the commands is the classic way to get this wrong, because the
 * lookups would all run against the unmodified board.
 *
 * The work is done in this order:
 *   -# The devices a footprint drag moved are moved, because their pads are
 *      net line anchors and everything below is anchored against the board
 *      as it is at that moment.
 *   -# A via the router only moved keeps its identity and is moved in place,
 *      which matters because its UUID is part of the file format.
 *   -# Everything else the router removed or replaced is removed, through
 *      ::librepcb::editor::CmdRemoveBoardItems, which re-splits whatever is
 *      left of the affected net segments.
 *   -# Every added segment is anchored and added.
 *   -# The touched net segments are simplified, so that corners the router's
 *      optimiser removed do not survive as dangling net points.
 *
 * An endpoint is anchored, in this order of preference, on an existing pad,
 * via or net point of the same net at exactly that position on that layer;
 * on the interior of an existing net line of the same net, which is then
 * split; on a via this commit adds; or on a new net point.
 */
class CmdBoardApplyPnsCommit final : public UndoCommandGroup {
public:
  // Constructors / Destructor
  CmdBoardApplyPnsCommit() = delete;
  CmdBoardApplyPnsCommit(const CmdBoardApplyPnsCommit& other) = delete;

  /**
   * @brief Constructor
   *
   * @param board   The board the commit was routed on.
   * @param commit  What the routing session changed. Copied, so the session
   *                may be destroyed before this command is executed.
   */
  CmdBoardApplyPnsCommit(Board& board, const BoardPnsCommit& commit) noexcept;
  ~CmdBoardApplyPnsCommit() noexcept override;

  // Operator Overloadings
  CmdBoardApplyPnsCommit& operator=(const CmdBoardApplyPnsCommit& rhs) = delete;

private:  // Types
  /**
   * @brief An anchor found on the board, with the net segment owning it
   */
  struct FoundAnchor final {
    /// The anchor, or `nullptr` if there is nothing at that position.
    BI_NetLineAnchor* anchor = nullptr;

    /// The net segment the anchor belongs to. Null for a footprint pad which
    /// has no trace connected yet, which is exactly the pad a route starts on.
    BI_NetSegment* segment = nullptr;
  };

  /**
   * @brief A via the commit adds which is not on the board yet
   *
   * A via is only put on the board together with the first net line that
   * ends on it, because a net segment must stay cohesive after every single
   * command and a via added on its own would not be connected to anything.
   */
  struct PendingVia final {
    Point position;
    PositiveLength diameter;
    PositiveLength drill;
    const Layer* startLayer = nullptr;
    const Layer* endLayer = nullptr;
    NetSignal* net = nullptr;
    bool placed = false;
  };

private:  // Methods
  /// @copydoc ::librepcb::editor::UndoCommand::performExecute()
  bool performExecute() override;

  /**
   * @brief Refuse a commit which carries a curved trace
   *
   * LibrePCB has no arc trace: a ::librepcb::Trace serialises a layer, a
   * width and two anchors and no angle
   * (`libs/librepcb/core/geometry/trace.cpp:236`). The router can produce one,
   * in the two rounded corner modes and with round meander corners, but
   * neither is offered at the settings boundary, so an arc reaching here means
   * that boundary has a hole in it. It is thrown rather than dropped, because
   * dropping it would leave a gap in the copper the user cannot see, and
   * rather than flattened, because a flattened arc is a different track which
   * the next session would read back as a polyline.
   *
   * @throws ::librepcb::LogicError if the commit carries an arc
   */
  void checkNoArcs() const;

  /**
   * @brief Get the new geometry of an updated via which only moved
   *
   * @return The via geometry to move the board object to, or `nullptr` if
   *         the update is not a plain move and has to become a removal plus
   *         an addition instead.
   */
  static const BoardPnsNewVia* getViaMove(const BoardPnsHostRef& ref,
                                          const BoardPnsNewItem& item) noexcept;

  /**
   * @brief Collect one removed host object into the sets to remove
   */
  static void collectRemoval(const BoardPnsHostRef& ref,
                             QSet<BI_NetLine*>& netLines,
                             QSet<BI_Via*>& vias) noexcept;

  /**
   * @brief Turn one added segment into a net line
   */
  void addSegment(const BoardPnsNewSegment& item, NetSignal* net);

  /**
   * @brief Put a via nothing ended on into a net segment of its own
   */
  void addOrphanVia(PendingVia& via);

  /**
   * @brief Find or create the anchor for one endpoint
   *
   * Splits an existing net line if the point lies in its interior, which
   * changes the board, so this must only be called when the anchor is
   * actually going to be used.
   */
  FoundAnchor resolveAnchor(const Point& pos, const Layer& layer,
                            const NetSignal* net);

  /**
   * @brief Get the anchor to hand to the net line, creating one if needed
   *
   * @param pos         The endpoint position.
   * @param found       What #resolveAnchor() found there.
   * @param segment     The net segment the net line goes into.
   * @param cmd         The command the new elements are added to.
   * @param temporary   Set to the net point which was created as a stand in
   *                    for an anchor living in another net segment.
   */
  BI_NetLineAnchor* materializeAnchor(const Point& pos,
                                      const FoundAnchor& found,
                                      BI_NetSegment& segment,
                                      CmdBoardNetSegmentAddElements& cmd,
                                      BI_NetPoint*& temporary);

  /**
   * @brief Find an existing anchor at a position
   */
  FoundAnchor findAnchorAt(const Point& pos, const Layer& layer,
                           const NetSignal* net) const noexcept;

  /**
   * @brief Find an existing net line a position lies in the interior of
   */
  BI_NetLine* findNetLineAt(const Point& pos, const Layer& layer,
                            const NetSignal* net) const noexcept;

  /**
   * @brief Memorize a net segment for the final simplification
   */
  void rememberSegment(BI_NetSegment* segment) noexcept;

private:  // Data
  Board& mBoard;
  BoardPnsCommit mCommit;
  QVector<PendingVia> mPendingVias;
  QList<BI_NetSegment*> mTouchedSegments;
};

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb

#endif
