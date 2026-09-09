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
    mCurrentWidth(mContext.board.getDesignRules().getDefaultTraceWidth()),
    mCurrentViaDrill(std::nullopt),
    mCurrentViaSize(std::nullopt),
    mCursorPos(),
    mSnapActive(true),
    mCurrentNetSignal(nullptr) {
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

  if (mRouter && mRouter->isRoutingInProgress()) {
    // Keep whatever was fixed, like escape does, but do not build a new
    // session for a tool which is going away.
    const BoardPnsCommit commit = mRouter->stopRouting();
    mRouter.reset();
    applyCommit(commit);
  }
  mRouter.reset();
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
  if (mRouter && mRouter->isRoutingInProgress()) {
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
    case Qt::Key_V: {
      // Plain 'V' is the shortcut of the "add via" tool and never reaches a
      // tool state, so the via toggle is on SHIFT+V.
      if (e.modifiers.testFlag(Qt::ShiftModifier)) {
        mRouter->toggleViaPlacement();
        moveToCursor();
        return true;
      }
      break;
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
  const SnappedCursor cursor = snapCursor();

  if (mRouter->isRoutingInProgress()) {
    fixRoute(cursor, false);
  } else {
    startRouting(cursor);
  }
  return true;
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
    mRouter->flipPosture();
    moveToCursor();

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
  mRouter.reset();
  try {
    mRouter.reset(new BoardPnsRouter(
        mContext.board,
        BoardPnsRouter::Settings{
            BoardPnsRouter::Mode::Walkaround,
            mCurrentWidth,
            getViaSize(),
            getViaDrillDiameter(),
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
      BoardPnsRouter::Mode::Walkaround,
      mCurrentWidth,
      getViaSize(),
      getViaDrillDiameter(),
  });
}

BoardEditorState_RouteTrace::SnappedCursor
    BoardEditorState_RouteTrace::snapCursor() noexcept {
  SnappedCursor cursor{mCursorPos.mappedToGrid(getGridInterval()), 0};
  if ((!mSnapActive) || (!mRouter)) {
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

void BoardEditorState_RouteTrace::startRouting(
    const SnappedCursor& cursor) noexcept {
  if (!mRouter) return;

  const Layer& layer = *mCurrentLayer;
  BoardPnsRouter::StartResult result =
      mRouter->isStartingPointRoutable(cursor.pos, cursor.item, layer);
  if (result == BoardPnsRouter::StartResult::Ok) {
    result = mRouter->startRouting(cursor.pos, cursor.item, layer);
  }
  if (result != BoardPnsRouter::StartResult::Ok) {
    mAdapter.fsmSetStatusBarMessage(getStartResultMessage(result), 3000);
    return;
  }

  mCurrentNetSignal = getNetSignalOfHostId(cursor.item);
  mAdapter.fsmCrossProbe({mCurrentNetSignal});
  emit layerChanged(getLayer());

  if (mPreviewItems) {
    mPreviewItems->update(mRouter->getPreview());
  }
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
    // Copy, the session which owns it is replaced below.
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
  if (commit.removed.isEmpty() && commit.added.isEmpty() &&
      commit.updated.isEmpty()) {
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
    default:
      return tr("The router refused to start a trace here.");
  }
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb
