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

#ifndef LIBREPCB_CORE_BOARDPNSROUTER_H
#define LIBREPCB_CORE_BOARDPNSROUTER_H

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include "../../types/length.h"
#include "../../types/point.h"
#include "../../utils/rusthandle.h"
#include "boardpnssnapshot.h"

#include <QtCore>

#include <memory>
#include <optional>
#include <variant>

/*******************************************************************************
 *  Namespace / Forward Declarations
 ******************************************************************************/
namespace librepcb {

class BI_Device;
class Board;
class Layer;
class NetSignal;

namespace rs {
struct PnsPreviewVia;
struct PnsRouter;
struct PnsNewItem;
}  // namespace rs

/*******************************************************************************
 *  Preview Types
 ******************************************************************************/

/**
 * @brief How one element of a ::librepcb::BoardPnsPreview should be drawn
 */
enum class BoardPnsPreviewStyle {
  Head,  ///< The trace being placed right now.
  Tail,  ///< Geometry the session has already fixed.
  Hover,  ///< The item under the cursor. Never set by the router itself.
  SemiSolid,  ///< One primitive of a keepout zone. Not emitted yet.
  Collision,  ///< Something a rule violation was found on.
};

/**
 * @brief One polyline of a ::librepcb::BoardPnsPreview
 */
struct BoardPnsPreviewItem final {
  QVector<Point> path;
  PositiveLength width = PositiveLength(Length(1));
  const Layer* layer = nullptr;
  const NetSignal* net = nullptr;
  BoardPnsPreviewStyle style = BoardPnsPreviewStyle::Head;

  /// The clearance outline to draw around the polyline, if a rule applies.
  std::optional<Length> clearance;
};

/**
 * @brief One via of a ::librepcb::BoardPnsPreview
 */
struct BoardPnsPreviewVia final {
  Point position;
  PositiveLength diameter = PositiveLength(Length(1));
  PositiveLength drill = PositiveLength(Length(1));
  const Layer* startLayer = nullptr;
  const Layer* endLayer = nullptr;
  const NetSignal* net = nullptr;
  BoardPnsPreviewStyle style = BoardPnsPreviewStyle::Head;

  /// The clearance outline to draw around the via, if a rule applies.
  std::optional<Length> clearance;
};

/**
 * @brief One obstacle the route being placed runs into
 */
struct BoardPnsViolation final {
  /// The board object the route collides with.
  BoardPnsHostRef host;

  /// The clearance that was asked for and not met.
  Length clearance;

  /// The layer to draw the obstacle on instead of its own, if the router
  /// wants the violation shown where the user is working.
  const Layer* forcedLayer = nullptr;

  /// Whether the obstacle's normal rendering should be hidden while the
  /// violation is drawn.
  bool hideOriginal = false;
};

/**
 * @brief One device a footprint drag moves, and how far it moves
 *
 * The router names the pads it moved, one entry per pad, and every pad of
 * one device carries the same offset; this is that list folded onto the
 * board objects which actually move, which is what the host has to edit.
 * The device is not const for the same reason
 * ::librepcb::BoardPnsHostRef's pointers are not: the whole point of the
 * answer is that ::librepcb::editor::CmdBoardApplyPnsCommit moves it.
 */
struct BoardPnsMovedDevice final {
  BI_Device* device = nullptr;

  /// How far the device has to move, which is a translation only: the
  /// router never rotates or mirrors anything.
  Point offset;
};

/**
 * @brief Everything a host has to draw after one router event
 *
 * A whole frame replacement: the host clears what it drew last time and
 * rebuilds it from this, which is what the router's own semantics are.
 */
struct BoardPnsPreview final {
  QVector<BoardPnsPreviewItem> items;

  /// The via the next fix would place, if one is armed.
  std::optional<BoardPnsPreviewVia> via;

  /// The vias the session has already fixed.
  QVector<BoardPnsPreviewVia> fixedVias;

  /// The rat line from the end of the route to what it still has to reach.
  QVector<Point> ratline;

  QVector<BoardPnsViolation> violations;

  /// Board objects the host must stop drawing while the session runs.
  QVector<BoardPnsHostRef> hidden;

