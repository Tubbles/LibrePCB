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
#include "boardeditorstate_routetrace.h"

#include "../../../undostack.h"
#include "../../cmd/cmdboardapplypnscommit.h"
#include "../boardgraphicsscene.h"
#include "../boardpnspreview.h"
#include "../graphicsitems/bgi_netline.h"
#include "../graphicsitems/bgi_netpoint.h"
#include "../graphicsitems/bgi_pad.h"
#include "../graphicsitems/bgi_via.h"
#include "../pnssessionrecorder.h"

#include <librepcb/core/geometry/via.h>
#include <librepcb/core/project/board/board.h>
#include <librepcb/core/project/board/boarddesignrules.h>
#include <librepcb/core/project/board/items/bi_netline.h>
#include <librepcb/core/project/board/items/bi_netpoint.h>
#include <librepcb/core/project/board/items/bi_netsegment.h>
#include <librepcb/core/project/board/items/bi_pad.h>
#include <librepcb/core/project/board/items/bi_via.h>
#include <librepcb/core/types/layer.h>
#include <librepcb/core/utils/toolbox.h>
#include <librepcb/core/workspace/workspace.h>
#include <librepcb/core/workspace/workspacesettings.h>

#include <QtCore>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {

/*******************************************************************************
 *  Constructors / Destructor
 ******************************************************************************/

BoardEditorState_RouteTrace::BoardEditorState_RouteTrace(
    const Context& context) noexcept
  : BoardEditorState(context),
    mRouter(),
    mPreviewItems(),
    mCurrentLayer(&Layer::topCopper()),
    mCurrentMode(BoardPnsRouter::Mode::Walkaround),
    mCornerMode90(false),
    mCurrentWidth(mContext.board.getDesignRules().getDefaultTraceWidth()),
    mCurrentViaDrill(std::nullopt),
    mCurrentViaSize(std::nullopt),
    mCursorPos(),
    mSnapActive(true),
    mPendingDrag(std::nullopt),
    mCurrentNetSignal(nullptr) {
  // The workspace settings dialog stays usable while the tool is open, so
  // a new iteration limit, and a new answer to whether a colliding route
  // may be committed, have to reach the running session too.
  connect(&mContext.workspace.getSettings().pnsShoveIterationLimit,
          &WorkspaceSettingsItem::edited, this,
          &BoardEditorState_RouteTrace::updateRouterSettings);
  connect(&mContext.workspace.getSettings().pnsAllowDrcViolations,
          &WorkspaceSettingsItem::edited, this,
          &BoardEditorState_RouteTrace::updateRouterSettings);

  // The last moment at which a session which is recording can still hand
  // its recording over, so this one is not a rebuild.
  connect(&mContext.pnsRecorder, &PnsSessionRecorder::recordingAboutToStop,
          this, &BoardEditorState_RouteTrace::writeSessionRecording);
  connect(&mContext.pnsRecorder, &PnsSessionRecorder::recordingStarted, this,
          &BoardEditorState_RouteTrace::handleRecordingToggled);
  connect(&mContext.pnsRecorder, &PnsSessionRecorder::recordingStopped, this,
          &BoardEditorState_RouteTrace::handleRecordingToggled);
}

BoardEditorState_RouteTrace::~BoardEditorState_RouteTrace() noexcept {
}

/*******************************************************************************
 *  General Methods
 ******************************************************************************/

bool BoardEditorState_RouteTrace::entry() noexcept {
  // The session snapshots the board in its constructor, so any temporary
  // change another editor still holds has to be discarded first.
  abortBlockingToolsInOtherEditors();

  if (!createRouter()) {
    // Leaving from within entry() is not possible, the FSM has not entered
    // this state yet, so ask it to leave once it has (queued connection).
    emit requestLeavingState();
    return true;
  }

  if (BoardGraphicsScene* scene = getActiveBoardScene()) {
    mPreviewItems.reset(new BoardPnsPreviewItems(*scene, mContext.layers));
  }

  mAdapter.fsmToolEnter(*this);
  mAdapter.fsmSetViewCursor(Qt::CrossCursor);
  return true;
}

