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
#include "boardpnsrouter.h"

#include "../../exceptions.h"
#include "../../types/layer.h"
#include "board.h"

#include <librepcb/rust-core/ffi.h>

#include <QtCore>

#include <algorithm>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {

/*******************************************************************************
 *  Helpers
 ******************************************************************************/

static Point toPoint(const rs::PnsPoint& p) noexcept {
  return Point(Length(p.x), Length(p.y));
}

static rs::PnsPoint toFfi(const Point& p) noexcept {
  return rs::PnsPoint{p.getX().toNm(), p.getY().toNm()};
}

/**
 * Widen a length the router answered into a PositiveLength.
 *
 * The router never places zero width copper, but a degenerate answer must
 * not throw out of a getter, so it is widened to the smallest positive
 * value instead.
 */
static PositiveLength toPositiveLength(qint64 nm) noexcept {
  return PositiveLength(Length(std::max<qint64>(nm, 1)));
}

/**
 * Turn the router's negative-for-none clearance into an optional.
 */
static std::optional<Length> toClearance(qint64 nm) noexcept {
  if (nm < 0) {
    return std::nullopt;
  }
  return Length(nm);
}

static BoardPnsPreviewStyle toStyle(rs::PnsPreviewStyle style) noexcept {
  switch (style) {
    case rs::PnsPreviewStyle::Tail:
      return BoardPnsPreviewStyle::Tail;
    case rs::PnsPreviewStyle::Hover:
      return BoardPnsPreviewStyle::Hover;
    case rs::PnsPreviewStyle::SemiSolid:
      return BoardPnsPreviewStyle::SemiSolid;
    case rs::PnsPreviewStyle::Collision:
      return BoardPnsPreviewStyle::Collision;
    case rs::PnsPreviewStyle::Head:
    default:
      return BoardPnsPreviewStyle::Head;
  }
}

static BoardPnsRouter::StartResult toStartResult(
    rs::PnsStartResult result) noexcept {
  switch (result) {
    case rs::PnsStartResult::AlreadyRouting:
      return BoardPnsRouter::StartResult::AlreadyRouting;
    case rs::PnsStartResult::UnknownStartItem:
      return BoardPnsRouter::StartResult::UnknownStartItem;
    case rs::PnsStartResult::NotRoutable:
      return BoardPnsRouter::StartResult::NotRoutable;
    case rs::PnsStartResult::StartPointViolatesRules:
      return BoardPnsRouter::StartResult::StartPointViolatesRules;
    case rs::PnsStartResult::PlacerRefused:
      return BoardPnsRouter::StartResult::PlacerRefused;
    case rs::PnsStartResult::NothingToDrag:
      return BoardPnsRouter::StartResult::NothingToDrag;
    case rs::PnsStartResult::ComponentDragUnsupported:
      return BoardPnsRouter::StartResult::ComponentDragUnsupported;
    case rs::PnsStartResult::NotDraggable:
      return BoardPnsRouter::StartResult::NotDraggable;
    case rs::PnsStartResult::Ok:
    default:
      return BoardPnsRouter::StartResult::Ok;
  }
}

static BoardPnsRouter::FixOutcome toFixOutcome(
    rs::PnsFixOutcome outcome) noexcept {
  switch (outcome) {
    case rs::PnsFixOutcome::Finished:
      return BoardPnsRouter::FixOutcome::Finished;
    case rs::PnsFixOutcome::NotRouting:
      return BoardPnsRouter::FixOutcome::NotRouting;
    case rs::PnsFixOutcome::Continue:
    default:
      return BoardPnsRouter::FixOutcome::Continue;
  }
}

static rs::PnsRouterSettings toFfi(
    const BoardPnsRouter::Settings& settings) noexcept {
  quint8 mode = 2;  // pnsrouter::settings::RouterMode::Walkaround
  if (settings.mode == BoardPnsRouter::Mode::MarkObstacles) {
    mode = 0;
  } else if (settings.mode == BoardPnsRouter::Mode::Shove) {
    mode = 1;
  }
  return rs::PnsRouterSettings{
      mode,
      (*settings.traceWidth).toNm(),
      (*settings.viaDiameter).toNm(),
      (*settings.viaDrill).toNm(),
      settings.shoveIterationLimit,
      settings.recordSession,
  };
}

/*******************************************************************************
 *  Constructors / Destructor
 ******************************************************************************/

static RustHandle<rs::PnsRouter> construct(
    BoardPnsSnapshot& snapshot, const BoardPnsRouter::Settings& settings) {
  const rs::PnsRouterSettings ffiSettings = toFfi(settings);
  // The session consumes the snapshot, so this must be the last thing that
  // can fail before the handle owns it.
  if (auto obj = rs::ffi_pnsrouter_new(snapshot.release(), &ffiSettings)) {
    return RustHandle<rs::PnsRouter>(*obj, &rs::ffi_pnsrouter_delete);
  } else {
    throw RuntimeError(__FILE__, __LINE__,
                       QString("Failed to create PnsRouter"));
  }
}