  /// The devices a footprint drag is moving, and by how much. Their pads
  /// are in #hidden too, so a host which cannot draw them at the offset
  /// at least stops drawing them where they are.
  QVector<BoardPnsMovedDevice> movedDevices;
};

/*******************************************************************************
 *  Commit Types
 ******************************************************************************/

/**
 * @brief The straight trace half of a ::librepcb::BoardPnsNewItem
 */
struct BoardPnsNewSegment final {
  Point start;
  Point end;
  PositiveLength width = PositiveLength(Length(1));
  const Layer* layer = nullptr;
};

/**
 * @brief The via half of a ::librepcb::BoardPnsNewItem
 */
struct BoardPnsNewVia final {
  Point position;
  PositiveLength diameter = PositiveLength(Length(1));
  PositiveLength drill = PositiveLength(Length(1));
  const Layer* startLayer = nullptr;
  const Layer* endLayer = nullptr;
};

/**
 * @brief The curved trace half of a ::librepcb::BoardPnsNewItem
 *
 * LibrePCB cannot store one: a ::librepcb::Trace serialises a layer, a width
 * and two anchors and no angle (`libs/librepcb/core/geometry/trace.cpp:236`),
 * and the board file format is stable. The struct exists so that an arc the
 * engine sent arrives as an arc and is refused by
 * ::librepcb::editor::CmdBoardApplyPnsCommit, instead of arriving as the
 * straight trace across its chord. Nothing a LibrePCB session can do produces
 * one, for the four reasons the `PnsNewGeometryKind` documentation in
 * `libs/librepcb/rust-core/src/ffi/router_ffi.rs` lists.
 *
 * The three points are KiCad's own arc form, which the engine uses
 * throughout: the two ends plus any point of the curve strictly between them,
 * whose side of the chord says which way round the arc runs. There is no
 * centre and no angle, because neither survives the conversion exactly.
 */
struct BoardPnsNewArc final {
  Point start;
  Point mid;
  Point end;
  PositiveLength width = PositiveLength(Length(1));
  const Layer* layer = nullptr;
};

/**
 * @brief One trace, arc or via a routing session produced
 *
 * A single track placer emits nothing else. The geometry is a variant rather
 * than all three structs plus a discriminator, so that a via cannot carry a
 * trace width and a trace cannot carry a drill diameter. Read it with
 * #getSegment(), #getVia() and #getArc(), each of which answers `nullptr` for
 * the other kinds.
 */
struct BoardPnsNewItem final {
  /// The net, or `nullptr` for no net. A route placed in free space has
  /// none: the router puts it on an internal orphan net which has no
  /// counterpart in the circuit. Not const, unlike the net of a preview
  /// item, because a commit is applied to the board rather than drawn.
  NetSignal* net = nullptr;

  /// The board object this item descends from, all null for a freshly
  /// routed one. Set for the halves an existing trace was split into.
  BoardPnsHostRef source;

  /// What was placed.
  std::variant<BoardPnsNewSegment, BoardPnsNewVia, BoardPnsNewArc> geometry;

  /// @brief Get the straight trace, or `nullptr` if this item is not one
  const BoardPnsNewSegment* getSegment() const noexcept {
    return std::get_if<BoardPnsNewSegment>(&geometry);
  }

  /// @brief Get the via, or `nullptr` if this item is not one
  const BoardPnsNewVia* getVia() const noexcept {
    return std::get_if<BoardPnsNewVia>(&geometry);
  }

  /// @brief Get the arc, or `nullptr` if this item is not one
  ///
  /// Never anything but `nullptr` for a commit a LibrePCB session produced,
  /// see ::librepcb::BoardPnsNewArc.
  const BoardPnsNewArc* getArc() const noexcept {
    return std::get_if<BoardPnsNewArc>(&geometry);
  }
};

/**
 * @brief What one routing session changed
 *
 * The three lists are applied in one undo transaction. An update is a
 * removal and an addition the router folded together because they share a
 * board object, which is what keeps that object's identity across a route.
 */
struct BoardPnsCommit final {
  QVector<BoardPnsHostRef> removed;
  QVector<BoardPnsNewItem> added;
  QVector<QPair<BoardPnsHostRef, BoardPnsNewItem>> updated;