bool BoardEditorState_RouteTrace::exit() noexcept {
  // Stop drawing before the board is edited: the preview holds board objects
  // hidden and the commit below can delete them.
  mPreviewItems.reset();

  mPendingDrag = std::nullopt;

  std::optional<BoardPnsCommit> commit;
  if (mRouter && mRouter->isRoutingInProgress()) {
    // Keep whatever was fixed, like escape does, but do not build a new
    // session for a tool which is going away. A drag is not fixed by this,
    // it answers an empty commit and is discarded.
    commit = mRouter->stopRouting();
  }

  // After the stop above, so that the last commit is in the file too.
  writeSessionRecording();

  // The board objects the commit names are the ones the session hid, so
  // the session goes away before the board is edited.
  mRouter.reset();
  if (commit) {
    applyCommit(*commit);
  }
  mCurrentNetSignal = nullptr;

  mAdapter.fsmCrossProbe();
  mAdapter.fsmSetViewCursor(std::nullopt);
  mAdapter.fsmToolLeave();
  return true;
}

/*******************************************************************************
 *  Event Handlers
 ******************************************************************************/

bool BoardEditorState_RouteTrace::processAbortCommand() noexcept {
  mPendingDrag = std::nullopt;

  if (mRouter && mRouter->isDragging()) {
    // A drag is thrown away instead of being kept: nothing of it was ever
    // fixed, so there is nothing to commit.
    abortDragging();
    return true;
  } else if (mRouter && mRouter->isRoutingInProgress()) {
    // Just finish the current route, not exiting the whole tool.
    stopRouting();
    return true;
  } else {
    // Allow leaving the tool.
    return false;
  }
}

bool BoardEditorState_RouteTrace::processKeyPressed(
    const GraphicsSceneKeyEvent& e) noexcept {
  if ((!mRouter) || (!mRouter->isRoutingInProgress())) {
    return false;
  }

  switch (e.key) {
    case Qt::Key_Shift: {
      mSnapActive = false;
      moveToCursor();
      return true;
    }
    case Qt::Key_Backspace: {
      // The router clears its head when a segment is undone, so the route
      // only becomes visible again after a move.
      mRouter->undoLastSegment();
      moveToCursor();
      return true;
    }
    default:
      break;
  }

  return false;
}

bool BoardEditorState_RouteTrace::processKeyReleased(
    const GraphicsSceneKeyEvent& e) noexcept {
  if ((!mRouter) || (!mRouter->isRoutingInProgress())) {
    return false;
  }

  switch (e.key) {
    case Qt::Key_Shift: {
      mSnapActive = true;
      moveToCursor();
      return true;
    }
    default:
      break;
  }

  return false;
}

bool BoardEditorState_RouteTrace::processGraphicsSceneMouseMoved(
    const GraphicsSceneMouseEvent& e) noexcept {
  // Update snap, in case we missed the key pressed/released events for some
  // reason (e.g. focus issue).
  mSnapActive = !e.modifiers.testFlag(Qt::ShiftModifier);
  mCursorPos = e.scenePos;

  if (mPendingDrag) {
    if (!e.buttons.testFlag(Qt::LeftButton)) {
      // The release was lost, so the press cannot become anything any more.
      mPendingDrag = std::nullopt;
    } else if (exceedsDragThreshold(mPendingDrag->pressPos, e.scenePos)) {
      // Far enough from the press to mean a drag rather than a click. The
      // drag starts where the button went down, not where the cursor is
      // now, and the move below takes it from there to the cursor.
      const SnappedCursor cursor = mPendingDrag->cursor;
      mPendingDrag = std::nullopt;
      startDragging(cursor);
    }
  }

  if (mRouter && mRouter->isRoutingInProgress()) {
    moveToCursor();
    return true;
  }

  return false;
}

bool BoardEditorState_RouteTrace::processGraphicsSceneLeftMouseButtonPressed(
    const GraphicsSceneMouseEvent& e) noexcept {
  if (!mRouter) return false;

  mSnapActive = !e.modifiers.testFlag(Qt::ShiftModifier);
  mCursorPos = e.scenePos;
  mPendingDrag = std::nullopt;

  if (mRouter->isDragging()) {
    // The button which started the drag is still down and the drag ends on
    // its release, so nothing may happen in between.
    return true;
  }

  const SnappedCursor cursor = snapCursor();
  if (mRouter->isRoutingInProgress()) {
    fixRoute(cursor, false);
  } else if (isDraggable(cursor.item)) {
    // Whether this press starts a route or drags the object under it is not
    // known yet, so the decision waits for the first mouse move or for the
    // release.
    mPendingDrag = PendingDrag{e.scenePos, cursor};
  } else {
    startRouting(cursor);
  }
  return true;
}