BoardPnsRouter::BoardPnsRouter(const Board& board, const Settings& settings)
  : mSnapshot(new BoardPnsSnapshot(board)),  // can throw
    mInnerLayerCount(board.getInnerLayerCount()),
    mHandle(construct(*mSnapshot, settings)),  // can throw
    mPreview(),
    mCommit() {
}

BoardPnsRouter::~BoardPnsRouter() noexcept {
}

/*******************************************************************************
 *  Getters
 ******************************************************************************/

int BoardPnsRouter::getCopperLayerCount() const noexcept {
  return rs::ffi_pnsrouter_copper_layer_count(*mHandle);
}

bool BoardPnsRouter::isRoutingInProgress() const noexcept {
  return rs::ffi_pnsrouter_routing_in_progress(*mHandle);
}

const Layer* BoardPnsRouter::getCurrentLayer() const noexcept {
  return toLayer(rs::ffi_pnsrouter_current_layer(*mHandle));
}

bool BoardPnsRouter::isDragging() const noexcept {
  return rs::ffi_pnsrouter_is_dragging(*mHandle);
}

bool BoardPnsRouter::isPlacingVia() const noexcept {
  return rs::ffi_pnsrouter_placing_via(*mHandle);
}

const QVector<BoardPnsHostRef>& BoardPnsRouter::getHostRefs() const noexcept {
  return mSnapshot->getHostRefs();
}

BoardPnsHostRef BoardPnsRouter::getHostRef(quint64 hostId) const noexcept {
  const QVector<BoardPnsHostRef>& refs = mSnapshot->getHostRefs();
  if (hostId < static_cast<quint64>(refs.count())) {
    return refs.at(static_cast<int>(hostId));
  }
  // The router invented this item and the board has no object for it yet.
  return BoardPnsHostRef();
}

/*******************************************************************************
 *  General Methods
 ******************************************************************************/

QVector<quint64> BoardPnsRouter::hover(const Point& pos,
                                       const Layer* layer) noexcept {
  const int denseLayer = layer
      ? BoardPnsSnapshot::toDenseLayerIndex(*layer, mInnerLayerCount)
      : -1;
  const std::size_t count =
      rs::ffi_pnsrouter_hover(*mHandle, toFfi(pos), denseLayer);

  QVector<quint64> result;
  result.reserve(static_cast<int>(count));
  for (std::size_t i = 0; i < count; ++i) {
    result.append(rs::ffi_pnsrouter_hover_at(*mHandle, i));
  }
  return result;
}

BoardPnsRouter::StartResult BoardPnsRouter::isStartingPointRoutable(
    const Point& pos, quint64 startItem, const Layer& layer) const noexcept {
  const int denseLayer =
      BoardPnsSnapshot::toDenseLayerIndex(layer, mInnerLayerCount);
  return toStartResult(rs::ffi_pnsrouter_is_starting_point_routable(
      *mHandle, toFfi(pos), startItem, denseLayer));
}

BoardPnsRouter::StartResult BoardPnsRouter::startRouting(
    const Point& pos, quint64 startItem, const Layer& layer) noexcept {
  const int denseLayer =
      BoardPnsSnapshot::toDenseLayerIndex(layer, mInnerLayerCount);
  const rs::PnsStartResult result = rs::ffi_pnsrouter_start_routing(
      *mHandle, toFfi(pos), startItem, denseLayer);
  updatePreview();
  return toStartResult(result);
}

BoardPnsRouter::StartResult BoardPnsRouter::startDragging(
    const Point& pos, quint64 hostId, bool freeAngle) noexcept {
  // The router drags a pad as a component drag, which moves the whole
  // footprint, and this host applies no footprint move yet. Holes and
  // copper graphics are obstacles the router never moves.
  const BoardPnsHostRef ref = getHostRef(hostId);
  if (ref.pad) {
    return StartResult::ComponentDragUnsupported;
  } else if (ref.hole || ref.polygon) {
    return StartResult::NotDraggable;
  }
  const rs::PnsStartResult result =
      rs::ffi_pnsrouter_start_dragging(*mHandle, toFfi(pos), hostId, freeAngle);
  updatePreview();
  return toStartResult(result);
}

void BoardPnsRouter::moveTo(const Point& pos, quint64 endItem) noexcept {
  rs::ffi_pnsrouter_move_to(*mHandle, toFfi(pos), endItem);
  updatePreview();
}