  /// The devices a footprint drag moved. They have to be moved before the
  /// three lists above are applied, because the traces in them already end
  /// on the pads' new positions.
  QVector<BoardPnsMovedDevice> movedDevices;

  /// @brief Check whether the commit changes nothing at all
  bool isEmpty() const noexcept {
    return removed.isEmpty() && added.isEmpty() && updated.isEmpty() &&
        movedDevices.isEmpty();
  }
};

/*******************************************************************************
 *  Class BoardPnsRouter
 ******************************************************************************/

/**
 * @brief One interactive push and shove routing session over one board
 *
 * Owns a ::librepcb::BoardPnsSnapshot, which it hands to the router on
 * construction, and keeps it afterwards for the host ID and net tables that
 * translate the router's answers back into board objects.
 *
 * The class is headless: it has no user interface, no graphics items and no
 * undo stack. It takes already snapped points and host IDs, because snapping
 * depends on the grid, on layer visibility and on the high contrast mode,
 * none of which the router knows. A caller drives it with the event methods,
 * reads #getPreview() after each of them, and applies the
 * ::librepcb::BoardPnsCommit the session ends with.
 *
 * The board is read once, in the constructor. Pointers into it stay valid
 * only as long as the board is not edited underneath the session, so a
 * caller must block other editors for the session's lifetime.
 *
 * @note This is just a wrapper around its Rust implementation.
 */
class BoardPnsRouter final {
public:
  /**
   * @brief Which algorithm a session runs
   */
  enum class Mode {
    MarkObstacles,  ///< Route straight through and mark what collides.
    Walkaround,  ///< Bend the route around whatever is in the way.
    Shove,  ///< Push colliding traces and vias aside.
  };

  /**
   * @brief The algorithm and the geometry a session places with
   */
  struct Settings final {
    Mode mode = Mode::Walkaround;
    PositiveLength traceWidth;
    PositiveLength viaDiameter;
    PositiveLength viaDrill;

    /// How many times the shove may push a colliding trace before it gives
    /// up and the router walks around instead. KiCad's default is 250. A
    /// smaller value bounds the time one mouse move can take on a densely
    /// populated board.
    uint shoveIterationLimit = 250;

    /// Whether a route which breaks a design rule may be committed anyway,
    /// which is KiCad's "Allow DRC violations". Only
    /// ::librepcb::BoardPnsRouter::Mode::MarkObstacles honours it, that
    /// being the mode whose job is to show what a route breaks; the other
    /// two never place colliding copper either way.
    bool allowDrcViolations = false;

    /// Whether corners are built at 90 degrees instead of 45. Part of the
    /// settings rather than a command of its own, so that a session
    /// rebuilt after a commit starts on the mode the user left.
    bool cornerMode90 = false;

    /// Whether the session records what it is driven with, so that
    /// #takeRecording() can answer with it. Off by default: a recording
    /// keeps a copy of the whole board snapshot and of every event.
    /// Only honoured by the constructor, not by #setSettings().
    bool recordSession = false;
  };

  /**
   * @brief Why a session refused to start
   *
   * The last three can only come out of #startDragging().
   */
  enum class StartResult {
    Ok,  ///< The point may be routed from.
    AlreadyRouting,  ///< A route is already being placed.
    UnknownStartItem,  ///< The host ID is not one of this snapshot's.
    NotRoutable,  ///< A drill, a board edge or another fixed obstacle.
    StartPointViolatesRules,  ///< Even a minimum width trace collides here.
    PlacerRefused,  ///< The router could not build a placement.
    NothingToDrag,  ///< No object was named to drag.
    IncompleteDeviceDrag,  ///< Pads which are not one whole device, which
                           ///< would leave the traces of the pads left out
                           ///< behind.
    NotDraggable,  ///< A hole, a copper graphic, a keepout zone or a pad
                   ///< of no device: obstacles the router never moves.
  };

  /**
   * @brief What happened to a fix
   */
  enum class FixOutcome {
    Continue,  ///< The placement carries on, read #getPreview() again.
    Finished,  ///< The route was committed, read #getCommit().
    NotRouting,  ///< Nothing was being routed, so nothing was committed.
  };