bool BoardEditorState_RouteTrace::processGraphicsSceneLeftMouseButtonReleased(
    const GraphicsSceneMouseEvent& e) noexcept {
  if (!mRouter) return false;

  mSnapActive = !e.modifiers.testFlag(Qt::ShiftModifier);
  mCursorPos = e.scenePos;

  if (mRouter->isDragging()) {
    // The drag ends where the cursor is, which is what KiCad's router does
    // when the button goes up, and the force flag is the drag's force
    // commit: there is no button left to try a refused fix again with.
    fixRoute(snapCursor(), true);
    if (mRouter && mRouter->isDragging()) {
      // The router refused even the forced commit, so the drag is dropped
      // rather than left running without a button holding it.
      mAdapter.fsmSetStatusBarMessage(
          tr("The router could not move this object here."), 3000);
      abortDragging();
    }
    return true;
  }

  if (mPendingDrag) {
    // A press which never travelled far enough is an ordinary click, and a
    // click starts a route where the button went down.
    const SnappedCursor cursor = mPendingDrag->cursor;
    mPendingDrag = std::nullopt;
    startRouting(cursor);
    return true;
  }

  return false;
}

bool BoardEditorState_RouteTrace::
    processGraphicsSceneLeftMouseButtonDoubleClicked(
        const GraphicsSceneMouseEvent& e) noexcept {
  if ((!mRouter) || (!mRouter->isRoutingInProgress())) {
    return processGraphicsSceneLeftMouseButtonPressed(e);
  }

  mSnapActive = !e.modifiers.testFlag(Qt::ShiftModifier);
  mCursorPos = e.scenePos;
  fixRoute(snapCursor(), true);
  return true;
}

bool BoardEditorState_RouteTrace::processGraphicsSceneRightMouseButtonReleased(
    const GraphicsSceneMouseEvent& e) noexcept {
  mCursorPos = e.scenePos;

  if (mRouter && mRouter->isRoutingInProgress()) {
    flipPosture();

    // Always accept the event if we are routing! When ignoring the event, the
    // state machine will abort the tool by a right click!
    return true;
  }

  return false;
}

/*******************************************************************************
 *  Connection to UI
 ******************************************************************************/

QSet<const Layer*> BoardEditorState_RouteTrace::getAvailableLayers() noexcept {
  return mContext.board.getCopperLayers();
}

const Layer& BoardEditorState_RouteTrace::getLayer() const noexcept {
  if (mRouter) {
    if (const Layer* layer = mRouter->getCurrentLayer()) {
      return *layer;
    }
  }
  return *mCurrentLayer;
}

void BoardEditorState_RouteTrace::setLayer(const Layer& layer) noexcept {
  if (!mContext.board.getCopperLayers().contains(&layer)) return;
  if (&layer == &getLayer()) return;

  if (mRouter && mRouter->isRoutingInProgress()) {
    if (!mRouter->switchLayer(layer)) {
      mAdapter.fsmSetStatusBarMessage(
          tr("The router cannot change the layer here."), 3000);
      // Put the toolbar back onto the layer which is actually routed on.
      emit layerChanged(getLayer());
      return;
    }
    mCurrentLayer = &layer;
    makeLayerVisible(layer.getColorRole());
    // A layer switch produces no frame, so follow it with a move.
    moveToCursor();
  } else {
    mCurrentLayer = &layer;
    makeLayerVisible(layer.getColorRole());
  }

  emit layerChanged(*mCurrentLayer);
}

void BoardEditorState_RouteTrace::setMode(BoardPnsRouter::Mode mode) noexcept {
  if (mode == mCurrentMode) return;

  mCurrentMode = mode;
  emit modeChanged(mCurrentMode);

  // Unlike the geometry, the mode may change in the middle of a route; the
  // router applies it from the next move on.
  updateRouterSettings();
  moveToCursor();
}

void BoardEditorState_RouteTrace::setCornerMode(bool corners90) noexcept {
  if (corners90 == mCornerMode90) return;

  mCornerMode90 = corners90;
  emit cornerModeChanged(mCornerMode90);

  // Like the mode, the corner mode may change in the middle of a route; the
  // router applies it from the next move on.
  updateRouterSettings();
  moveToCursor();
}