BoardPnsRouter::FixOutcome BoardPnsRouter::fixRoute(
    const Point& pos, quint64 endItem, bool forceFinish) noexcept {
  const rs::PnsFixOutcome outcome =
      rs::ffi_pnsrouter_fix_route(*mHandle, toFfi(pos), endItem, forceFinish);
  updatePreview();
  if (outcome == rs::PnsFixOutcome::Finished) {
    updateCommit();
  }
  return toFixOutcome(outcome);
}

BoardPnsRouter::FixOutcome BoardPnsRouter::finish() noexcept {
  const rs::PnsFixOutcome outcome = rs::ffi_pnsrouter_finish(*mHandle);
  updatePreview();
  if (outcome == rs::PnsFixOutcome::Finished) {
    updateCommit();
  }
  return toFixOutcome(outcome);
}

std::optional<Point> BoardPnsRouter::undoLastSegment() noexcept {
  rs::PnsPoint at = {};
  if (!rs::ffi_pnsrouter_undo_last_segment(*mHandle, &at)) {
    return std::nullopt;
  }
  return toPoint(at);
}

bool BoardPnsRouter::switchLayer(const Layer& layer) noexcept {
  const int denseLayer =
      BoardPnsSnapshot::toDenseLayerIndex(layer, mInnerLayerCount);
  if (denseLayer < 0) {
    return false;  // Not a copper layer of this board.
  }
  return rs::ffi_pnsrouter_switch_layer(*mHandle, denseLayer);
}

bool BoardPnsRouter::toggleViaPlacement() noexcept {
  return rs::ffi_pnsrouter_toggle_via_placement(*mHandle);
}

void BoardPnsRouter::flipPosture() noexcept {
  rs::ffi_pnsrouter_flip_posture(*mHandle);
}

void BoardPnsRouter::toggleCornerMode() noexcept {
  rs::ffi_pnsrouter_toggle_corner_mode(*mHandle);
}

BoardPnsCommit BoardPnsRouter::stopRouting() noexcept {
  rs::ffi_pnsrouter_stop_routing(*mHandle);
  updatePreview();
  updateCommit();
  return mCommit;
}

void BoardPnsRouter::abortRouting() noexcept {
  rs::ffi_pnsrouter_abort_routing(*mHandle);
  updatePreview();
  updateCommit();
}

void BoardPnsRouter::setSettings(const Settings& settings) noexcept {
  const rs::PnsRouterSettings ffiSettings = toFfi(settings);
  rs::ffi_pnsrouter_set_settings(*mHandle, &ffiSettings);
}

QString BoardPnsRouter::takeRecording() noexcept {
  QString text;
  if (!rs::ffi_pnsrouter_take_recording(*mHandle, &text)) {
    return QString();
  }
  return text;
}

/*******************************************************************************
 *  Private Methods
 ******************************************************************************/

void BoardPnsRouter::updatePreview() noexcept {
  mPreview = BoardPnsPreview();

  const std::size_t itemCount = rs::ffi_pnsrouter_preview_item_count(*mHandle);
  mPreview.items.reserve(static_cast<int>(itemCount));
  for (std::size_t i = 0; i < itemCount; ++i) {
    rs::PnsPreviewItem raw = {};
    rs::ffi_pnsrouter_preview_item(*mHandle, i, &raw);

    BoardPnsPreviewItem item;
    item.path.reserve(static_cast<int>(raw.point_count));
    for (std::size_t p = 0; p < raw.point_count; ++p) {
      item.path.append(
          toPoint(rs::ffi_pnsrouter_preview_item_point(*mHandle, i, p)));
    }
    item.width = toPositiveLength(raw.width);
    item.layer = toLayer(raw.layer);
    item.net = toNetSignal(raw.net);
    item.style = toStyle(raw.style);
    item.clearance = toClearance(raw.clearance);
    mPreview.items.append(item);
  }

  if (rs::ffi_pnsrouter_preview_has_via(*mHandle)) {
    rs::PnsPreviewVia raw = {};
    rs::ffi_pnsrouter_preview_via(*mHandle, &raw);
    mPreview.via = toPreviewVia(raw);
  }

  const std::size_t viaCount =
      rs::ffi_pnsrouter_preview_fixed_via_count(*mHandle);
  mPreview.fixedVias.reserve(static_cast<int>(viaCount));
  for (std::size_t i = 0; i < viaCount; ++i) {
    rs::PnsPreviewVia raw = {};
    rs::ffi_pnsrouter_preview_fixed_via(*mHandle, i, &raw);
    mPreview.fixedVias.append(toPreviewVia(raw));
  }

  const std::size_t ratlineCount =
      rs::ffi_pnsrouter_preview_ratline_point_count(*mHandle);
  mPreview.ratline.reserve(static_cast<int>(ratlineCount));
  for (std::size_t i = 0; i < ratlineCount; ++i) {
    mPreview.ratline.append(
        toPoint(rs::ffi_pnsrouter_preview_ratline_point(*mHandle, i)));
  }

  const std::size_t violationCount =
      rs::ffi_pnsrouter_preview_violation_count(*mHandle);
  mPreview.violations.reserve(static_cast<int>(violationCount));
  for (std::size_t i = 0; i < violationCount; ++i) {
    rs::PnsViolationMarker raw = {};
    rs::ffi_pnsrouter_preview_violation(*mHandle, i, &raw);

    BoardPnsViolation violation;
    violation.host = getHostRef(raw.host_id);
    violation.clearance = Length(raw.clearance);
    violation.forcedLayer = toLayer(raw.forced_layer);
    violation.hideOriginal = raw.hide_original;
    mPreview.violations.append(violation);
  }

  const std::size_t hiddenCount =
      rs::ffi_pnsrouter_preview_hidden_count(*mHandle);
  mPreview.hidden.reserve(static_cast<int>(hiddenCount));
  for (std::size_t i = 0; i < hiddenCount; ++i) {
    mPreview.hidden.append(
        getHostRef(rs::ffi_pnsrouter_preview_hidden_at(*mHandle, i)));
  }
}