  // Constructors / Destructor
  BoardPnsRouter() = delete;
  BoardPnsRouter(const BoardPnsRouter& other) = delete;

  /**
   * @brief Open a routing session over a board
   *
   * @param board     The board to route on. Snapshotted in the constructor
   *                  and not referenced afterwards, but the board objects it
   *                  names must outlive the session.
   * @param settings  The algorithm and the geometry to place with.
   *
   * @throws ::librepcb::RuntimeError if the board carries a coordinate
   *         outside the range the router supports, which is about plus or
   *         minus 2 metres.
   */
  BoardPnsRouter(const Board& board, const Settings& settings);
  ~BoardPnsRouter() noexcept;

  // Getters

  /**
   * @brief Get the number of copper layers of the board
   */
  int getCopperLayerCount() const noexcept;

  /**
   * @brief Check whether a route is being placed or an object dragged
   *
   * True for both, which is the router's own "routing in progress"; use
   * #isDragging() to tell the two apart.
   */
  bool isRoutingInProgress() const noexcept;

  /**
   * @brief Check whether an existing object is being dragged
   *
   * The one thing #isRoutingInProgress() cannot tell apart from a
   * placement. A drag runs the same event methods as a route, but it only
   * ever commits through #fixRoute().
   */
  bool isDragging() const noexcept;

  /**
   * @brief Get the layer the route is being placed on
   *
   * @return The layer, or `nullptr` if nothing is being routed.
   */
  const Layer* getCurrentLayer() const noexcept;

  /**
   * @brief Check whether the next fix would place a via
   */
  bool isPlacingVia() const noexcept;

  /**
   * @brief Get everything to draw after the last event
   *
   * Rebuilt after every event that produces a frame, which is #startRouting(),
   * #moveTo(), #fixRoute(), #finish(), #stopRouting() and #abortRouting().
   * The small commands do not produce one, so a caller follows them with a
   * #moveTo() like the router's other hosts do.
   */
  const BoardPnsPreview& getPreview() const noexcept { return mPreview; }

  /**
   * @brief Get what the last commit changed
   *
   * Refreshed by every call that can commit, which is #fixRoute(), #finish()
   * and #stopRouting(). Cleared by #abortRouting().
   *
   * By value, like #stopRouting()'s answer: a caller applies the commit by
   * building a new session over the edited board, which destroys this one,
   * so a reference into it would dangle exactly where it is used.
   */
  BoardPnsCommit getCommit() const noexcept { return mCommit; }

  /**
   * @brief Get the board object of each host ID
   *
   * Index 0 is an all null entry, because host ID 0 is the null value.
   */
  const QVector<BoardPnsHostRef>& getHostRefs() const noexcept;

  /**
   * @brief Get the board object of one host ID
   *
   * @return The board object, or an all null reference for host ID 0 and for
   *         an ID this session does not know.
   */
  BoardPnsHostRef getHostRef(quint64 hostId) const noexcept;

  /**
   * @brief Get the snapshot the session was built from
   *
   * Its host ID table is how a caller turns a board object into the host ID
   * the event methods take.
   */
  const BoardPnsSnapshot& getSnapshot() const noexcept { return *mSnapshot; }

  // General Methods

  /**
   * @brief Find every board object under a point
   *
   * @param pos     The point to test, in board coordinates.
   * @param layer   The copper layer to filter by, or `nullptr` for any.
   *
   * @return The host IDs, which are what the other methods take. Map them to
   *         board objects with #getHostRef().
   */
  QVector<quint64> hover(const Point& pos, const Layer* layer) noexcept;

  /**
   * @brief Check whether a route may be started at a point
   *
   * The same gate #startRouting() runs, so that a caller can grey out its
   * cursor before the user clicks.
   *
   * @param pos         The already snapped start point.
   * @param startItem   The host ID under the cursor, or 0 for free space.
   * @param layer       The copper layer to start on.
   */
  StartResult isStartingPointRoutable(const Point& pos, quint64 startItem,
                                      const Layer& layer) const noexcept;