void BoardEditorState_RouteTrace::flipPosture() noexcept {
  if ((!mRouter) || (!mRouter->isRoutingInProgress())) return;

  mRouter->flipPosture();
  moveToCursor();
}

void BoardEditorState_RouteTrace::toggleVia() noexcept {
  if ((!mRouter) || (!mRouter->isRoutingInProgress())) return;

  mRouter->toggleViaPlacement();
  moveToCursor();
}

void BoardEditorState_RouteTrace::setWidth(
    const PositiveLength& width) noexcept {
  if (width == mCurrentWidth) return;

  mCurrentWidth = width;
  emit widthChanged(mCurrentWidth);

  // Note: A running placement keeps the width it started with, the router has
  // no entry point for a mid route size change. The new width applies to the
  // next leg.
  updateRouterSettings();
}

PositiveLength BoardEditorState_RouteTrace::getViaDrillDiameter()
    const noexcept {
  if (auto drill = mCurrentViaDrill) {
    return *drill;
  }
  return mContext.board.getDesignRules().getDefaultViaDrillDiameter();
}

void BoardEditorState_RouteTrace::setViaDrillDiameter(
    const std::optional<PositiveLength>& diameter) noexcept {
  // Avoid creating a via with auto drill but manual size.
  if ((!diameter) && mCurrentViaSize) {
    setViaSize(std::nullopt);
  }

  // Avoid creating vias with a drill larger than size.
  if (diameter && mCurrentViaSize && ((*diameter) > (*mCurrentViaSize))) {
    setViaSize(diameter);
  }

  if (diameter == mCurrentViaDrill) return;

  const PositiveLength oldSize = getViaSize();
  mCurrentViaDrill = diameter;
  emit viaDrillDiameterChanged(!mCurrentViaDrill.has_value(),
                               getViaDrillDiameter());

  const PositiveLength newSize = getViaSize();
  if (newSize != oldSize) {
    emit viaSizeChanged(!mCurrentViaSize.has_value(), newSize);
  }

  updateRouterSettings();
}

PositiveLength BoardEditorState_RouteTrace::getViaSize() const noexcept {
  if (auto size = mCurrentViaSize) {
    return *size;
  }
  return Via::calcSizeFromRules(
      getViaDrillDiameter(),
      mContext.board.getDesignRules().getViaAnnularRing());
}

void BoardEditorState_RouteTrace::setViaSize(
    const std::optional<PositiveLength>& size) noexcept {
  // Avoid creating a via with auto drill but manual size.
  if (size && (!mCurrentViaDrill)) {
    setViaDrillDiameter(getViaDrillDiameter());
  }

  // Avoid creating vias with a drill larger than size.
  if (size && ((*size) < mCurrentViaDrill)) {
    setViaDrillDiameter(*size);
  }

  if (size == mCurrentViaSize) return;

  mCurrentViaSize = size;
  emit viaSizeChanged(!mCurrentViaSize.has_value(), getViaSize());

  updateRouterSettings();
}

/*******************************************************************************
 *  Private Methods
 ******************************************************************************/

bool BoardEditorState_RouteTrace::createRouter() noexcept {
  // Every commit and every abort is followed by a new session, so this is
  // the one place an outgoing session's recording has to be collected.
  writeSessionRecording();
  mRouter.reset();
  try {
    mRouter.reset(new BoardPnsRouter(
        mContext.board,
        BoardPnsRouter::Settings{
            mCurrentMode,
            mCurrentWidth,
            getViaSize(),
            getViaDrillDiameter(),
            getShoveIterationLimit(),
            getAllowDrcViolations(),
            mCornerMode90,
            mContext.pnsRecorder.isRecording(),
        }));
    return true;
  } catch (const Exception& e) {
    QMessageBox::critical(parentWidget(), tr("Error"), e.getMsg());
    return false;
  }
}

void BoardEditorState_RouteTrace::rebuildRouter() noexcept {
  if (!createRouter()) {
    emit requestLeavingState();
  }
}

void BoardEditorState_RouteTrace::updateRouterSettings() noexcept {
  if (!mRouter) return;

  mRouter->setSettings(BoardPnsRouter::Settings{
      mCurrentMode,
      mCurrentWidth,
      getViaSize(),
      getViaDrillDiameter(),
      getShoveIterationLimit(),
      getAllowDrcViolations(),
      mCornerMode90,
  });
}