void BoardPnsRouter::updateCommit() noexcept {
  mCommit = BoardPnsCommit();

  const std::size_t removedCount =
      rs::ffi_pnsrouter_commit_removed_count(*mHandle);
  mCommit.removed.reserve(static_cast<int>(removedCount));
  for (std::size_t i = 0; i < removedCount; ++i) {
    mCommit.removed.append(
        getHostRef(rs::ffi_pnsrouter_commit_removed_at(*mHandle, i)));
  }

  const std::size_t addedCount = rs::ffi_pnsrouter_commit_added_count(*mHandle);
  mCommit.added.reserve(static_cast<int>(addedCount));
  for (std::size_t i = 0; i < addedCount; ++i) {
    rs::PnsNewItem raw = {};
    rs::ffi_pnsrouter_commit_added_at(*mHandle, i, &raw);
    mCommit.added.append(toNewItem(raw));
  }

  const std::size_t updatedCount =
      rs::ffi_pnsrouter_commit_updated_count(*mHandle);
  mCommit.updated.reserve(static_cast<int>(updatedCount));
  for (std::size_t i = 0; i < updatedCount; ++i) {
    uint64_t hostId = 0;
    rs::PnsNewItem raw = {};
    rs::ffi_pnsrouter_commit_updated_at(*mHandle, i, &hostId, &raw);
    mCommit.updated.append(qMakePair(getHostRef(hostId), toNewItem(raw)));
  }
}

BoardPnsPreviewVia BoardPnsRouter::toPreviewVia(
    const rs::PnsPreviewVia& via) const noexcept {
  BoardPnsPreviewVia result;
  result.position = toPoint(via.pos);
  result.diameter = toPositiveLength(via.diameter);
  result.drill = toPositiveLength(via.drill);
  result.startLayer = toLayer(via.layer_start);
  result.endLayer = toLayer(via.layer_end);
  result.net = toNetSignal(via.net);
  result.style = toStyle(via.style);
  result.clearance = toClearance(via.clearance);
  return result;
}

BoardPnsNewItem BoardPnsRouter::toNewItem(
    const rs::PnsNewItem& item) const noexcept {
  BoardPnsNewItem result;
  result.net = toNetSignal(item.net);
  result.source = getHostRef(item.source);
  if (item.kind == rs::PnsNewGeometryKind::Via) {
    result.kind = BoardPnsNewItem::Kind::Via;
    result.position = toPoint(item.pos);
    result.diameter = toPositiveLength(item.diameter);
    result.drill = toPositiveLength(item.drill);
    result.startLayer = toLayer(item.layer_start);
    result.endLayer = toLayer(item.layer_end);
  } else {
    result.kind = BoardPnsNewItem::Kind::Segment;
    result.start = toPoint(item.p1);
    result.end = toPoint(item.p2);
    result.width = toPositiveLength(item.width);
    // A trace occupies exactly one layer, so both ends of the router's
    // layer range are the same.
    result.layer = toLayer(item.layer_start);
  }
  return result;
}

const Layer* BoardPnsRouter::toLayer(int denseIndex) const noexcept {
  return BoardPnsSnapshot::fromDenseLayerIndex(denseIndex, mInnerLayerCount);
}

const NetSignal* BoardPnsRouter::toNetSignal(quint32 netNumber) const noexcept {
  return mSnapshot->getNetSignal(netNumber);
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace librepcb