  /**
   * @brief Begin routing a trace
   *
   * @param pos         The already snapped start point.
   * @param startItem   The host ID under the cursor, or 0 for free space.
   * @param layer       The copper layer to start on.
   */
  StartResult startRouting(const Point& pos, quint64 startItem,
                           const Layer& layer) noexcept;

  /**
   * @brief Begin dragging existing board objects
   *
   * Which kind of drag it becomes is the router's decision, taken from the
   * shape of the set and from where on it the drag began: nothing but pads
   * is a footprint drag, which moves the devices which own them and pulls
   * the traces on their pads along; more than one trace is a multi drag,
   * which moves every one of them; and a single trace or via is a plain
   * drag, where a click near an end drags that corner and a click in the
   * middle drags the segment. The algorithm is the mode the session was
   * built with, exactly as for a route.
   *
   * The caller has to hand in a whole device's pads for a footprint drag,
   * because the router moves only the pads it is given while the device
   * moves as a whole: a set which is neither all the pads of the devices
   * it names nor free of pads is refused with
   * ::librepcb::BoardPnsRouter::StartResult::IncompleteDeviceDrag.
   *
   * The session holds the frame of a drag which has not moved yet, which
   * is empty, so a caller follows this with a #moveTo().
   *
   * @param pos         The already snapped point the drag starts at.
   * @param items       The host IDs of the objects to drag. Zeros are
   *                    ignored, so an empty set and a set of nothing but
   *                    zeros both answer
   *                    ::librepcb::BoardPnsRouter::StartResult::NothingToDrag.
   * @param freeAngle   Whether to drag the clicked corner without the 45
   *                    degree constraint. Reaches a single drag only.
   */
  StartResult startDragging(const Point& pos, const QVector<quint64>& items,
                            bool freeAngle) noexcept;

  /**
   * @brief Begin dragging one existing trace or via
   *
   * The one object overload of #startDragging(), which is every gesture
   * but the footprint drag and the multi drag.
   */
  StartResult startDragging(const Point& pos, quint64 hostId,
                            bool freeAngle) noexcept;

  /**
   * @brief Move the end of the route, or the object being dragged
   *
   * @param pos       The already snapped cursor point.
   * @param endItem   The host ID under the cursor, or 0 for free space.
   *                  Ignored while dragging.
   */
  void moveTo(const Point& pos, quint64 endItem) noexcept;

  /**
   * @brief Pin the route down to where the cursor is
   *
   * @param pos           The already snapped cursor point.
   * @param endItem       The host ID under the cursor, or 0 for free space.
   * @param forceFinish   Whether to end the session here rather than start a
   *                      new leg, which is what a double click does. For a
   *                      drag it is the force commit instead, which takes
   *                      the drag even where the rules refuse it; a drag is
   *                      always terminal either way.
   */
  FixOutcome fixRoute(const Point& pos, quint64 endItem,
                      bool forceFinish) noexcept;

  /**
   * @brief Route the rest of the way to the nearest unconnected anchor
   *
   * ::librepcb::BoardPnsRouter::FixOutcome::NotRouting if nothing was being
   * routed, if nothing unconnected was left to reach, or if the route did not
   * settle on the anchor. Nothing is committed in that case.
   */
  FixOutcome finish() noexcept;

  /**
   * @brief Undo the last fix
   *
   * @return Where the undone leg began, which a caller uses to warp the
   *         cursor back there, or `std::nullopt` if there was nothing to
   *         undo.
   */
  std::optional<Point> undoLastSegment() noexcept;

  /**
   * @brief Move the route to another copper layer
   *
   * @return Whether the router honoured the request. It refuses once a fix
   *         has ended a leg without leaving a via behind.
   */
  bool switchLayer(const Layer& layer) noexcept;

  /**
   * @brief Arm or disarm the via the next fix would place
   *
   * The via is only materialised on the next #moveTo(), so a caller has to
   * move before the preview shows it.
   *
   * @return Whether the router honoured the request, not the new state. Read
   *         that back with #isPlacingVia().
   */
  bool toggleViaPlacement() noexcept;

  /**
   * @brief Turn the route's first corner the other way
   */
  void flipPosture() noexcept;