uint BoardEditorState_RouteTrace::getShoveIterationLimit() const noexcept {
  const WorkspaceSettings& settings = mContext.workspace.getSettings();
  return qBound(1U, settings.pnsShoveIterationLimit.get(), 10000U);
}

bool BoardEditorState_RouteTrace::getAllowDrcViolations() const noexcept {
  return mContext.workspace.getSettings().pnsAllowDrcViolations.get();
}

BoardEditorState_RouteTrace::SnappedCursor
    BoardEditorState_RouteTrace::snapCursor() noexcept {
  SnappedCursor cursor{mCursorPos.mappedToGrid(getGridInterval()), 0};
  if ((!mSnapActive) || (!mRouter)) {
    return cursor;
  }

  // A drag snaps to the grid only. The router's own host excludes the line
  // being dragged from the snap search (PNS::TOOL_BASE::checkSnap, through
  // DRAGGER::GetOriginalLine); there is no such exclusion here, and without
  // it the cursor would snap onto the very object being dragged and pin it
  // where it started.
  if (mRouter->isDragging()) {
    return cursor;
  }

  // While routing, restrict the search the same way the draw trace tool
  // restricts its end anchor search.
  const bool routing = mRouter->isRoutingInProgress();
  const Layer* layerFilter = routing ? &getLayer() : nullptr;
  QSet<const NetSignal*> netFilter;
  if (routing) {
    netFilter.insert(mCurrentNetSignal);
  }
  const std::shared_ptr<QGraphicsItem> item = findItemAtPos(
      mCursorPos,
      FindFlag::Vias | FindFlag::NetPoints | FindFlag::NetLines |
          FindFlag::BoardPads | FindFlag::FootprintPads |
          FindFlag::AcceptNextGridMatch,
      layerFilter, netFilter);

  const BoardPnsSnapshot& snapshot = mRouter->getSnapshot();
  if (auto via = std::dynamic_pointer_cast<BGI_Via>(item)) {
    cursor.pos = via->getVia().getPosition();
    cursor.item = snapshot.getHostId(via->getVia());
  } else if (auto pad = std::dynamic_pointer_cast<BGI_Pad>(item)) {
    cursor.pos = pad->getPad().getPosition();
    cursor.item = snapshot.getHostId(pad->getPad());
  } else if (auto netPoint = std::dynamic_pointer_cast<BGI_NetPoint>(item)) {
    cursor.pos = netPoint->getNetPoint().getPosition();
    cursor.item = getHostIdOfNetPoint(netPoint->getNetPoint());
  } else if (auto netLine = std::dynamic_pointer_cast<BGI_NetLine>(item)) {
    cursor.pos = Toolbox::nearestPointOnLine(
        cursor.pos, netLine->getNetLine().getP1().getPosition(),
        netLine->getNetLine().getP2().getPosition());
    cursor.item = snapshot.getHostId(netLine->getNetLine());
  }
  return cursor;
}

quint64 BoardEditorState_RouteTrace::getHostIdOfNetPoint(
    const BI_NetPoint& netPoint) const noexcept {
  if (!mRouter) return 0;

  quint64 hostId = 0;
  foreach (const BI_NetLine* netLine, netPoint.getNetLines()) {
    const quint64 id = mRouter->getSnapshot().getHostId(*netLine);
    if ((id != 0) && ((hostId == 0) || (id < hostId))) {
      hostId = id;
    }
  }
  return hostId;
}

const Layer& BoardEditorState_RouteTrace::getStartLayer(
    quint64 hostId) const noexcept {
  // The rules of the draw trace tool (startPositioning() in
  // boardeditorstate_drawtrace.cpp): a trace hands over its layer, a
  // surface mount pad its solder layer, a via its start layer only when
  // the selected layer is not one it spans, and a through hole pad or
  // free space keeps the selected layer.
  const Layer* layer = mCurrentLayer;
  if (!mRouter) return *layer;
  const QSet<const Layer*> copperLayers = mContext.board.getCopperLayers();
  const BoardPnsHostRef ref = mRouter->getHostRef(hostId);
  if (ref.netLine) {
    layer = &ref.netLine->getLayer();
  } else if (ref.via) {
    const Via& via = ref.via->getVia();
    if ((!via.isOnLayer(*layer)) &&
        copperLayers.contains(&via.getStartLayer())) {
      layer = &via.getStartLayer();
    }
  } else if (ref.pad) {
    if (!ref.pad->getProperties().isTht()) {
      layer = &ref.pad->getSolderLayer();
    }
  }
  return copperLayers.contains(layer) ? *layer : *mCurrentLayer;
}

