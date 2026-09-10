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

#ifndef LIBREPCB_EDITOR_BOARDEDITORSTATE_ROUTETRACE_H
#define LIBREPCB_EDITOR_BOARDEDITORSTATE_ROUTETRACE_H

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include "boardeditorstate.h"

#include <librepcb/core/project/board/boardpnsrouter.h>

#include <QtCore>

#include <memory>
#include <optional>

/*******************************************************************************
 *  Namespace / Forward Declarations
 ******************************************************************************/
namespace librepcb {

class BI_NetPoint;
class FilePath;
class Layer;
class NetSignal;

namespace editor {

class BoardPnsPreviewItems;

/*******************************************************************************
 *  Class BoardEditorState_RouteTrace
 ******************************************************************************/

/**
 * @brief The "route trace" state/tool of the board editor
 *
 * Drives a ::librepcb::BoardPnsRouter, the push and shove routing session,
 * and applies what it commits with
 * ::librepcb::editor::CmdBoardApplyPnsCommit. It coexists with
 * ::librepcb::editor::BoardEditorState_DrawTrace, which is untouched.
 *
 * The two tools differ in where the trace being placed lives. The draw trace
 * tool opens an undo command group on entry and edits real board items for
 * the whole route, so the board is in a half edited state while the tool
 * runs. This tool holds no command group at all: the route is inside the
 * router until it is committed, and one commit is one undo entry.
 *
 * The router works on a snapshot of the board taken in its constructor, so
 * every board object it names is stale as soon as the board is edited. The
 * session is therefore rebuilt on entry and after every applied commit, and
 * other editors are blocked for the tool's lifetime.
 *
 * Snapping stays here rather than in the router, because it depends on the
 * grid, on layer visibility and on what is under the cursor, none of which
 * the router knows. #snapCursor() is the only place that produces the
 * ::librepcb::editor::BoardEditorState_RouteTrace::SnappedCursor every
 * router call takes, so an unsnapped point cannot reach the router.
 *
 * @note Setting the environment variable `LIBREPCB_PNS_RECORD_DIR` to an
 *       existing directory makes every session record what it is driven
 *       with and write it to `<dir>/<yyyyMMdd-HHmmss>-<board name>.txt`
 *       when the session ends. The file is in the router crate's own
 *       recorded session format, so it can be dropped into the crate's
 *       `tests/fixtures/sessions/` and replayed as a regression fixture.
 *       This is a developer switch with no user interface: nothing happens
 *       while the variable is unset, and a failed write only reaches the
 *       status bar, never a dialog, and never interrupts routing.
 */
class BoardEditorState_RouteTrace final : public BoardEditorState {
  Q_OBJECT

public:
  // Constructors / Destructor
  BoardEditorState_RouteTrace() = delete;
  BoardEditorState_RouteTrace(const BoardEditorState_RouteTrace& other) =
      delete;
  explicit BoardEditorState_RouteTrace(const Context& context) noexcept;
  ~BoardEditorState_RouteTrace() noexcept override;

  // General Methods
  bool entry() noexcept override;
  bool exit() noexcept override;

  // Event Handlers
  bool processAbortCommand() noexcept override;
  bool processKeyPressed(const GraphicsSceneKeyEvent& e) noexcept override;
  bool processKeyReleased(const GraphicsSceneKeyEvent& e) noexcept override;
  bool processGraphicsSceneMouseMoved(
      const GraphicsSceneMouseEvent& e) noexcept override;
  bool processGraphicsSceneLeftMouseButtonPressed(
      const GraphicsSceneMouseEvent& e) noexcept override;
  bool processGraphicsSceneLeftMouseButtonDoubleClicked(
      const GraphicsSceneMouseEvent& e) noexcept override;
  bool processGraphicsSceneRightMouseButtonReleased(
      const GraphicsSceneMouseEvent& e) noexcept override;

  // Connection to UI
  QSet<const Layer*> getAvailableLayers() noexcept;
  const Layer& getLayer() const noexcept;
  void setLayer(const Layer& layer) noexcept;
  BoardPnsRouter::Mode getMode() const noexcept { return mCurrentMode; }
  void setMode(BoardPnsRouter::Mode mode) noexcept;
  /// Whether 90 degree corners are built instead of 45 degree ones.
  bool getCornerMode() const noexcept { return mCornerMode90; }
  void setCornerMode(bool corners90) noexcept;
  void flipPosture() noexcept;
  void toggleVia() noexcept;
  const PositiveLength& getWidth() const noexcept { return mCurrentWidth; }
  void setWidth(const PositiveLength& width) noexcept;
  bool getViaAutoDrillDiameter() const noexcept {
    return !mCurrentViaDrill.has_value();
  }
  PositiveLength getViaDrillDiameter() const noexcept;
  void setViaDrillDiameter(
      const std::optional<PositiveLength>& diameter) noexcept;
  bool getAutoViaSize() const noexcept { return !mCurrentViaSize.has_value(); }
  PositiveLength getViaSize() const noexcept;
  void setViaSize(const std::optional<PositiveLength>& size) noexcept;

  // Operator Overloadings
  BoardEditorState_RouteTrace& operator=(
      const BoardEditorState_RouteTrace& rhs) = delete;

signals:
  void layerChanged(const Layer& layer);
  void modeChanged(BoardPnsRouter::Mode mode);
  void cornerModeChanged(bool corners90);
  void widthChanged(const PositiveLength& width);
  void viaDrillDiameterChanged(bool autoSize, const PositiveLength& diameter);
  void viaSizeChanged(bool autoSize, const PositiveLength& size);

private:  // Types
  /**
   * @brief A cursor position the router may be handed
   *
   * Produced only by #snapCursor(), so that no code path can pass an
   * unsnapped point to the router.
   */
  struct SnappedCursor final {
    /// The snapped position: the grid point, or the position of the board
    /// object under the cursor when one was found.
    Point pos;

    /// The host ID of that board object, or 0 for free space.
    quint64 item = 0;
  };

private:  // Methods
  /**
   * @brief Build a new routing session over the current board
   *
   * @return True on success. On failure the error is shown and the tool has
   *         no session, so the caller must leave the tool.
   */
  bool createRouter() noexcept;

  /**
   * @brief Build a new routing session and leave the tool if that fails
   */
  void rebuildRouter() noexcept;

  /**
   * @brief Push the current width, via size, via drill and workspace
   *        settings into the session
   *
   * Also connected to the workspace setting the router reads, so that a
   * change made while the tool is open reaches the running session. Does
   * nothing when no session is open.
   */
  void updateRouterSettings() noexcept;

  /**
   * @brief Get the shove iteration limit from the workspace settings
   *
   * Clamped to a range the router can work with: zero would make every
   * shove fail immediately, and a value far above the default only wastes
   * time the user waits for.
   */
  uint getShoveIterationLimit() const noexcept;

  /**
   * @brief Snap the cursor to the grid and to the board object under it
   *
   * While routing, the search is restricted to the layer being routed on and
   * to the net being routed, which is what the draw trace tool does for its
   * end anchor. Shift disables it, in which case the plain grid point is
   * returned.
   */
  SnappedCursor snapCursor() noexcept;

  /**
   * @brief Get the host ID a net point stands for
   *
   * A net point is not a router object; the traces meeting there are. The
   * lowest host ID of them is taken, which is deterministic because the host
   * IDs are handed out in the snapshot's own walk order while
   * ::librepcb::BI_NetPoint::getNetLines() is an unordered set.
   */
  quint64 getHostIdOfNetPoint(const BI_NetPoint& netPoint) const noexcept;

  /**
   * @brief Get the net signal of a board object the router named
   */
  const NetSignal* getNetSignalOfHostId(quint64 hostId) const noexcept;

  /**
   * @brief Begin a route at the cursor
   */
  void startRouting(const SnappedCursor& cursor) noexcept;

  /**
   * @brief Move the end of the route to the cursor
   *
   * Also the way to make the router materialise what a command without a
   * frame changed, which is a layer switch, a via toggle, a posture flip and
   * an undone segment.
   */
  void moveToCursor() noexcept;

  /**
   * @brief Pin the route down at the cursor and apply what that commits
   *
   * @param forceFinish   Whether to end the session here rather than start a
   *                      new leg, which is what a double click does.
   */
  void fixRoute(const SnappedCursor& cursor, bool forceFinish) noexcept;

  /**
   * @brief Commit what was routed so far and return to the idle state
   *
   * This is what escape does. Both KiCad and Horizon keep the already fixed
   * part of a route on escape rather than throwing it away, and so does
   * this tool, which is a deliberate difference to the draw trace tool.
   */
  void stopRouting() noexcept;

  /**
   * @brief Apply one commit to the board as a single undo entry
   *
   * Does not rebuild the session; the caller does that, because the tool
   * exit path applies a commit without needing a new session.
   */
  void applyCommit(const BoardPnsCommit& commit) noexcept;

  /**
   * @brief Get the directory recorded sessions are written to
   *
   * @return The directory named by `LIBREPCB_PNS_RECORD_DIR`, or an invalid
   *         ::librepcb::FilePath when the variable is unset or does not name
   *         an existing directory, which is what switches recording off.
   */
  static FilePath getRecordingDirectory() noexcept;

  /**
   * @brief Write what the current session recorded, if it recorded anything
   *
   * Taking the recording ends it, so this must be called exactly once per
   * session, right before the session is replaced or dropped. Does nothing
   * when there is no session, when recording is off, or when the recording
   * was already taken.
   */
  void writeSessionRecording() noexcept;

  /**
   * @brief Get the message to show for a refused start
   */
  static QString getStartResultMessage(
      BoardPnsRouter::StartResult result) noexcept;

private:  // Data
  /// The routing session. Null only if building it failed, in which case the
  /// tool is on its way out.
  std::unique_ptr<BoardPnsRouter> mRouter;

  /// Draws what the session wants shown. Null if the tool was entered
  /// without a graphics scene.
  std::unique_ptr<BoardPnsPreviewItems> mPreviewItems;

  /// The layer a new route starts on. While routing, the router owns the
  /// current layer and #getLayer() reports its answer instead.
  const Layer* mCurrentLayer;

  /// The algorithm the router runs. Applied to a running placement too, the
  /// router picks it up on the next move.
  BoardPnsRouter::Mode mCurrentMode;

  /// Whether the router builds 90 degree corners instead of 45 degree ones.
  /// Not part of ::librepcb::BoardPnsRouter::Settings, so it has to be
  /// re-applied to every new session.
  bool mCornerMode90;

  PositiveLength mCurrentWidth;  ///< the current trace width

  /// The via drill diameter, or `std::nullopt` for the board's default.
  std::optional<PositiveLength> mCurrentViaDrill;

  /// The via size, or `std::nullopt` to derive it from the drill diameter.
  std::optional<PositiveLength> mCurrentViaSize;

  Point mCursorPos;  ///< the current cursor position, not snapped
  bool mSnapActive;  ///< whether the cursor snaps to board objects

  /// The net of the route being placed, `nullptr` for a route in free space
  /// and while idle.
  const NetSignal* mCurrentNetSignal;
};

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb

#endif
