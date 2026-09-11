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
#include "cmdboardapplypnscommit.h"

#include "cmdboardnetsegmentadd.h"
#include "cmdboardnetsegmentaddelements.h"
#include "cmdboardsplitnetline.h"
#include "cmdboardviaedit.h"
#include "cmdcombineboardnetsegments.h"
#include "cmddeviceinstanceedit.h"
#include "cmdremoveboarditems.h"
#include "cmdsimplifyboardnetsegments.h"

#include <librepcb/core/project/board/board.h>
#include <librepcb/core/project/board/items/bi_device.h>
#include <librepcb/core/project/board/items/bi_netline.h>
#include <librepcb/core/project/board/items/bi_netpoint.h>
#include <librepcb/core/project/board/items/bi_netsegment.h>
#include <librepcb/core/project/board/items/bi_pad.h>
#include <librepcb/core/project/board/items/bi_via.h>
#include <librepcb/core/project/circuit/netsignal.h>
#include <librepcb/core/types/layer.h>
#include <librepcb/core/types/maskconfig.h>
#include <librepcb/core/utils/scopeguard.h>
#include <librepcb/core/utils/toolbox.h>

#include <QtCore>

#include <memory>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {
namespace editor {

/*******************************************************************************
 *  Constructors / Destructor
 ******************************************************************************/

CmdBoardApplyPnsCommit::CmdBoardApplyPnsCommit(
    Board& board, const BoardPnsCommit& commit) noexcept
  : UndoCommandGroup(tr("Route Trace")), mBoard(board), mCommit(commit) {
}

CmdBoardApplyPnsCommit::~CmdBoardApplyPnsCommit() noexcept {
}

/*******************************************************************************
 *  Inherited from UndoCommand
 ******************************************************************************/

bool CmdBoardApplyPnsCommit::performExecute() {
  // If an error occurs, undo all already executed child commands.
  auto undoScopeGuard = scopeGuard([&]() { performUndo(); });

  QVector<BoardPnsNewItem> additions = mCommit.added;
  QSet<BI_NetLine*> netLinesToRemove;
  QSet<BI_Via*> viasToRemove;

  // A footprint drag moves the devices whose pads were dragged, and it has
  // to happen before anything else: a pad is a net line anchor, the traces
  // below already end where the router put the pads, and every anchor is
  // resolved against the board as it is at that moment. The router names
  // one pad per entry and this list is already folded onto the devices,
  // which is KiCad's `processedFootprints` set.
  foreach (const BoardPnsMovedDevice& moved, mCommit.movedDevices) {
    if ((!moved.device) || (!moved.device->isAddedToBoard())) {
      continue;
    }
    std::unique_ptr<CmdDeviceInstanceEdit> cmd(
        new CmdDeviceInstanceEdit(*moved.device));
    cmd->setPosition(moved.device->getPosition() + moved.offset, true);
    execNewChildCmd(cmd.release());  // can throw
  }

  // A via the router only shoved keeps its identity, which matters because
  // its UUID is in the file format. Anything else the router updated is a
  // removal plus an addition, because a net line's endpoints cannot be
  // edited. The moves run before the removals, since removing anything from
  // a net segment re-creates the whole segment and the moved via has to
  // carry its new position into that copy.
  for (const auto& pair : mCommit.updated) {
    if (const BoardPnsNewVia* moved = getViaMove(pair.first, pair.second)) {
      BI_Via* via = pair.first.via;
      std::unique_ptr<CmdBoardViaEdit> cmd(new CmdBoardViaEdit(*via));
      cmd->setPosition(moved->position, true);
      rememberSegment(&via->getNetSegment());
      execNewChildCmd(cmd.release());  // can throw
    } else {
      collectRemoval(pair.first, netLinesToRemove, viasToRemove);
      additions.append(pair.second);
    }
  }
  for (const BoardPnsHostRef& ref : mCommit.removed) {
    collectRemoval(ref, netLinesToRemove, viasToRemove);
  }

  // Removals go through CmdRemoveBoardItems rather than through
  // CmdBoardNetSegmentRemoveElements, because a net segment has to stay
  // cohesive after every single command: removing a net line in the middle
  // of a trace splits its segment in two and leaves its former corner
  // dangling, which CmdRemoveBoardItems handles with the net segment
  // splitter and the plain remove command does not.
  if ((!netLinesToRemove.isEmpty()) || (!viasToRemove.isEmpty())) {
    CmdRemoveBoardItems* cmd = new CmdRemoveBoardItems(mBoard);
    cmd->removeNetLines(netLinesToRemove);
    cmd->removeVias(viasToRemove);
    execNewChildCmd(cmd);  // can throw
    foreach (BI_NetSegment* segment, cmd->getModifiedNetSegments()) {
      rememberSegment(segment);
    }
  }

  // Vias are held back until a net line ends on one, see PendingVia.
  for (const BoardPnsNewItem& item : additions) {
    const BoardPnsNewVia* via = item.getVia();
    if (via && via->startLayer && via->endLayer) {
      mPendingVias.append(PendingVia{via->position, via->diameter, via->drill,
                                     via->startLayer, via->endLayer, item.net,
                                     false});
    }
  }
  for (const BoardPnsNewItem& item : additions) {
    if (const BoardPnsNewSegment* segment = item.getSegment()) {
      addSegment(*segment, item.net);  // can throw
    }
  }
  for (PendingVia& via : mPendingVias) {
    if (!via.placed) {
      addOrphanVia(via);  // can throw
    }
  }

  // Collapse the corners the router's optimiser removed. Segments which were
  // dissolved into another one on the way are not on the board any more.
  QList<BI_NetSegment*> segments;
  foreach (BI_NetSegment* segment, mTouchedSegments) {
    if (segment->isAddedToBoard()) {
      segments.append(segment);
    }
  }
  if (!segments.isEmpty()) {
    execNewChildCmd(new CmdSimplifyBoardNetSegments(segments));  // can throw
  }

  undoScopeGuard.dismiss();  // No undo required.
  return getChildCount() > 0;
}

/*******************************************************************************
 *  Private Methods
 ******************************************************************************/

const BoardPnsNewVia* CmdBoardApplyPnsCommit::getViaMove(
    const BoardPnsHostRef& ref, const BoardPnsNewItem& item) noexcept {
  const BoardPnsNewVia* via = item.getVia();
  if (via && ref.via && ref.via->isAddedToBoard() &&
      (&ref.via->getVia().getStartLayer() == via->startLayer) &&
      (&ref.via->getVia().getEndLayer() == via->endLayer)) {
    return via;
  }
  return nullptr;
}

void CmdBoardApplyPnsCommit::collectRemoval(const BoardPnsHostRef& ref,
                                            QSet<BI_NetLine*>& netLines,
                                            QSet<BI_Via*>& vias) noexcept {
  // Pads, holes and copper graphics are obstacles the router never removes,
  // so a reference to one of them is silently ignored here.
  if (ref.netLine && ref.netLine->isAddedToBoard()) {
    netLines.insert(ref.netLine);
  } else if (ref.via && ref.via->isAddedToBoard()) {
    vias.insert(ref.via);
  }
}

void CmdBoardApplyPnsCommit::addSegment(const BoardPnsNewSegment& item,
                                        NetSignal* net) {
  if (!item.layer) {
    throw LogicError(__FILE__, __LINE__, "Routed trace without a layer.");
  }
  if (item.start == item.end) {
    return;  // A zero length trace is not representable and means nothing.
  }
  const Layer& layer = *item.layer;

  // Resolve both ends against the board as it is right now. Splitting an
  // existing net line for the first end happens before the second end is
  // looked up, so the second end can anchor on the split point.
  const FoundAnchor a = resolveAnchor(item.start, layer, net);
  const FoundAnchor b = resolveAnchor(item.end, layer, net);

  // Add to the net segment an anchor already belongs to, or open a new one.
  // A footprint pad with no trace on it yet has no net segment, so it can
  // join whichever segment the other end picks.
  BI_NetSegment* segment = a.segment ? a.segment : b.segment;
  if (!segment) {
    CmdBoardNetSegmentAdd* cmd = new CmdBoardNetSegmentAdd(mBoard, net);
    execNewChildCmd(cmd);  // can throw
    segment = cmd->getNetSegment();
    if (!segment) {
      throw LogicError(__FILE__, __LINE__);
    }
  }

  // Both ends and the net line go in as one command, because the net segment
  // has to be cohesive again by the time the command returns.
  std::unique_ptr<CmdBoardNetSegmentAddElements> cmdAdd(
      new CmdBoardNetSegmentAddElements(*segment));
  BI_NetPoint* temporaryStart = nullptr;
  BI_NetPoint* temporaryEnd = nullptr;
  BI_NetLineAnchor* p1 =
      materializeAnchor(item.start, a, *segment, *cmdAdd, temporaryStart);
  BI_NetLineAnchor* p2 =
      materializeAnchor(item.end, b, *segment, *cmdAdd, temporaryEnd);
  cmdAdd->addNetLine(*p1, *p2, layer, item.width);
  execNewChildCmd(cmdAdd.release());  // can throw
  rememberSegment(segment);

  // Only the second end can end up in a foreign net segment: the first end
  // is what chose the segment. The stand in net point is dissolved into the
  // foreign anchor, which is what merges the two segments. Everything of the
  // dissolved segment is re-created in the surviving one, so no pointer into
  // it may be used after this.
  if (temporaryEnd && b.anchor && b.segment && (b.segment != segment)) {
    execNewChildCmd(new CmdCombineBoardNetSegments(
        *segment, *temporaryEnd, *b.segment, *b.anchor));  // can throw
    rememberSegment(b.segment);
  }
}

void CmdBoardApplyPnsCommit::addOrphanVia(PendingVia& via) {
  CmdBoardNetSegmentAdd* cmdSegment =
      new CmdBoardNetSegmentAdd(mBoard, via.net);
  execNewChildCmd(cmdSegment);  // can throw
  BI_NetSegment* segment = cmdSegment->getNetSegment();
  if (!segment) {
    throw LogicError(__FILE__, __LINE__);
  }
  std::unique_ptr<CmdBoardNetSegmentAddElements> cmd(
      new CmdBoardNetSegmentAddElements(*segment));
  cmd->addVia(Via(Uuid::createRandom(), *via.startLayer, *via.endLayer,
                  via.position, std::make_optional(via.drill),
                  std::make_optional(via.diameter),
                  MaskConfig::automatic()));  // can throw
  execNewChildCmd(cmd.release());  // can throw
  via.placed = true;
  rememberSegment(segment);
}

CmdBoardApplyPnsCommit::FoundAnchor CmdBoardApplyPnsCommit::resolveAnchor(
    const Point& pos, const Layer& layer, const NetSignal* net) {
  const FoundAnchor found = findAnchorAt(pos, layer, net);
  if (found.anchor) {
    return found;
  }
  if (BI_NetLine* netLine = findNetLineAt(pos, layer, net)) {
    BI_NetSegment* segment = &netLine->getNetSegment();
    std::unique_ptr<CmdBoardSplitNetLine> cmd(
        new CmdBoardSplitNetLine(*netLine, pos));
    BI_NetPoint* splitPoint = cmd->getSplitPoint();
    execNewChildCmd(cmd.release());  // can throw
    rememberSegment(segment);
    return FoundAnchor{splitPoint, segment};
  }
  return FoundAnchor{};
}

BI_NetLineAnchor* CmdBoardApplyPnsCommit::materializeAnchor(
    const Point& pos, const FoundAnchor& found, BI_NetSegment& segment,
    CmdBoardNetSegmentAddElements& cmd, BI_NetPoint*& temporary) {
  // An anchor of the net segment we are adding to, or a footprint pad which
  // belongs to no segment at all, can be used as it is.
  if (found.anchor && ((found.segment == &segment) || (!found.segment))) {
    return found.anchor;
  }

  // Nothing there yet, so this is where a via the commit adds goes in, if
  // one lands here. It goes in with the net line, which is what keeps the
  // net segment cohesive.
  if (!found.anchor) {
    for (PendingVia& via : mPendingVias) {
      if ((!via.placed) && (via.position == pos)) {
        const Layer* startLayer = via.startLayer;
        const Layer* endLayer = via.endLayer;
        if (startLayer->getCopperNumber() > endLayer->getCopperNumber()) {
          std::swap(startLayer, endLayer);
        }
        BI_Via* item = cmd.addVia(
            Via(Uuid::createRandom(), *startLayer, *endLayer, via.position,
                std::make_optional(via.drill), std::make_optional(via.diameter),
                MaskConfig::automatic()));  // can throw
        via.placed = true;
        return item;
      }
    }
  }

  // Either free space, or an anchor of another net segment which the caller
  // merges this net point into afterwards.
  temporary = cmd.addNetPoint(pos);
  if (!temporary) {
    throw LogicError(__FILE__, __LINE__);
  }
  return temporary;
}

CmdBoardApplyPnsCommit::FoundAnchor CmdBoardApplyPnsCommit::findAnchorAt(
    const Point& pos, const Layer& layer,
    const NetSignal* net) const noexcept {
  // The board editor's tools use BoardGraphicsScene::findItemsAtPos() for
  // this, which needs a scene and therefore an open editor. A command has
  // neither, so the board itself is walked instead. That is not a shortcut:
  // the tool needs the fuzzy "next grid match" behaviour of the scene query,
  // while the router's endpoints are exact by construction, so an exact
  // comparison is both correct and cheaper here.
  foreach (const BI_Device* device, mBoard.getDeviceInstances()) {
    foreach (BI_Pad* pad, device->getPads()) {
      if ((pad->getNetSignal() == net) && (pad->getPosition() == pos) &&
          pad->isOnLayer(layer)) {
        return FoundAnchor{pad, pad->getNetSegmentOfLines()};
      }
    }
  }
  foreach (BI_NetSegment* segment, mBoard.getNetSegments()) {
    if (segment->getNetSignal() != net) {
      continue;
    }
    foreach (BI_Pad* pad, segment->getPads()) {
      if ((pad->getPosition() == pos) && pad->isOnLayer(layer)) {
        return FoundAnchor{pad, segment};
      }
    }
    foreach (BI_Via* via, segment->getVias()) {
      if ((via->getPosition() == pos) && via->getVia().isOnLayer(layer)) {
        return FoundAnchor{via, segment};
      }
    }
    foreach (BI_NetPoint* netPoint, segment->getNetPoints()) {
      const Layer* pointLayer = netPoint->getLayerOfTraces();
      if ((netPoint->getPosition() == pos) &&
          ((!pointLayer) || (pointLayer == &layer))) {
        return FoundAnchor{netPoint, segment};
      }
    }
  }
  return FoundAnchor{};
}

BI_NetLine* CmdBoardApplyPnsCommit::findNetLineAt(
    const Point& pos, const Layer& layer,
    const NetSignal* net) const noexcept {
  foreach (BI_NetSegment* segment, mBoard.getNetSegments()) {
    if (segment->getNetSignal() != net) {
      continue;
    }
    foreach (BI_NetLine* netLine, segment->getNetLines()) {
      if (&netLine->getLayer() != &layer) {
        continue;
      }
      const Point& p1 = netLine->getP1().getPosition();
      const Point& p2 = netLine->getP2().getPosition();
      if ((pos == p1) || (pos == p2)) {
        continue;  // An endpoint, not the interior.
      }
      if (*Toolbox::shortestDistanceBetweenPointAndLine(pos, p1, p2) ==
          Length(0)) {
        return netLine;
      }
    }
  }
  return nullptr;
}

void CmdBoardApplyPnsCommit::rememberSegment(BI_NetSegment* segment) noexcept {
  if (segment && (!mTouchedSegments.contains(segment))) {
    mTouchedSegments.append(segment);
  }
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace editor
}  // namespace librepcb