const NetSignal* BoardEditorState_RouteTrace::getNetSignalOfHostId(
    quint64 hostId) const noexcept {
  if (!mRouter) return nullptr;

  const BoardPnsHostRef ref = mRouter->getHostRef(hostId);
  if (ref.netLine) {
    return ref.netLine->getNetSegment().getNetSignal();
  } else if (ref.via) {
    return ref.via->getNetSegment().getNetSignal();
  } else if (ref.pad) {
    return ref.pad->getNetSignal();
  }
  return nullptr;
}

bool BoardEditorState_RouteTrace::isDraggable(quint64 hostId) const noexcept {
  if (!mRouter) return false;

  const BoardPnsHostRef ref = mRouter->getHostRef(hostId);
  return ref.netLine || ref.via;
}

bool BoardEditorState_RouteTrace::exceedsDragThreshold(
    const Point& pressPos, const Point& pos) const noexcept {
  // A multiplier of one is five screen pixels, which is the tolerance
  // findItemsAtPos() picks board objects with and the distance the view
  // takes as the beginning of a pan. The view converts it to scene
  // coordinates, so the threshold is the same distance on screen at every
  // zoom level.
  const QPainterPath area = mAdapter.fsmCalcPosWithTolerance(pressPos, 1);
  return !area.contains(pos.toPxQPointF());
}

void BoardEditorState_RouteTrace::startRouting(
    const SnappedCursor& cursor) noexcept {
  if (!mRouter) return;

  const Layer& layer = getStartLayer(cursor.item);
  BoardPnsRouter::StartResult result =
      mRouter->isStartingPointRoutable(cursor.pos, cursor.item, layer);
  if (result == BoardPnsRouter::StartResult::Ok) {
    result = mRouter->startRouting(cursor.pos, cursor.item, layer);
  }
  if (result != BoardPnsRouter::StartResult::Ok) {
    mAdapter.fsmSetStatusBarMessage(getStartResultMessage(result), 3000);
    return;
  }

  // The start item decided the layer, so the toolbar follows it.
  if (&layer != mCurrentLayer) {
    mCurrentLayer = &layer;
    makeLayerVisible(layer.getColorRole());
  }
  mCurrentNetSignal = getNetSignalOfHostId(cursor.item);
  mAdapter.fsmCrossProbe({mCurrentNetSignal});
  emit layerChanged(getLayer());

  if (mPreviewItems) {
    mPreviewItems->update(mRouter->getPreview());
  }
}

void BoardEditorState_RouteTrace::startDragging(
    const SnappedCursor& cursor) noexcept {
  if (!mRouter) return;

  // Free angle dragging has no control in this tool, so the router decides
  // between a corner drag and a segment drag from the clicked object and
  // from where on it the drag began, and keeps the 45 degree constraint.
  const BoardPnsRouter::StartResult result =
      mRouter->startDragging(cursor.pos, cursor.item, false);
  if (result != BoardPnsRouter::StartResult::Ok) {
    mAdapter.fsmSetStatusBarMessage(getStartResultMessage(result), 3000);
    return;
  }

  // The dragged object decides the layer, like the start item of a route.
  const Layer& layer = getStartLayer(cursor.item);
  if (&layer != mCurrentLayer) {
    mCurrentLayer = &layer;
    makeLayerVisible(layer.getColorRole());
  }
  mCurrentNetSignal = getNetSignalOfHostId(cursor.item);
  mAdapter.fsmCrossProbe({mCurrentNetSignal});
  emit layerChanged(getLayer());

  // A drag which has not moved yet has an empty frame, so this only takes
  // the last route's leftovers off the scene; the caller's move fills it.
  if (mPreviewItems) {
    mPreviewItems->update(mRouter->getPreview());
  }
}

void BoardEditorState_RouteTrace::abortDragging() noexcept {
  if ((!mRouter) || (!mRouter->isDragging())) return;

  // The board was never edited, so there is no commit to apply and no new
  // session to build: the router drops everything the drag speculatively
  // built and is idle again.
  mRouter->abortRouting();
  if (mPreviewItems) {
    mPreviewItems->clear();
  }
  mCurrentNetSignal = nullptr;
  mAdapter.fsmCrossProbe();
}