  /**
   * @brief Commit what was routed and end the session
   *
   * A drag commits nothing here, which is the router's own semantics:
   * #fixRoute() is the only path which commits one, so a drag which was
   * never fixed is discarded.
   *
   * @return What the caller has to apply to the board. Empty if nothing was
   *         being routed.
   */
  BoardPnsCommit stopRouting() noexcept;

  /**
   * @brief Throw the session away without committing anything
   */
  void abortRouting() noexcept;

  /**
   * @brief Replace the algorithm and the geometry to place with
   *
   * A running placement keeps the geometry it started with, because the
   * router has no entry point for a mid route size change yet. A caller
   * applies a width change by fixing and starting a new leg. Everything
   * which is not geometry, which is the mode, the shove iteration limit,
   * the DRC violation switch and the corner mode, reaches a running
   * placement on its next #moveTo().
   *
   * ::librepcb::BoardPnsRouter::Settings::recordSession is ignored here: a
   * recording opens with the board snapshot, which only exists while the
   * session is being constructed.
   */
  void setSettings(const Settings& settings) noexcept;

  /**
   * @brief Take what this session recorded, in the router's own format
   *
   * The text of the router crate's `SessionRecording::to_text()`: the board
   * snapshot, the settings, the sizes, every event the session was driven
   * with and every commit it answered. The crate parses it back with
   * `SessionRecording::from_text()` and replays it, so a file written from
   * here is a regression fixture for the router without any conversion.
   *
   * Taking the recording ends it, which is the crate's own semantics. A
   * caller that wants to keep recording has to build a new session.
   *
   * @return The recording, or an empty string if the session was not built
   *         with ::librepcb::BoardPnsRouter::Settings::recordSession or if
   *         its recording was already taken.
   */
  QString takeRecording() noexcept;

  // Operator Overloadings
  BoardPnsRouter& operator=(const BoardPnsRouter& rhs) = delete;

private:  // Methods
  /**
   * @brief Rebuild #mPreview from the frame the session holds
   */
  void updatePreview() noexcept;

  /**
   * @brief Rebuild #mCommit from the commit the session holds
   */
  void updateCommit() noexcept;

  /**
   * @brief Check whether a set of board objects may be dragged at all
   *
   * The host's own gate in front of the router's: it decides what a set of
   * pads means, because the router moves pads and the host moves devices.
   *
   * @return ::librepcb::BoardPnsRouter::StartResult::Ok if the router
   *         should be asked.
   */
  StartResult checkDraggableItems(const QVector<quint64>& items) const noexcept;

  /**
   * @brief Add one moved pad to a list of moved devices
   *
   * The router reports one entry per pad and the host moves the device
   * which owns it, so the pads of one device fold into one entry. A pad of
   * no device is dropped: #checkDraggableItems() refuses to start such a
   * drag, so this can only be reached by a caller which bypassed it.
   */
  static void appendMovedDevice(QVector<BoardPnsMovedDevice>& devices,
                                const BoardPnsHostRef& ref,
                                const Point& offset) noexcept;

  /**
   * @brief Convert one router preview via into board vocabulary
   */
  BoardPnsPreviewVia toPreviewVia(const rs::PnsPreviewVia& via) const noexcept;

  /**
   * @brief Convert one router commit item into board vocabulary
   */
  BoardPnsNewItem toNewItem(const rs::PnsNewItem& item) const noexcept;

  /**
   * @brief Map a dense copper layer index back to a board layer
   */
  const Layer* toLayer(int denseIndex) const noexcept;

  /**
   * @brief Map an FFI net number back to a net signal
   */
  NetSignal* toNetSignal(quint32 netNumber) const noexcept;

private:  // Data
  /// Kept alive for its host ID and net tables. Not movable, hence the
  /// pointer.
  std::unique_ptr<BoardPnsSnapshot> mSnapshot;

  /// The board's inner layer count, which the dense layer mapping needs.
  int mInnerLayerCount;

  RustHandle<rs::PnsRouter> mHandle;
  BoardPnsPreview mPreview;
  BoardPnsCommit mCommit;
};

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace librepcb

#endif