void BoardEditorState_RouteTrace::moveToCursor() noexcept {
  if ((!mRouter) || (!mRouter->isRoutingInProgress())) return;

  const SnappedCursor cursor = snapCursor();
  mRouter->moveTo(cursor.pos, cursor.item);

  if (mPreviewItems) {
    mPreviewItems->update(mRouter->getPreview());
  }
}

void BoardEditorState_RouteTrace::fixRoute(const SnappedCursor& cursor,
                                           bool forceFinish) noexcept {
  if ((!mRouter) || (!mRouter->isRoutingInProgress())) return;

  const BoardPnsRouter::FixOutcome outcome =
      mRouter->fixRoute(cursor.pos, cursor.item, forceFinish);

  if (mPreviewItems) {
    mPreviewItems->update(mRouter->getPreview());
  }

  if (outcome == BoardPnsRouter::FixOutcome::Finished) {
    const BoardPnsCommit commit = mRouter->getCommit();
    if (mPreviewItems) {
      mPreviewItems->clear();
    }
    mCurrentNetSignal = nullptr;
    mAdapter.fsmCrossProbe();
    applyCommit(commit);
    rebuildRouter();
  }
}

void BoardEditorState_RouteTrace::stopRouting() noexcept {
  if ((!mRouter) || (!mRouter->isRoutingInProgress())) return;

  const BoardPnsCommit commit = mRouter->stopRouting();
  if (mPreviewItems) {
    mPreviewItems->clear();
  }
  mCurrentNetSignal = nullptr;
  mAdapter.fsmCrossProbe();
  applyCommit(commit);
  rebuildRouter();
}

void BoardEditorState_RouteTrace::applyCommit(
    const BoardPnsCommit& commit) noexcept {
  if (commit.isEmpty()) {
    return;
  }

  try {
    mContext.undoStack.execCmd(
        new CmdBoardApplyPnsCommit(mContext.board, commit));  // can throw
  } catch (const Exception& e) {
    QMessageBox::critical(parentWidget(), tr("Error"), e.getMsg());
  }

  // Note: The planes do not have to be invalidated or rebuilt here. Adding,
  // removing and moving net lines and vias invalidates the affected layers
  // through the item lifecycle, and BoardEditor rebuilds them because it
  // listens to UndoStack::stateModified, which execCmd() emits with no
  // command group open. That also rebuilds the air wires.
}

void BoardEditorState_RouteTrace::writeSessionRecording() noexcept {
  if (!mRouter) return;

  const QString text = mRouter->takeRecording();
  if (text.isEmpty()) return;

  const QString boardName = *mContext.board.getName();
  try {
    mContext.pnsRecorder.writeSession(text, boardName);  // can throw
  } catch (const Exception& e) {
    // Recording must never get in the way of routing, so this is a status
    // bar line rather than a dialog.
    mAdapter.fsmSetStatusBarMessage(e.getMsg(), 5000);
  }
}

void BoardEditorState_RouteTrace::handleRecordingToggled() noexcept {
  if (mRouter && (!mRouter->isRoutingInProgress())) {
    rebuildRouter();
  }
}

QString BoardEditorState_RouteTrace::getStartResultMessage(
    BoardPnsRouter::StartResult result) noexcept {
  switch (result) {
    case BoardPnsRouter::StartResult::AlreadyRouting:
      return tr("A trace is already being routed.");
    case BoardPnsRouter::StartResult::UnknownStartItem:
      return tr("This object is not known to the router.");
    case BoardPnsRouter::StartResult::NotRoutable:
      return tr("No trace can be started here.");
    case BoardPnsRouter::StartResult::StartPointViolatesRules:
      return tr("There is not enough space here for a trace.");
    case BoardPnsRouter::StartResult::PlacerRefused:
      return tr("The router could not start a trace here.");
    case BoardPnsRouter::StartResult::NothingToDrag:
      return tr("There is nothing to drag here.");
    case BoardPnsRouter::StartResult::ComponentDragUnsupported:
      return tr("Footprints cannot be dragged with the router yet.");
    case BoardPnsRouter::StartResult::NotDraggable:
      return tr("This object cannot be dragged.");
    default:
      return tr("The router refused to start a trace here.");
  }
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb
